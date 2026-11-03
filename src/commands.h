#pragma once

#include "audio_platform.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dolby {

enum class RendererLatencyMode;

enum class DtsXDecodeOutput {
    SpatialObjects,
    Pcm71,
};

void RenderTransportTest(double seconds, const std::wstring& filter,
                         const std::wstring& mode);
void ReplayIec61937Wave(const std::filesystem::path& inputPath,
                        const std::wstring& endpointFilter,
                        std::uint64_t repeatCount);
void SpatialSignalTest(double seconds, const std::wstring& filter,
                       const std::wstring& mode);
void CaptureLoopback(double seconds, const std::wstring& filter,
                     const std::filesystem::path& outputPath);
void CaptureProcessLoopback(double seconds, DWORD processId,
                            const std::filesystem::path& outputPath,
                            WORD channels = 8);

void ListAudioDecoders(const std::wstring& filter, bool inspectTypes);
void ProbeWinRtDecoder(const std::wstring& runtimeClass);
void ProbeSpatialMetadata(const std::wstring& endpointFilter);
void ProbeDtsXLicense(const std::wstring& codecName);
void ProbeDtsXFieldOfUse();
void ProbeDtsXDecode(const std::filesystem::path& inputPath, std::size_t maxBursts,
                     DtsXDecodeOutput outputMode,
                     const std::filesystem::path& outputPath = {});
void PlayLiveDtsXLayout(double seconds, const std::filesystem::path& layoutPath,
                        double gain, DWORD prebufferMilliseconds,
                        RendererLatencyMode latencyMode);
void PlayLivePcmLayout(double seconds, const std::filesystem::path& layoutPath,
                       double gain, DWORD prebufferMilliseconds,
                       RendererLatencyMode latencyMode);
void ProbeMediaTypes(const std::filesystem::path& inputPath);

void AnalyzeFloatWave(const std::filesystem::path& inputPath);
void AnalyzeIec61937Wave(const std::filesystem::path& inputPath);
void ExtractDtsHdWave(const std::filesystem::path& inputPath,
                      const std::filesystem::path& outputPath);
void AnalyzeDtsXExss(const std::filesystem::path& inputPath);
void AnalyzeMatWave(const std::filesystem::path& inputPath);
void AnalyzeMatLayout(const std::filesystem::path& inputPath,
                      const std::filesystem::path& layoutPath);
void AnalyzeMatPositionTimeline(const std::filesystem::path& inputPath);
void CompareMatPositionFixtures(const std::array<std::filesystem::path, 6>& inputPaths);
void ExtractMat712Wave(const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath);
void PlayAnalog712(const std::filesystem::path& inputPath,
                   const std::wstring& rearFilter,
                   const std::wstring& heightFilter,
                   double gain,
                   std::uint64_t repeatCount);
void PlayLiveMat712(double seconds,
                    const std::wstring& rearFilter,
                    const std::wstring& heightFilter,
                    double gain,
                    DWORD prebufferMilliseconds);
void PlayLiveMatLayout(double seconds,
                       const std::filesystem::path& layoutPath,
                       double gain,
                       DWORD prebufferMilliseconds,
                       RendererLatencyMode latencyMode);
void PlayLiveOmniphonyPcm(double seconds,
                          const std::filesystem::path& layoutPath,
                          double gain,
                          DWORD prebufferMilliseconds,
                          RendererLatencyMode latencyMode);
void PlayLiveNspLayout(double seconds,
                       const std::filesystem::path& layoutPath,
                       double gain,
                       DWORD prebufferMilliseconds,
                       RendererLatencyMode latencyMode,
                       const std::wstring& ringName = L"Local\\NSP_PCM714");

// Opens a process-loopback capture on the given process tree. The returned
// client is not initialized: the caller picks the capture format.
ComPtr<IAudioClient> ActivateProcessLoopback(DWORD processId);

// Bridges the 12-channel bed a process renders to the speaker layout, using
// Windows process loopback as the source. Loopback only carries what the
// process submits, which is the endpoint mix format, so a 12-channel bed needs
// a 12-channel endpoint (a virtual cable, for instance) — a stereo endpoint
// hands back two channels no matter what the capture format asks for.
void PlayLiveProcessLayout(double seconds,
                           DWORD processId,
                           const std::filesystem::path& layoutPath,
                           double gain,
                           DWORD prebufferMilliseconds,
                           RendererLatencyMode latencyMode);

} // namespace dolby
