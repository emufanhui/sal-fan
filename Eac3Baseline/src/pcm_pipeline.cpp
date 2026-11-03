#include "commands.h"

#include "bridge_meter.h"
#include "crossover_stage.h"
#include "layout_mix.h"
#include "mat_capture_client.h"
#include "multi_endpoint_renderer.h"
#include "nsp_ring_reader.h"
#include "omniphony_pcm_bridge.h"
#include "speaker_layout.h"
#include "utf8.h"

#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dolby {
namespace {

constexpr DWORD kPcm714ChannelMask = 0x0002D63F;

// Arms the optional crossover on a queue that is about to run.
//
// No config file is the normal case and means bypass, so that returns nothing
// to print and changes no behaviour. Anything else - DLL absent, config
// rejected, chunk size or channel count that does not match this stream - comes
// back as one line, because a crossover that silently did not engage is the
// worst possible failure here.
void ArmCrossover(InterleavedPcmQueue& queue, CrossoverStage& stage,
                  const std::size_t sampleRate) {
    const std::filesystem::path configPath = ResolveCrossoverConfigPath();
    if (configPath.empty()) return;
    queue.SetCrossover(&stage);
    if (stage.Load(ResolveEngineDirectory(), configPath, sampleRate)) {
        queue.ConfigureCrossover(sampleRate);
    }
    // UTF-8 on std::cout for the same reason as the endpoint names: a wide
    // stream under the default C locale drops everything after the first
    // non-ASCII character, which is exactly where a Chinese path would be.
    std::wcout.flush();
    std::cout << "  " << Utf8(stage.Diagnostic()) << "\n" << std::flush;
}
void ValidatePcm714Format(const MAT_CAPTURE_READ_HEADER& header) {
    if (header.Version < MAT_CAPTURE_PROTOCOL_VERSION) {
        throw std::runtime_error(
            "PCM 7.1.4 requires capture driver protocol v3; install the rebuilt SysVAD driver");
    }
    if (!IsEqualGUID(header.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        header.SampleRate != 48'000 || header.Channels != 12 ||
        header.ChannelMask != kPcm714ChannelMask ||
        (header.BitsPerSample != 16 && header.BitsPerSample != 32) ||
        header.BlockAlign != header.Channels * (header.BitsPerSample / 8)) {
        throw std::runtime_error(
            "Capture endpoint is not emitting PCM 7.1.4 at 48 kHz with the expected channel mask");
    }
}

std::vector<std::int16_t> ConvertPcm714(
    const BYTE* bytes,
    const std::size_t byteCount,
    const MAT_CAPTURE_READ_HEADER& format,
    const SpeakerLayout& layout,
    const Canonical714Mix& mix,
    std::vector<BYTE>& carry) {
    carry.insert(carry.end(), bytes, bytes + byteCount);
    const std::size_t frameBytes = format.BlockAlign;
    const std::size_t frames = carry.size() / frameBytes;
    if (frames == 0) return {};

    std::vector<std::int16_t> output(frames * layout.speakers.size());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const BYTE* sourceFrame = carry.data() + frame * frameBytes;
        std::array<std::int16_t, 12> sourceSamples{};
        for (std::size_t source = 0; source < sourceSamples.size(); ++source) {
            std::int16_t sample = 0;
            if (format.BitsPerSample == 16) {
                std::memcpy(&sample, sourceFrame + source * sizeof(sample), sizeof(sample));
            } else {
                std::int32_t wideSample = 0;
                std::memcpy(&wideSample, sourceFrame + source * sizeof(wideSample),
                            sizeof(wideSample));
                sample = static_cast<std::int16_t>(wideSample >> 16);
            }
            sourceSamples[source] = sample;
        }
        MixCanonical714Frame(
            sourceSamples,
            mix,
            std::span<std::int16_t>(
                output.data() + frame * layout.speakers.size(),
                layout.speakers.size()));
    }

    const std::size_t consumed = frames * frameBytes;
    carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(consumed));
    return output;
}

} // namespace

