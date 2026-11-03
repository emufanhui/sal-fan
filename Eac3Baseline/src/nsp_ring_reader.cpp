#include "nsp_ring_reader.h"

#include <algorithm>
#include <cmath>
#include <intrin.h>

namespace dolby {
namespace {

// `read`/`written` cross a process boundary, so plain member access is not
// enough; every access goes through an interlocked operation.
std::uint64_t AcquireLoad(const std::uint64_t& slot) noexcept {
    auto* target = reinterpret_cast<volatile std::int64_t*>(
        const_cast<std::uint64_t*>(&slot));
    return static_cast<std::uint64_t>(
        _InterlockedCompareExchange64(target, 0, 0));
}

void ReleaseStore(std::uint64_t& slot, std::uint64_t value) noexcept {
    _InterlockedExchange64(reinterpret_cast<volatile std::int64_t*>(&slot),
                           static_cast<std::int64_t>(value));
}

std::int16_t FromFloat(float value) noexcept {
    const float scaled = value * 32767.0f;
    const float bounded = std::min(32767.0f, std::max(-32768.0f, scaled));
    return static_cast<std::int16_t>(std::lround(bounded));
}

}  // namespace

NspRingReader::NspRingReader(const std::wstring& mappingName) {
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                mappingName.c_str());
    if (mapping_ == nullptr) return;

    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (view_ == nullptr) return;

    auto* header = static_cast<detail::NspHeader*>(view_);
    if (header->magic != kMagic || header->version != kVersion ||
        header->channels != kChannels || header->sampleRate != kSampleRate ||
        (header->format != kFormatInt16 && header->format != kFormatFloat32) ||
        header->capacity == 0) {
        // Not a ring we can read. Leave IsOpen() false rather than guessing.
        UnmapViewOfFile(view_);
        view_ = nullptr;
        return;
    }

    header_ = header;
    data_ = static_cast<unsigned char*>(view_) + 64;
    channels_ = header->channels;
    sampleRate_ = header->sampleRate;
    format_ = header->format;

    dataEvent_ = OpenEventW(SYNCHRONIZE, FALSE, (mappingName + L"_Data").c_str());
}

NspRingReader::~NspRingReader() {
    if (dataEvent_ != nullptr) CloseHandle(dataEvent_);
    if (view_ != nullptr) UnmapViewOfFile(view_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
}

std::uint64_t NspRingReader::Dropped() const noexcept {
    return header_ == nullptr ? 0 : AcquireLoad(header_->dropped);
}

std::uint64_t NspRingReader::Pushes() const noexcept {
    return header_ == nullptr ? 0 : AcquireLoad(header_->pushes);
}

std::vector<std::int16_t> NspRingReader::ReadFrames(std::size_t maxFrames) {
    if (header_ == nullptr || maxFrames == 0) return {};

    const std::uint64_t written = AcquireLoad(header_->written);
    std::uint64_t read = AcquireLoad(header_->read);
    const std::uint64_t capacity = header_->capacity;

    // Falling behind means the oldest frames are already gone; skip to the
    // oldest one still present instead of replaying stale audio.
    if (written - read > capacity) read = written - capacity;

    const std::uint64_t available = written - read;
    if (available == 0) return {};

    const std::size_t frames = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, maxFrames));
    const std::size_t channels = channels_;

    std::vector<std::int16_t> out(frames * channels);
    if (format_ == kFormatInt16) {
        const auto* samples = reinterpret_cast<const std::int16_t*>(data_);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at =
                static_cast<std::size_t>((read + frame) % capacity) * channels;
            std::copy_n(samples + at, channels, out.begin() +
                        static_cast<std::ptrdiff_t>(frame * channels));
        }
    } else {
        const auto* samples = reinterpret_cast<const float*>(data_);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at =
                static_cast<std::size_t>((read + frame) % capacity) * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                out[frame * channels + channel] = FromFloat(samples[at + channel]);
            }
        }
    }

    ReleaseStore(header_->read, read + frames);
    return out;
}

std::vector<float> NspRingReader::ReadFramesFloat(std::size_t maxFrames) {
    if (header_ == nullptr || maxFrames == 0) return {};

    const std::uint64_t written = AcquireLoad(header_->written);
    std::uint64_t read = AcquireLoad(header_->read);
    const std::uint64_t capacity = header_->capacity;

    if (written - read > capacity) read = written - capacity;

    const std::uint64_t available = written - read;
    if (available == 0) return {};

    const std::size_t frames = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, maxFrames));
    const std::size_t channels = channels_;

    std::vector<float> out(frames * channels);
    if (format_ == kFormatInt16) {
        const auto* samples = reinterpret_cast<const std::int16_t*>(data_);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at =
                static_cast<std::size_t>((read + frame) % capacity) * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                out[frame * channels + channel] =
                    static_cast<float>(samples[at + channel]) / 32768.0f;
            }
        }
    } else {
        const auto* samples = reinterpret_cast<const float*>(data_);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at =
                static_cast<std::size_t>((read + frame) % capacity) * channels;
            std::copy_n(samples + at, channels,
                        out.begin() + static_cast<std::ptrdiff_t>(frame * channels));
        }
    }

    ReleaseStore(header_->read, read + frames);
    return out;
}

bool NspRingReader::WaitForData(DWORD milliseconds) noexcept {
    if (dataEvent_ == nullptr) return false;
    return WaitForSingleObject(dataEvent_, milliseconds) == WAIT_OBJECT_0;
}

}  // namespace dolby
