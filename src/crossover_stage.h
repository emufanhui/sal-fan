#pragma once

// Optional crossover / bass-management stage, ported from the Aggregate ASIO
// workspace (SalAsio) where it sits between the host block and the engine ring.
//
// In SAL the same work happens just before the producer's float samples become
// the queue's interleaved int16 block: one stage over the whole 12-channel
// frame, before it is split across the three cards.
//
// It is backed by cdsp_cpp.dll, a small C ABI library (cdsp_init / cdsp_process
// / ...) that walks a set of filter paths and a mixing matrix over one
// interleaved float block. The dependency is soft: playback works without it,
// and every failure - DLL missing, config missing or rejected, chunk size or
// channel count that does not match - falls back to bypass with one diagnostic
// line instead of breaking audio.
//
// Threading: Load / Reload / Configure / Reset belong to the control side,
// Process to the audio side. Process allocates nothing, takes no lock and logs
// nothing.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>

namespace dolby {

class CrossoverStage {
public:
    CrossoverStage() = default;
    ~CrossoverStage();

    CrossoverStage(const CrossoverStage&) = delete;
    CrossoverStage& operator=(const CrossoverStage&) = delete;

    // Loads cdsp_cpp.dll from `directory` and applies the JSON at `configPath`
    // at `sampleRate`. False means "stay bypassed"; Diagnostic() says why.
    //
    // cdsp never resamples - it designs its filters and its millisecond delays
    // for the samplerate in the JSON - so the config is re-issued with the rate
    // SAL actually runs at instead of forcing one JSON per sample rate.
    bool Load(const std::filesystem::path& directory, const std::filesystem::path& configPath,
              std::size_t sampleRate);

    // Re-reads the JSON without stopping the audio path.
    bool Reload(std::size_t sampleRate);

    // Agrees the DSP's chunk size, channel counts and rate with the stream that
    // is about to run. Process() only runs after this succeeded.
    bool Configure(std::size_t frames, std::size_t channels, std::size_t sampleRate);

    // Audio side. True when `out` holds processed audio; false means the caller
    // keeps using the input block unchanged.
    bool Process(const float* in, float* out, std::size_t frames) noexcept;

    // Control side. Drops the cdsp state but keeps the library loaded.
    void Reset() noexcept;

    bool IsLoaded() const noexcept { return module_ != nullptr; }
    bool IsActive() const noexcept { return active_.load(std::memory_order_acquire); }
    std::size_t ChunkSize() const noexcept { return chunk_; }
    std::size_t ChannelsIn() const noexcept { return dspIn_; }
    std::size_t ChannelsOut() const noexcept { return dspOut_; }
    std::size_t SampleRate() const noexcept { return rate_; }
    const std::filesystem::path& ConfigPath() const noexcept { return configPath_; }
    const std::wstring& Diagnostic() const noexcept { return diagnostic_; }

private:
    using InitFn = int (*)(const char*);
    using ProcessFn = int (*)(const float*, float*, unsigned int);
    using CountFn = int (*)();
    using ResetFn = void (*)();

    bool OpenLibrary(const std::filesystem::path& directory);
    bool ApplyConfig(std::size_t sampleRate);

    void* module_{};
    InitFn init_{};
    ProcessFn process_{};
    CountFn channelsIn_{};
    CountFn channelsOut_{};
    CountFn chunkSize_{};
    CountFn sampleRate_{};
    ResetFn reset_{};
    std::filesystem::path configPath_;
    std::wstring diagnostic_;
    std::atomic<bool> active_{false};
    std::size_t frames_{};
    std::size_t channels_{};
    std::size_t dspIn_{};
    std::size_t dspOut_{};
    std::size_t chunk_{};
    std::size_t rate_{};
};

// Resolves the config file SAL should hand to the DSP stage. Three candidates,
// first hit wins:
//
//   1. %SAL_CDSP_CONFIG%                          - explicit override, for tests
//   2. <exe dir>\dsp.json                          - next to the engine
//   3. %ProgramData%\SpatialAudioLab\dsp.json      - machine-wide install
//
// Empty when nothing is there, which means bypass. Known folders are resolved
// with SHGetKnownFolderPath, never by expanding an environment variable.
std::filesystem::path ResolveCrossoverConfigPath();

// Folder the engine lives in; cdsp_cpp.dll is loaded from here on purpose, so a
// stale copy picked up from PATH cannot silently change what the filters do.
std::filesystem::path ResolveEngineDirectory();

} // namespace dolby