void PlayLivePcmLayout(const double seconds,
                       const std::filesystem::path& layoutPath,
                       const double gain,
                       const DWORD prebufferMilliseconds,
                       const RendererLatencyMode latencyMode) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Live PCM layout gain must be between 0 and 1");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    const Canonical714Mix mix = BuildCanonical714Mix(layout);
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());
    BridgeMeterPublisher meters(layout, BridgeMeterMode::Pcm);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    CrossoverStage crossover;
    InterleavedPcmQueue queue(layout.speakers.size());
    ArmCrossover(queue, crossover, 48'000);

    constexpr std::size_t requestPayloadBytes = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + requestPayloadBytes);
    std::vector<BYTE> carry;
    carry.reserve(4096);
    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t ringBytes = 0;
    std::uint64_t pcmFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool haveSequence = false;
    bool acceptingInput = true;
    bool haveFormat = false;

    std::wcout << L"Live native PCM 7.1.4 layout: " << layout.name << L"\n"
               << L"  speakers=" << layout.speakers.size()
               << L", outputs=" << layout.outputs.size() << L"\n";

    for (const EndpointRenderStats& output : renderer.Stats()) {

        std::wcout << L"  " << output.routeName << L": " << output.endpointName
                   << (output.isMaster ? L" [master]" : L"")
                   << L", buffer=" << std::fixed << std::setprecision(2)
                   << output.bufferMilliseconds << L" ms, period="
                   << output.selectedPeriodFrames << L" frames"
                   << (output.lowLatencyApi ? L" [IAudioClient3]" : L"") << L"\n";
    }
    std::wcout << L"  decoder=none, input=PCM 7.1.4/48 kHz, gain=" << std::fixed
               << std::setprecision(2) << gain << L", prebuffer="
               << prebufferMilliseconds << L" ms, latency="
               << RendererLatencyModeName(latencyMode) << L", effective="
               << std::setprecision(2) << prebufferFrames / 48.0 << L" ms\n" << std::flush;

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    const bool unlimited = seconds == 0.0;
    auto lastPayloadTime = startTime;
    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (acceptingInput) {
                const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
                if (read.header.PayloadBytes != 0) {
                    ValidatePcm714Format(read.header);
                    if (read.header.FormatChanges != 0) {
                        throw std::runtime_error("Capture format changed during PCM playback");
                    }
                    haveFormat = true;
                    if (haveSequence && read.header.FirstByteSequence != expectedSequence) {
                        sequenceGaps += read.header.FirstByteSequence > expectedSequence
                            ? read.header.FirstByteSequence - expectedSequence
                            : expectedSequence - read.header.FirstByteSequence;
                        carry.clear();
                    }
                    haveSequence = true;
                    expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
                    ringBytes += read.header.PayloadBytes;
                    lastPayloadTime = now;
                    auto pcm = ConvertPcm714(read.payload, read.header.PayloadBytes,
                                             read.header, layout, mix, carry);
                    if (!pcm.empty()) {
                        pcmFrames += pcm.size() / layout.speakers.size();
                        meters.Update(pcm);
                        queue.Append(std::move(pcm));
                        maximumQueuedFrames = std::max(
                            maximumQueuedFrames, renderer.MinimumFramesAvailable(queue));
                    }
                }
                if (!unlimited && now >= deadline) acceptingInput = false;
            }

            const std::uint64_t queuedForAll = renderer.MinimumFramesAvailable(queue);
            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames || (!acceptingInput && queuedForAll != 0))) {
                renderer.PrimeAndStart(queue);
            }

            if (renderer.IsStarted()) {
                const bool activeInput = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);
                renderer.Service(queue, activeInput, 2);
                renderer.DiscardConsumed(queue);
            } else {
                Sleep(2);
            }

            if (!acceptingInput && (!renderer.IsStarted() || renderer.IsDrained(queue))) break;
        }
    } catch (...) {
        renderer.Stop();
        throw;
    }

    if (!haveFormat || !renderer.IsStarted()) {
        throw std::runtime_error("No PCM 7.1.4 frames arrived before the live timeout");
    }
    renderer.Stop();
    const MAT_CAPTURE_STATS ringStats = QueryMatCaptureStats(device.Get());
    std::wcout << L"Live native PCM layout playback complete\n"
               << L"  ring bytes=" << ringBytes << L", driver dropped="
               << ringStats.DroppedBytes << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"  PCM frames=" << pcmFrames << L", trailing bytes=" << carry.size()
               << L", max PCM queue=" << maximumQueuedFrames << L" frames\n";
    PrintEndpointRenderStats(renderer.Stats());
}

