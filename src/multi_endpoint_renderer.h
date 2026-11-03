#pragma once

#include "speaker_layout.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dolby {

enum class RendererLatencyMode {
    Safe,
    Balanced,
    Low,
};

RendererLatencyMode ParseRendererLatencyMode(std::wstring_view value);
std::wstring_view RendererLatencyModeName(RendererLatencyMode mode);

class CrossoverStage;

class InterleavedPcmQueue {
public:
    explicit InterleavedPcmQueue(std::size_t channels);

    void Append(std::vector<std::int16_t>&& samples);

    // Producer-side entry for sources that hand over float, which is all of the
    // live ones (NSP ring, process loopback, Omniphony pipe). Running the
    // crossover here rather than after the conversion keeps the filters in the
    // float domain and gives every producer one place that does it.
    //
    // cdsp only accepts blocks of exactly its configured chunk size, while a
    // capture callback carries whatever it happens to carry, so this accumulates
    // and processes whole chunks. The remainder waits for the next call, which
    // costs a fixed one-chunk delay (256 frames, 5.33 ms at 48 kHz) and means
    // the last partial chunk of a stream is dropped on stop.
    void AppendInterleaved(const float* samples, std::size_t frames);

    // Control side. `stage` may be null, which is the bypass case and the
    // default: nothing about the queue changes until one is attached.
    void SetCrossover(CrossoverStage* stage);
    bool ConfigureCrossover(std::size_t sampleRate);
    CrossoverStage* Crossover() const { return crossover_; }

    std::uint64_t EndFrame() const;
    std::uint64_t FramesAvailable(double sourcePosition) const;
    double Sample(std::uint64_t frame, std::size_t channel) const;
    void DiscardBefore(double sourcePosition);
    std::size_t Channels() const { return channels_; }

private:
    void AppendBypassed(const float* samples, std::size_t frames);

    std::size_t channels_{};
    std::vector<std::int16_t> samples_;
    std::uint64_t baseFrame_{};
    CrossoverStage* crossover_{};
    std::vector<float> pending_;
    std::vector<float> scratch_;
};

struct EndpointRenderStats {
    std::wstring routeName;
    std::wstring endpointName;
    bool isMaster{};
    std::uint64_t starvationFrames{};
    double clockSeconds{};
    double clockDeltaMilliseconds{};
    double relativeRatePpm{};
    double resampleAdjustmentPpm{};
    double phaseErrorMilliseconds{};
    double maximumPhaseErrorMilliseconds{};
    double bufferMilliseconds{};
    double streamLatencyMilliseconds{};
    std::uint32_t bufferFrames{};
    std::uint32_t defaultPeriodFrames{};
    std::uint32_t minimumPeriodFrames{};
    std::uint32_t selectedPeriodFrames{};
    bool lowLatencyApi{};
    bool mmcssEnabled{};
};

class MultiEndpointRenderer {
public:
    MultiEndpointRenderer(const SpeakerLayout& layout, double gain,
                          RendererLatencyMode latencyMode = RendererLatencyMode::Safe);
    ~MultiEndpointRenderer();

    MultiEndpointRenderer(const MultiEndpointRenderer&) = delete;
    MultiEndpointRenderer& operator=(const MultiEndpointRenderer&) = delete;

    void Prime(const InterleavedPcmQueue& queue);
    void Start();
    void PrimeAndStart(const InterleavedPcmQueue& queue);
    void Service(const InterleavedPcmQueue& queue,
                 bool countStarvation,
                 DWORD timeoutMilliseconds = 2);
    void DiscardConsumed(InterleavedPcmQueue& queue) const;
    std::uint64_t MinimumFramesAvailable(const InterleavedPcmQueue& queue) const;
    std::uint32_t MaximumBufferFrames() const;
    bool IsDrained(const InterleavedPcmQueue& queue) const;
    bool IsStarted() const;
    void Stop();

    const SpeakerLayout& Layout() const;
    std::vector<EndpointRenderStats> Stats() const;

private:
    void RecoverPhysicalOutputs(const InterleavedPcmQueue& queue,
                                HRESULT failure,
                                const std::vector<double>& sourcePositions,
                                std::wstring_view stage);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void TestSpeakerLayout(double seconds,
                       const SpeakerLayout& layout,
                       double gain,
                       std::wstring_view speakerName = {},
                       RendererLatencyMode latencyMode = RendererLatencyMode::Safe);
void PrintEndpointRenderStats(const std::vector<EndpointRenderStats>& stats);

} // namespace dolby
