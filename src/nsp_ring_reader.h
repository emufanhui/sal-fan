#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dolby {

namespace detail {

// Wire contract fixed by NativeSpatialPipe in docs/spatialaudiolab-bridge.md.
// 64 bytes, then `capacity` frames of `channels` interleaved samples.
struct NspHeader {
    std::uint32_t magic;       // 0  "NSP7"
    std::uint32_t version;     // 4
    std::uint32_t channels;    // 8
    std::uint32_t sampleRate;  // 12
    std::uint32_t format;      // 16  0 = int16, 1 = float32
    std::uint32_t capacity;    // 20  ring depth in frames
    std::uint64_t written;     // 24  producer owned
    std::uint64_t read;        // 32  consumer owned
    std::uint64_t dropped;     // 40  producer owned
    std::uint64_t pushes;      // 48  producer owned
    std::uint64_t reserved;    // 56
};

}  // namespace detail

/// Reader for the NativeSpatialPipe shared-memory PCM ring.
///
/// Data arrives in the SpatialAudioLab canonical channel order, so a consumer
/// must not re-map channels the way the MAT capture path does. `read` lives
/// inside the mapping, so the mapping has to be opened for write access even
/// though this side only consumes audio.
class NspRingReader {
public:
    static constexpr std::uint32_t kMagic = 0x3750534Eu;  // "NSP7"
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kFormatInt16 = 0;
    static constexpr std::uint32_t kFormatFloat32 = 1;
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::size_t kChannels = 12;

    explicit NspRingReader(
        const std::wstring& mappingName = L"Local\\NSP_PCM714");
    ~NspRingReader();

    NspRingReader(const NspRingReader&) = delete;
    NspRingReader& operator=(const NspRingReader&) = delete;

    bool IsOpen() const noexcept { return view_ != nullptr; }
    std::uint32_t Channels() const noexcept { return channels_; }
    std::uint32_t SampleRate() const noexcept { return sampleRate_; }
    std::uint32_t Format() const noexcept { return format_; }

    std::uint64_t Dropped() const noexcept;
    std::uint64_t Pushes() const noexcept;

    /// Up to maxFrames frames of interleaved int16, advancing `read`.
    std::vector<std::int16_t> ReadFrames(std::size_t maxFrames);

    /// Up to maxFrames frames of interleaved float32, advancing `read`.
    ///
    /// Same frames as ReadFrames, handed over without being quantised: an int16
    /// ring is scaled to +/-1. The crossover wants this form, and a ring may
    /// only be consumed once, so a stage that needs both has to call this one
    /// and convert for the meters itself.
    std::vector<float> ReadFramesFloat(std::size_t maxFrames);

    /// Waits for the producer's data signal. Returns false when the companion
    /// event is unavailable so callers fall back to polling.
    bool WaitForData(DWORD milliseconds) noexcept;

private:
    HANDLE mapping_{};
    void* view_{};
    HANDLE dataEvent_{};
    detail::NspHeader* header_{};
    unsigned char* data_{};
    std::uint32_t channels_{};
    std::uint32_t sampleRate_{};
    std::uint32_t format_{};
};

}  // namespace dolby