void PlayLiveOmniphonyPcm(const double seconds,
                          const std::filesystem::path& layoutPath,
                          const double gain,
                          const DWORD prebufferMilliseconds,
                          const RendererLatencyMode latencyMode) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Omniphony PCM gain must be between 0 and 1");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    if (layout.speakers.size() != OmniphonyPcmBridge::kChannels) {
        throw std::runtime_error(
            "Omniphony PCM bridge requires a 12-speaker 7.1.4 layout");
    }

    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    CrossoverStage crossover;
    InterleavedPcmQueue queue(layout.speakers.size());
    ArmCrossover(queue, crossover, 48'000);
    OmniphonyPcmBridge bridge;

    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());

    std::uint64_t pcmFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool acceptingInput = true;
    bool receivedInput = false;

    std::wcout << L"Live Omniphony PCM 7.1.4: " << layout.name << L"\n"
               << L"  speakers=" << layout.speakers.size()
               << L", outputs=" << layout.outputs.size() << L"\n";
    for (const EndpointRenderStats& output : renderer.Stats()) {
        std::wcout << L"  " << output.routeName << L": " << output.endpointName
                   << (output.isMaster ? L" [master]" : L"")
                   << L", buffer=" << std::fixed << std::setprecision(2)
                   << output.bufferMilliseconds << L" ms, period="
                   << output.selectedPeriodFrames << L" frames"
                   << (output.lowLatencyApi ? L" [IAudioClient3]" : L"") << L"\n";
    }

    std::wcout << L"  input=Omniphony float32 PCM, 48 kHz, 12 channels\n"
               << L"  pipe=\\\\.\\pipe\\SpatialAudioLab.Pcm714\n"
               << L"  gain=" << std::fixed << std::setprecision(2) << gain
               << L", prebuffer=" << prebufferMilliseconds
               << L" ms, latency=" << RendererLatencyModeName(latencyMode)
               << L", effective=" << std::setprecision(2)
               << prebufferFrames / 48.0 << L" ms\n"
               << L"Waiting for Omniphony PCM pipe client...\n"
               << std::flush;

    bridge.WaitForConnection();

    std::wcout << L"Omniphony PCM pipe connected\n" << std::flush;

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    const bool unlimited = seconds == 0.0;
    auto lastPayloadTime = startTime;

    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();

            if (acceptingInput) {
                const bool readAny = bridge.ReadAvailable(queue);

                if (readAny) {
                    receivedInput = true;
                    lastPayloadTime = now;

                    const std::uint64_t queuedFrames =
                        renderer.MinimumFramesAvailable(queue);
                    maximumQueuedFrames =
                        std::max(maximumQueuedFrames, queuedFrames);

                    pcmFrames = std::max(
                        pcmFrames,
                        queue.FramesAvailable(0));
                }

                if (bridge.IsClientClosed()) {
                    acceptingInput = false;
                    std::wcout << L"Omniphony PCM pipe client closed\n"
                               << std::flush;
                }

                if (!unlimited && now >= deadline) {
                    acceptingInput = false;
                }
            }

            const std::uint64_t queuedForAll =
                renderer.MinimumFramesAvailable(queue);

            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames ||
                 (!acceptingInput && queuedForAll != 0))) {
                renderer.PrimeAndStart(queue);
            }

            if (renderer.IsStarted()) {
                const bool activeInput = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);

                renderer.Service(queue, activeInput, 2);
                renderer.DiscardConsumed(queue);
            } else {
                Sleep(2);
            }

            if (!acceptingInput &&
                (!renderer.IsStarted() || renderer.IsDrained(queue))) {
                break;
            }

            if (!bridge.IsConnected() && !acceptingInput &&
                (!renderer.IsStarted() || renderer.IsDrained(queue))) {
                break;
            }
        }
    } catch (...) {
        renderer.Stop();
        throw;
    }

    if (!receivedInput || !renderer.IsStarted()) {
        throw std::runtime_error(
            "No Omniphony PCM 7.1.4 frames arrived before the live timeout");
    }

    renderer.Stop();

    std::wcout << L"Live Omniphony PCM playback complete\n"
               << L"  PCM frames observed=" << pcmFrames
               << L", max PCM queue=" << maximumQueuedFrames << L" frames\n";

    PrintEndpointRenderStats(renderer.Stats());
}

void PlayLiveNspLayout(const double seconds,
                       const std::filesystem::path& layoutPath,
                       const double gain,
                       const DWORD prebufferMilliseconds,
                       const RendererLatencyMode latencyMode,
                       const std::wstring& ringName) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("NSP layout gain must be between 0 and 1");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    if (layout.speakers.size() > NspRingReader::kChannels) {
        throw std::runtime_error(
            "NSP bridge cannot feed a layout with more than 12 speakers");
    }

    // The ring always carries the canonical 7.1.4 bed, so a smaller layout is
    // fed through the same azimuth-based matrix the decoder path already uses:
    // channels the layout does not have (SL/SR on a 5.1.x bed, TBL/TBR on a
    // x.1.2 bed) fold into their neighbours by angle instead of being dropped.
    // An exact 12-speaker layout skips the matrix entirely.
    const std::size_t ringChannels = NspRingReader::kChannels;
    const std::size_t layoutChannels = layout.speakers.size();
    const bool downmix = layoutChannels != ringChannels;
    const Canonical714Mix mix =
        downmix ? BuildCanonical714Mix(layout) : Canonical714Mix{};
    std::vector<float> mixed;
    std::vector<std::int16_t> metersPcm;

    // Open the endpoints before waiting on the ring. A layout that does not
    // match the hardware then fails immediately, instead of surfacing only
    // after the ring wait has already timed out.
    BridgeMeterPublisher meters(layout, BridgeMeterMode::Pcm);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    CrossoverStage crossover;
    InterleavedPcmQueue queue(layout.speakers.size());
    ArmCrossover(queue, crossover, 48'000);

    // NSP is usually started after the renderer, so wait for the ring instead
    // of failing on the first attempt.
    auto ring = std::make_unique<NspRingReader>(ringName);
    if (!ring->IsOpen()) {
        std::wcout << L"Waiting for NSP ring " << ringName << L" ...\n" << std::flush;
        const auto giveUp =
            std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!ring->IsOpen() && std::chrono::steady_clock::now() < giveUp) {
            Sleep(200);
            ring = std::make_unique<NspRingReader>(ringName);
        }
    }
    if (!ring->IsOpen()) {
        throw std::runtime_error(
            "NSP shared-memory ring did not appear; start NSP with the "
            "shared-memory sink as its output target");
    }

    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());

    std::uint64_t pcmFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool acceptingInput = true;
    bool receivedInput = false;

    // UTF-8 on std::cout, not std::wcout: endpoint names are localised and
    // a wide stream under the default C locale fails on the first non-ASCII
    // character, silently dropping the rest of the run including the stats.
    // Flushed per line because this is the only progress the operator sees.
    std::wcout.flush();
    std::cout << "Live NSP 7.1.4: " << Utf8(layout.name) << "\n"
              << "  speakers=" << layout.speakers.size()
              << ", outputs=" << layout.outputs.size()
              << (downmix ? ", downmix=12->" + std::to_string(layoutChannels) +
                                " (matrix)"
                          : ", downmix=none")
              << "\n" << std::flush;
    for (const EndpointRenderStats& output : renderer.Stats()) {
        std::cout << "  " << Utf8(output.routeName) << ": "
                  << Utf8(output.endpointName)
                  << (output.isMaster ? " [master]" : "")
                  << ", buffer=" << std::fixed << std::setprecision(2)
                  << output.bufferMilliseconds << " ms, period="
                  << output.selectedPeriodFrames << " frames"
                  << (output.lowLatencyApi ? " [IAudioClient3]" : "") << "\n"
                  << std::flush;
    }
    std::cout << "  ring=" << Utf8(ringName)
              << ", input=NSP 7.1.4 48 kHz, decoder=none\n"
              << "  gain=" << std::fixed << std::setprecision(2) << gain
              << ", prebuffer=" << prebufferMilliseconds
              << " ms, latency=" << Utf8(RendererLatencyModeName(latencyMode))
              << ", effective=" << std::setprecision(2)
              << prebufferFrames / 48.0 << " ms\n" << std::flush;

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    const bool unlimited = seconds == 0.0;
    auto lastPayloadTime = startTime;

    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();

            if (acceptingInput) {
                // Read the ring once, as float: the crossover runs before the
                // block is quantised, and a ring read cannot be repeated for a
                // second consumer.
                auto block = ring->ReadFramesFloat(1024);
                if (!block.empty()) {
                    // The ring is 12 channels wide whatever the layout is.
                    const std::size_t blockFrames = block.size() / ringChannels;
                    receivedInput = true;
                    lastPayloadTime = now;
                    pcmFrames += blockFrames;

                    if (downmix) {
                        mixed.resize(blockFrames * layoutChannels);
                        for (std::size_t frame = 0; frame < blockFrames; ++frame) {
                            std::array<float, NspRingReader::kChannels> source{};
                            for (std::size_t channel = 0; channel < ringChannels; ++channel) {
                                source[channel] = block[frame * ringChannels + channel];
                            }
                            MixCanonical714Frame(
                                source, mix,
                                std::span<float>(mixed.data() + frame * layoutChannels,
                                                 layoutChannels));
                        }
                    }

                    const float* interleaved = downmix ? mixed.data() : block.data();
                    const std::size_t sampleCount = blockFrames * layoutChannels;

                    metersPcm.resize(sampleCount);
                    for (std::size_t i = 0; i < sampleCount; ++i) {
                        const float sample = interleaved[i];
                        const double value = std::isfinite(sample)
                                                 ? std::clamp(static_cast<double>(sample), -1.0, 1.0)
                                                 : 0.0;
                        metersPcm[i] = static_cast<std::int16_t>(std::lround(value * 32767.0));
                    }
                    meters.Update(metersPcm);
                    queue.AppendInterleaved(interleaved, blockFrames);
                    maximumQueuedFrames = std::max(
                        maximumQueuedFrames, renderer.MinimumFramesAvailable(queue));
                } else {
                    ring->WaitForData(2);
                }

                if (!unlimited && now >= deadline) acceptingInput = false;

                // The studio owns this engine's lifetime, so a quiet ring is not
                // a stop condition: it just means nothing is playing right now
                // (game alt-tabbed, exited, or loading). Stopping here used to
                // kill the bridge five seconds after a game went quiet and
                // forced a manual restart; now only the studio ends the run.
            }

            const std::uint64_t queuedForAll = renderer.MinimumFramesAvailable(queue);
            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames ||
                 (!acceptingInput && queuedForAll != 0))) {
                renderer.PrimeAndStart(queue);
            }

            if (renderer.IsStarted()) {
                const bool activeInput = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);
                renderer.Service(queue, activeInput, 2);
                renderer.DiscardConsumed(queue);
            } else {
                Sleep(2);
            }

            if (!acceptingInput &&
                (!renderer.IsStarted() || renderer.IsDrained(queue))) {
                break;
            }
        }
    } catch (...) {
        renderer.Stop();
        throw;
    }

    if (!receivedInput || !renderer.IsStarted()) {
        throw std::runtime_error("No NSP 7.1.4 frames arrived before the live timeout");
    }

    renderer.Stop();

    std::cout << "Live NSP playback complete\n"
              << "  PCM frames=" << pcmFrames
              << ", max PCM queue=" << maximumQueuedFrames << " frames\n"
              << "  ring dropped=" << ring->Dropped()
              << ", pushes=" << ring->Pushes() << "\n" << std::flush;
    PrintEndpointRenderStats(renderer.Stats());
}

namespace {

std::int16_t LoopbackSampleToInt16(const float sample) {
    if (!std::isfinite(sample)) {
        return 0;
    }
    const double value = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<std::int16_t>(std::lround(value * 32767.0));
}

} // namespace

void PlayLiveProcessLayout(const double seconds,
                           const DWORD processId,
                           const std::filesystem::path& layoutPath,
                           const double gain,
                           const DWORD prebufferMilliseconds,
                           const RendererLatencyMode latencyMode) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Process loopback gain must be between 0 and 1");
    }

    // What the capture is asked for. This stays the full 7.1.4 bed whatever the
    // layout is: the loopback stream is whatever the source process submits to
    // its own endpoint, so asking for fewer channels would only get a different
    // conversion of the same bed. A smaller layout is downmixed after capture
    // instead, which keeps one capture format for every layout.
    constexpr std::size_t channels = 12;
    constexpr DWORD channelMask = 0x0002D63F; // 7.1 plus the four tops

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    if (layout.speakers.size() > channels) {
        throw std::runtime_error(
            "Process loopback bridge cannot feed a layout with more than 12 speakers");
    }

    // Same azimuth matrix the decoder path uses: channels the layout lacks fold
    // into their neighbours by angle rather than being dropped.
    const std::size_t layoutChannels = layout.speakers.size();
    const bool downmix = layoutChannels != channels;
    const Canonical714Mix mix =
        downmix ? BuildCanonical714Mix(layout) : Canonical714Mix{};
    std::vector<float> mixed;
    std::vector<std::int16_t> metersPcm;

    BridgeMeterPublisher meters(layout, BridgeMeterMode::Pcm);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    CrossoverStage crossover;
    InterleavedPcmQueue queue(layout.speakers.size());
    ArmCrossover(queue, crossover, 48'000);

    ComPtr<IAudioClient> client = ActivateProcessLoopback(processId);

    WAVEFORMATEXTENSIBLE requested{};
    requested.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    requested.Format.nChannels = static_cast<WORD>(channels);
    requested.Format.nSamplesPerSec = 48'000;
    requested.Format.wBitsPerSample = 32;
    requested.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    requested.Format.nAvgBytesPerSec =
        requested.Format.nSamplesPerSec * requested.Format.nBlockAlign;
    requested.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    requested.Samples.wValidBitsPerSample = 32;
    requested.dwChannelMask = channelMask;
    requested.SubFormat = kIeeeFloat;

    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    ThrowIfFailed(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                     &requested.Format, nullptr),
                  "Initialize process loopback for the live bridge");

    ComPtr<IAudioCaptureClient> capture;
    ThrowIfFailed(client->GetService(IID_PPV_ARGS(&capture)),
                  "Get process loopback capture client");

    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());

    // UTF-8 on std::cout, not std::wcout: endpoint names are localised and a
    // wide stream under the default C locale fails on the first non-ASCII
    // character, silently dropping the rest of the run including the stats.
    std::wcout.flush();
    std::cout << "Live process loopback 7.1.4: " << Utf8(layout.name) << "\n"
              << "  speakers=" << layout.speakers.size()
              << ", outputs=" << layout.outputs.size() << "\n"
              << "  source PID=" << processId << "\n" << std::flush;
    for (const EndpointRenderStats& output : renderer.Stats()) {
        std::cout << "  " << Utf8(output.routeName) << ": "
                  << Utf8(output.endpointName)
                  << (output.isMaster ? " [master]" : "")
                  << ", buffer=" << std::fixed << std::setprecision(2)
                  << output.bufferMilliseconds << " ms, period="
                  << output.selectedPeriodFrames << " frames"
                  << (output.lowLatencyApi ? " [IAudioClient3]" : "") << "\n"
                  << std::flush;
    }
    std::cout << "  capture=12ch float32 48 kHz, mask=0x2D63F"
              << ", gain=" << std::fixed << std::setprecision(2) << gain
              << ", prebuffer=" << prebufferMilliseconds
              << " ms, effective=" << prebufferFrames / 48.0 << " ms\n"
              << std::flush;

    ThrowIfFailed(client->Start(), "Start process loopback");

    std::uint64_t pcmFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool acceptingInput = true;
    bool receivedInput = false;

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    const bool unlimited = seconds == 0.0;
    auto lastPayloadTime = startTime;

    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();

            if (acceptingInput) {
                bool readAny = false;
                UINT32 packetFrames = 0;
                HRESULT next = capture->GetNextPacketSize(&packetFrames);
                while (SUCCEEDED(next) && packetFrames != 0) {
                    BYTE* data = nullptr;
                    DWORD bufferFlags = 0;
                    UINT64 devicePosition = 0;
                    UINT64 qpcPosition = 0;
                    const HRESULT buffer =
                        capture->GetBuffer(&data, &packetFrames, &bufferFlags,
                                           &devicePosition, &qpcPosition);
                    if (FAILED(buffer)) {
                        break;
                    }

                    const bool silent =
                        (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    if (!silent && data != nullptr) {
                        const auto* source = reinterpret_cast<const float*>(data);
                        const std::size_t frames =
                            static_cast<std::size_t>(packetFrames);

                        if (downmix) {
                            mixed.resize(frames * layoutChannels);
                            for (std::size_t frame = 0; frame < frames; ++frame) {
                                std::array<float, channels> bed{};
                                for (std::size_t channel = 0; channel < channels; ++channel) {
                                    bed[channel] = source[frame * channels + channel];
                                }
                                MixCanonical714Frame(
                                    bed, mix,
                                    std::span<float>(
                                        mixed.data() + frame * layoutChannels,
                                        layoutChannels));
                            }
                        }

                        const float* interleaved = downmix ? mixed.data() : source;
                        const std::size_t meterSamples = frames * layoutChannels;
                        metersPcm.resize(meterSamples);
                        for (std::size_t i = 0; i < meterSamples; ++i) {
                            metersPcm[i] = LoopbackSampleToInt16(interleaved[i]);
                        }

                        pcmFrames += packetFrames;
                        meters.Update(metersPcm);
                        // `metersPcm` above only feeds the meters: the queue
                        // takes the float block so the crossover, when it is
                        // armed, runs before any quantisation to int16.
                        queue.AppendInterleaved(interleaved, packetFrames);
                        readAny = true;
                    }

                    capture->ReleaseBuffer(packetFrames);
                    next = capture->GetNextPacketSize(&packetFrames);
                }

                if (readAny) {
                    receivedInput = true;
                    lastPayloadTime = now;
                    maximumQueuedFrames = std::max(
                        maximumQueuedFrames,
                        renderer.MinimumFramesAvailable(queue));
                } else {
                    Sleep(2);
                }

                if (!unlimited && now >= deadline) {
                    acceptingInput = false;
                }

                // As with the NSP ring, a quiet source is not a stop condition:
                // the studio owns this run, and silence only means nothing is
                // playing right now.
            }

            const std::uint64_t queuedForAll =
                renderer.MinimumFramesAvailable(queue);
            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames ||
                 (!acceptingInput && queuedForAll != 0))) {
                renderer.PrimeAndStart(queue);
            }

            if (renderer.IsStarted()) {
                const bool activeInput = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);
                renderer.Service(queue, activeInput, 2);
                renderer.DiscardConsumed(queue);
            } else {
                Sleep(2);
            }

            if (!acceptingInput &&
                (!renderer.IsStarted() || renderer.IsDrained(queue))) {
                break;
            }
        }
    } catch (...) {
        renderer.Stop();
        client->Stop();
        throw;
    }

    if (!receivedInput || !renderer.IsStarted()) {
        renderer.Stop();
        client->Stop();
        throw std::runtime_error(
            "No process-loopback 7.1.4 frames arrived before the live timeout");
    }

    renderer.Stop();
    client->Stop();

    std::cout << "Live process loopback complete\n"
              << "  PCM frames=" << pcmFrames
              << ", max PCM queue=" << maximumQueuedFrames << " frames\n"
              << std::flush;
    PrintEndpointRenderStats(renderer.Stats());
}
} // namespace dolby
