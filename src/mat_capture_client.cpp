#include "mat_capture_client.h"

#include "wave_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace dolby {

namespace {

struct MatCaptureReadHeaderV2 {
    ULONG version;
    ULONG headerBytes;
    ULONG payloadBytes;
    ULONG reserved;
    ULONGLONG firstByteSequence;
    ULONGLONG totalBytesWritten;
    ULONGLONG droppedBytes;
    GUID subFormat;
    ULONGLONG formatChanges;
};

struct MatCaptureStatsV2 {
    ULONG version;
    ULONG capacityBytes;
    ULONG availableBytes;
    ULONG reserved;
    ULONGLONG totalBytesWritten;
    ULONGLONG totalBytesRead;
    ULONGLONG droppedBytes;
    GUID subFormat;
    ULONGLONG formatChanges;
};

static_assert(offsetof(MAT_CAPTURE_READ_HEADER, SampleRate) == sizeof(MatCaptureReadHeaderV2));
static_assert(offsetof(MAT_CAPTURE_STATS, SampleRate) == sizeof(MatCaptureStatsV2));

Iec61937WaveFormat CarrierFormatForSubFormat(const GUID& subFormat) {
    if (IsEqualGUID(subFormat, kDts)) {
        Iec61937WaveFormat format = MakeIec61937Format(kDts, 48000,
                                                       KSAUDIO_SPEAKER_5POINT1_SURROUND);
        format.formatExt.Format.nChannels = 2;
        format.formatExt.Format.nSamplesPerSec = 48000;
        format.formatExt.Format.nAvgBytesPerSec = 192000;
        format.formatExt.Format.nBlockAlign = 4;
        format.encodedChannelCount = 6;
        return format;
    }
    if (IsEqualGUID(subFormat, kDolbyDigitalPlus) ||
        IsEqualGUID(subFormat, kDolbyDigitalPlusAtmos)) {
        return MakeDolbyDigitalPlusFormat(subFormat);
    }
    if (IsEqualGUID(subFormat, kDtsHd) || IsEqualGUID(subFormat, kDtsXE1) ||
        IsEqualGUID(subFormat, kDtsXE2) || IsEqualGUID(subFormat, kDolbyMlpMat10) ||
        IsEqualGUID(subFormat, kDolbyMat20) ||
        IsEqualGUID(subFormat, kDolbyMat21Profile3)) {
        return MakeIec61937Format(subFormat);
    }
    throw std::runtime_error("The capture driver reported an unsupported IEC 61937 subtype");
}

void PopulateLegacyFormat(const GUID& subFormat, MAT_CAPTURE_STATS& stats) {
    if (IsEqualGUID(subFormat, GUID_NULL)) return;
    const Iec61937WaveFormat format = CarrierFormatForSubFormat(subFormat);
    stats.SampleRate = format.formatExt.Format.nSamplesPerSec;
    stats.ChannelMask = format.formatExt.dwChannelMask;
    stats.Channels = format.formatExt.Format.nChannels;
    stats.BitsPerSample = format.formatExt.Format.wBitsPerSample;
    stats.ValidBitsPerSample = format.formatExt.Samples.wValidBitsPerSample;
    stats.BlockAlign = format.formatExt.Format.nBlockAlign;
}

void PopulateLegacyFormat(const GUID& subFormat, MAT_CAPTURE_READ_HEADER& header) {
    MAT_CAPTURE_STATS stats{};
    PopulateLegacyFormat(subFormat, stats);
    header.SampleRate = stats.SampleRate;
    header.ChannelMask = stats.ChannelMask;
    header.Channels = stats.Channels;
    header.BitsPerSample = stats.BitsPerSample;
    header.ValidBitsPerSample = stats.ValidBitsPerSample;
    header.BlockAlign = stats.BlockAlign;
}

} // namespace

MAT_CAPTURE_STATS QueryMatCaptureStats(const HANDLE device) {
    alignas(MAT_CAPTURE_STATS) std::array<BYTE, sizeof(MAT_CAPTURE_STATS)> response{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_GET_STATS, nullptr, 0,
                         response.data(), static_cast<DWORD>(response.size()),
                         &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_GET_STATS failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(ULONG)) {
        throw std::runtime_error("Capture driver returned truncated stats");
    }
    const ULONG version = *reinterpret_cast<const ULONG*>(response.data());
    if (version == MAT_CAPTURE_PROTOCOL_VERSION && returned == sizeof(MAT_CAPTURE_STATS)) {
        return *reinterpret_cast<const MAT_CAPTURE_STATS*>(response.data());
    }
    if (version == MAT_CAPTURE_PROTOCOL_VERSION_IEC61937 &&
        returned == sizeof(MatCaptureStatsV2)) {
        const auto& legacy = *reinterpret_cast<const MatCaptureStatsV2*>(response.data());
        MAT_CAPTURE_STATS stats{};
        stats.Version = legacy.version;
        stats.CapacityBytes = legacy.capacityBytes;
        stats.AvailableBytes = legacy.availableBytes;
        stats.Reserved = legacy.reserved;
        stats.TotalBytesWritten = legacy.totalBytesWritten;
        stats.TotalBytesRead = legacy.totalBytesRead;
        stats.DroppedBytes = legacy.droppedBytes;
        stats.SubFormat = legacy.subFormat;
        stats.FormatChanges = legacy.formatChanges;
        PopulateLegacyFormat(stats.SubFormat, stats);
        return stats;
    }
    {
        std::ostringstream message;
        message << "IEC 61937 capture driver returned incompatible stats (version="
                << version << ", bytes=" << returned << ", expected version="
                << MAT_CAPTURE_PROTOCOL_VERSION << ", expected bytes="
                << sizeof(MAT_CAPTURE_STATS) << ')';
        throw std::runtime_error(message.str());
    }
}

WinHandle OpenMatCaptureDevice() {
    WinHandle device(CreateFileW(MAT_CAPTURE_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device.IsValid()) {
        std::ostringstream message;
        message << "Could not open " << "\\\\.\\DolbyDecoderMat" << " (Win32 "
                << GetLastError() << "). Install and load the capture driver as administrator.";
        throw std::runtime_error(message.str());
    }
    return device;
}

void ResetMatCapture(const HANDLE device) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_RESET, nullptr, 0,
                         nullptr, 0, &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_RESET failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
}

MatCaptureReadView ReadMatCapture(const HANDLE device, std::vector<BYTE>& request) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_READ, nullptr, 0,
                         request.data(), static_cast<DWORD>(request.size()), &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_READ failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(ULONG)) {
        throw std::runtime_error("Capture driver returned a truncated read header");
    }

    const ULONG version = *reinterpret_cast<const ULONG*>(request.data());
    if (version == MAT_CAPTURE_PROTOCOL_VERSION) {
        if (returned < sizeof(MAT_CAPTURE_READ_HEADER)) {
            throw std::runtime_error("Capture driver returned a truncated v3 read header");
        }
        const auto* header = reinterpret_cast<const MAT_CAPTURE_READ_HEADER*>(request.data());
        if (header->HeaderBytes < sizeof(MAT_CAPTURE_READ_HEADER) ||
            static_cast<std::uint64_t>(header->HeaderBytes) + header->PayloadBytes > returned) {
            throw std::runtime_error("Capture driver returned an invalid v3 read result");
        }
        return {*header, request.data() + header->HeaderBytes};
    }
    if (version == MAT_CAPTURE_PROTOCOL_VERSION_IEC61937) {
        if (returned < sizeof(MatCaptureReadHeaderV2)) {
            throw std::runtime_error("Capture driver returned a truncated v2 read header");
        }
        const auto& legacy = *reinterpret_cast<const MatCaptureReadHeaderV2*>(request.data());
        if (legacy.headerBytes < sizeof(MatCaptureReadHeaderV2) ||
            static_cast<std::uint64_t>(legacy.headerBytes) + legacy.payloadBytes > returned) {
            throw std::runtime_error("Capture driver returned an invalid v2 read result");
        }
        MAT_CAPTURE_READ_HEADER header{};
        header.Version = legacy.version;
        header.HeaderBytes = legacy.headerBytes;
        header.PayloadBytes = legacy.payloadBytes;
        header.Reserved = legacy.reserved;
        header.FirstByteSequence = legacy.firstByteSequence;
        header.TotalBytesWritten = legacy.totalBytesWritten;
        header.DroppedBytes = legacy.droppedBytes;
        header.SubFormat = legacy.subFormat;
        header.FormatChanges = legacy.formatChanges;
        PopulateLegacyFormat(header.SubFormat, header);
        return {header, request.data() + legacy.headerBytes};
    }
    throw std::runtime_error("Capture driver returned an unsupported protocol version");
}

void ResetDriverFormatLog() {
    WinHandle device = OpenMatCaptureDevice();
    DWORD returned = 0;
    if (!DeviceIoControl(device.Get(), IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG, nullptr, 0,
                         nullptr, 0, &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG failed (Win32 "
                << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    std::wcout << L"Driver format log reset.\n";
}

void PrintDriverFormatLog() {
    constexpr std::size_t recordCapacity = 512;
    WinHandle device = OpenMatCaptureDevice();
    std::vector<BYTE> response(
        sizeof(MAT_CAPTURE_FORMAT_LOG) + recordCapacity * sizeof(MAT_CAPTURE_FORMAT_EVENT));
    DWORD returned = 0;
    if (!DeviceIoControl(device.Get(), IOCTL_MAT_CAPTURE_GET_FORMAT_LOG, nullptr, 0,
                         response.data(), static_cast<DWORD>(response.size()),
                         &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_GET_FORMAT_LOG failed (Win32 "
                << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(MAT_CAPTURE_FORMAT_LOG)) {
        throw std::runtime_error("Capture driver returned a truncated format log");
    }

    const auto& log = *reinterpret_cast<const MAT_CAPTURE_FORMAT_LOG*>(response.data());
    if (log.Version != MAT_CAPTURE_FORMAT_LOG_VERSION ||
        log.HeaderBytes != sizeof(MAT_CAPTURE_FORMAT_LOG) ||
        log.RecordBytes != sizeof(MAT_CAPTURE_FORMAT_EVENT) ||
        log.RecordCount > recordCapacity ||
        static_cast<std::uint64_t>(log.HeaderBytes) +
                static_cast<std::uint64_t>(log.RecordCount) * log.RecordBytes > returned) {
        throw std::runtime_error("Capture driver returned an incompatible format log");
    }

    const auto* records = reinterpret_cast<const MAT_CAPTURE_FORMAT_EVENT*>(
        response.data() + log.HeaderBytes);
    const ULONGLONG origin = log.RecordCount == 0 ? 0 : records[0].InterruptTime100ns;
    std::wcout << L"Driver format log: retained=" << log.RecordCount
               << L", total=" << log.TotalRecords
               << L", overwritten=" << log.DroppedRecords << L"\n";

    for (ULONG index = 0; index < log.RecordCount; ++index) {
        const auto& event = records[index];
        const wchar_t* type = event.Type == MatCaptureFormatEventIsFormatSupported
                                ? L"support"
                                : event.Type == MatCaptureFormatEventNewStream
                                    ? L"stream"
                                    : event.Type == MatCaptureFormatEventWritePacket
                                        ? L"packet"
                                    : L"unknown";

        const double milliseconds = static_cast<double>(
            event.InterruptTime100ns - origin) / 10'000.0;
        if (event.Type == MatCaptureFormatEventWritePacket) {
            const ULONG lastPacket = static_cast<ULONG>(event.FormatTag) |
                (static_cast<ULONG>(event.Channels) << 16);
            std::wcout << L'#' << event.Sequence
                       << L" +" << std::fixed << std::setprecision(3) << milliseconds << L" ms"
                       << L" packet stream=0x" << std::hex << std::uppercase
                       << event.ProcessId
                       << L" number=" << std::dec << event.Pin;
            if (lastPacket == ULONG_MAX) {
                std::wcout << L" last=none";
            } else {
                std::wcout << L" last=" << lastPacket;
            }
            std::wcout << L" state=" << event.Capture
                       << L" status=0x" << std::hex << std::uppercase
                       << std::setw(8) << std::setfill(L'0')
                       << static_cast<std::uint32_t>(event.Status)
                       << std::dec << std::nouppercase << std::setfill(L' ')
                       << L" buffer=" << event.FormatSize
                       << L" notifications=" << event.ChannelMask
                       << L" packet-bytes=" << event.AverageBytesPerSecond
                       << L" interval=" << event.SampleRate << L" ms\n";
            continue;
        }

        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = event.FormatTag;
        format.Format.nChannels = event.Channels;
        format.Format.nSamplesPerSec = event.SampleRate;
        format.Format.nAvgBytesPerSec = event.AverageBytesPerSecond;
        format.Format.nBlockAlign = event.BlockAlign;
        format.Format.wBitsPerSample = event.BitsPerSample;
        format.Format.cbSize = event.ExtraSize;
        format.Samples.wValidBitsPerSample = event.ValidBitsPerSample;
        format.dwChannelMask = event.ChannelMask;
        format.SubFormat = event.WaveSubFormat;

        std::wcout << L'#' << event.Sequence
                   << L" +" << std::fixed << std::setprecision(3) << milliseconds << L" ms"
                   << L" " << type
                   << L" pid=" << event.ProcessId
                   << L" tid=" << event.ThreadId
                   << L" pin=" << event.Pin
                   << (event.Capture != 0 ? L" capture" : L" render")
                   << L" status=0x" << std::hex << std::uppercase
                   << std::setw(8) << std::setfill(L'0')
                   << static_cast<std::uint32_t>(event.Status)
                   << std::dec << std::nouppercase << std::setfill(L' ')
                   << L" " << WaveFormatText(&format.Format)
                   << L", avg=" << event.AverageBytesPerSecond << L" B/s";
        if (!IsEqualGUID(event.DataSubFormat, event.WaveSubFormat)) {
            std::wcout << L"; KS subtype=" << GuidText(event.DataSubFormat);
        }
        std::wcout << L"\n";
    }
}

void CaptureIec61937Ring(const double seconds, const std::filesystem::path& outputPath,
                         const DWORD pollMilliseconds) {
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    const MAT_CAPTURE_STATS initialStats = QueryMatCaptureStats(device.Get());
    std::unique_ptr<WaveWriter> writer;
    GUID capturedSubFormat = GUID_NULL;
    WORD blockAlign = 0;
    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);
    std::vector<BYTE> alignmentBuffer;
    std::array<BYTE, 3> scanTail{};
    std::size_t scanTailBytes = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t capturedBytes = 0;
    std::uint64_t preambles = 0;

    std::wcout << L"IEC 61937 ring: capacity=" << initialStats.CapacityBytes
               << L" bytes, poll=" << pollMilliseconds << L" ms\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    bool draining = false;
    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (IsEqualGUID(read.header.SubFormat, GUID_NULL)) {
                throw std::runtime_error("Captured bytes arrived without an IEC 61937 subtype");
            }
            if (IsEqualGUID(capturedSubFormat, GUID_NULL)) {
                capturedSubFormat = read.header.SubFormat;
                const Iec61937WaveFormat carrierFormat =
                    CarrierFormatForSubFormat(capturedSubFormat);
                blockAlign = carrierFormat.formatExt.Format.nBlockAlign;
                writer = std::make_unique<WaveWriter>(outputPath,
                                                       &carrierFormat.formatExt.Format);
                std::wcout << L"Carrier: "
                           << WaveFormatText(&carrierFormat.formatExt.Format) << L"\n";
            } else if (!IsEqualGUID(capturedSubFormat, read.header.SubFormat) ||
                       read.header.FormatChanges != 0) {
                throw std::runtime_error("IEC 61937 subtype changed during capture");
            }
            if (capturedBytes != 0 && read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            capturedBytes += read.header.PayloadBytes;

            std::vector<BYTE> scan(scanTailBytes + read.header.PayloadBytes);
            std::copy_n(scanTail.data(), scanTailBytes, scan.begin());
            std::copy_n(read.payload, read.header.PayloadBytes, scan.begin() + scanTailBytes);
            preambles += CountIec61937Preambles(scan.data(), scan.size());
            scanTailBytes = std::min<std::size_t>(scanTail.size(), scan.size());
            std::copy(scan.end() - scanTailBytes, scan.end(), scanTail.begin());

            alignmentBuffer.insert(alignmentBuffer.end(), read.payload,
                                   read.payload + read.header.PayloadBytes);
            const std::size_t completeBytes =
                alignmentBuffer.size() - alignmentBuffer.size() % blockAlign;
            if (completeBytes != 0) {
                writer->Write(alignmentBuffer.data(),
                              static_cast<UINT32>(completeBytes / blockAlign), false);
                alignmentBuffer.erase(alignmentBuffer.begin(),
                                      alignmentBuffer.begin() + completeBytes);
            }
        }

        if (!draining && std::chrono::steady_clock::now() >= deadline) draining = true;
        if (draining && read.header.PayloadBytes == 0) break;
        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    if (!writer) {
        throw std::runtime_error(
            "No IEC 61937 bytes arrived. Start a spatial or encoded-audio source during capture.");
    }
    writer->Finalize();
    const MAT_CAPTURE_STATS finalStats = QueryMatCaptureStats(device.Get());
    std::wcout << L"IEC 61937 ring capture complete: " << outputPath.wstring() << L"\n"
               << L"Bytes=" << capturedBytes << L", IEC preambles=" << preambles
               << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"Driver written=" << finalStats.TotalBytesWritten
               << L", read=" << finalStats.TotalBytesRead
               << L", dropped=" << finalStats.DroppedBytes
               << L", trailing partial=" << alignmentBuffer.size() << L" bytes\n";
}

namespace {

// An E-AC3 syncframe starts with, in big-endian bit order:
//   syncword 0x0B77 (16) | strmtyp (2) | substreamid (3) | frmsiz (11)
// `frmsiz` counts 16-bit words in the frame, minus one.
//
// The capture ring hands over the IEC 61937 carrier as little-endian 16-bit
// samples, so every pair of bytes arrives swapped: the syncword reads 77 0B
// and a header word has to be reassembled low byte first. Frames themselves
// are contiguous; the carrier pads the gap between them, which is why the
// extractor has to resync by scanning rather than by stepping a fixed period.
constexpr BYTE kEc3SyncLow = 0x77;
constexpr BYTE kEc3SyncHigh = 0x0B;
constexpr std::size_t kEc3MinFrameBytes = 256;
constexpr std::size_t kEc3MaxFrameBytes = 8192;

enum class Ec3CommandKind { None, Stop, Rotate };

struct Ec3Command {
    Ec3CommandKind kind = Ec3CommandKind::None;
    std::filesystem::path path;
};

std::filesystem::path Utf8ToPath(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return std::filesystem::path(wide);
}

/// Reads one line-based command from the parent. A broken pipe means the
/// parent is gone, which stops the recording the same way an explicit STOP
/// does. ROTATE keeps the capture device open and only switches the file.
Ec3Command ReadEc3Command() {
    static std::string buffer;

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) return {};

    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            return { Ec3CommandKind::Stop, {} };
        }
        return {};
    }
    if (available == 0) return {};

    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!ReadFile(input, chunk.data(), available, &read, nullptr) || read == 0) {
        return {};
    }
    buffer.append(chunk.data(), read);

    std::size_t newline = buffer.find('\n');
    while (newline != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Tolerate a UTF-8 BOM. A parent whose encoder emits a preamble would
        // otherwise prefix the first command with EF BB BF, and that command
        // would be dropped as unrecognised.
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }

        if (line == "STOP") return { Ec3CommandKind::Stop, {} };
        constexpr char kRotatePrefix[] = "ROTATE ";
        if (line.rfind(kRotatePrefix, 0) == 0) {
            return { Ec3CommandKind::Rotate, Utf8ToPath(line.substr(sizeof(kRotatePrefix) - 1)) };
        }
        newline = buffer.find('\n');
    }
    return {};
}

/// Cuts whole E-AC3 syncframes out of the carrier stream.
///
/// The destination is a callback rather than a file stream: the same extractor
/// feeds `ec3-record` (to an .ec3 file) and `ec3-pipe` (to the live decoder
/// that renders 7.1.4 into the three endpoints).
class Ec3FrameExtractor {
public:
    using Sink = std::function<void(const BYTE*, std::size_t)>;

    explicit Ec3FrameExtractor(Sink sink) : sink_(std::move(sink)) {}

    void Feed(const BYTE* data, std::size_t bytes) {
        pending_.insert(pending_.end(), data, data + bytes);
        Drain();
    }

    std::uint64_t Frames() const noexcept { return frames_; }
    std::uint64_t PayloadBytes() const noexcept { return payloadBytes_; }

private:
    void Drain() {
        std::size_t from = 0;
        while (true) {
            const std::size_t sync = FindSync(from);
            if (sync == kNoMatch) break;
            // The four header bytes have to be present before frmsiz can be
            // read, and the whole frame before it can be written; otherwise
            // this position stays buffered for the next call.
            if (sync + 4 > pending_.size()) break;
            const std::uint16_t header = static_cast<std::uint16_t>(
                (pending_[sync + 3] << 8) | pending_[sync + 2]);
            const std::size_t frameBytes =
                (static_cast<std::size_t>(header & 0x07FF) + 1) * 2;
            if (frameBytes < kEc3MinFrameBytes || frameBytes > kEc3MaxFrameBytes) {
                from = sync + 2;
                continue;
            }
            if (sync + frameBytes > pending_.size()) break;
            WriteFrame(&pending_[sync], frameBytes);
            from = sync + frameBytes;
        }
        Trim(from);
    }

    /// Drops everything up to `from`, then caps the leftover carrier so that a
    /// long stretch of padding cannot grow the buffer without bound. The cap
    /// keeps two max frames' worth, which is enough to hold a syncword together
    /// with any padding that may precede it.
    void Trim(std::size_t from) {
        pending_.erase(pending_.begin(), pending_.begin() + from);
        constexpr std::size_t kKeep = kEc3MaxFrameBytes * 2;
        if (pending_.size() > kKeep) {
            pending_.erase(pending_.begin(), pending_.end() - kKeep);
        }
    }

    std::size_t FindSync(std::size_t from) const {
        for (std::size_t i = from; i + 1 < pending_.size(); ++i) {
            if (pending_[i] == kEc3SyncLow && pending_[i + 1] == kEc3SyncHigh) {
                return i;
            }
        }
        return kNoMatch;
    }

    /// Writes one syncframe, swapping each pair back into wire order.
    void WriteFrame(const BYTE* frame, std::size_t bytes) {
        std::vector<BYTE> swapped(bytes);
        for (std::size_t i = 0; i + 1 < bytes; i += 2) {
            swapped[i] = frame[i + 1];
            swapped[i + 1] = frame[i];
        }
        sink_(swapped.data(), bytes);
        ++frames_;
        payloadBytes_ += bytes;
    }

    static constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);

    Sink sink_;
    std::vector<BYTE> pending_;
    std::uint64_t frames_ = 0;
    std::uint64_t payloadBytes_ = 0;
};

}  // namespace

void RecordEc3Stream(const std::filesystem::path& outputPath,
                     const DWORD pollMilliseconds) {
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);

    std::ofstream sink(outputPath, std::ios::binary | std::ios::trunc);
    if (!sink) {
        throw std::runtime_error("Unable to create the E-AC3 output file");
    }
    Ec3FrameExtractor extractor([&sink](const BYTE* data, std::size_t bytes) {
        sink.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(bytes));
    });

    std::uint64_t carrierBytes = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t fileFrames = 0;
    std::uint64_t fileBytes = 0;
    bool sawSubFormat = false;
    GUID subFormat = GUID_NULL;
    auto announced = std::chrono::steady_clock::now();

    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (!sawSubFormat) {
                sawSubFormat = true;
                subFormat = read.header.SubFormat;
                std::wcout << L"FORMAT " << SubformatName(subFormat) << L"\n";
                if (!(IsEqualGUID(subFormat, kDolbyDigitalPlus) ||
                      IsEqualGUID(subFormat, kDolbyDigitalPlusAtmos))) {
                    std::wcout << L"STATUS not-eac3\n";
                }
            }
            if (carrierBytes != 0 &&
                read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            carrierBytes += read.header.PayloadBytes;
            extractor.Feed(read.payload, read.header.PayloadBytes);
        }

        const Ec3Command command = ReadEc3Command();
        if (command.kind == Ec3CommandKind::Stop) break;
        if (command.kind == Ec3CommandKind::Rotate) {
            // The capture device stays open, so no carrier bytes are lost
            // across a track change; only the destination file changes.
            sink.flush();
            sink.close();
            sink.clear();
            sink.open(command.path, std::ios::binary | std::ios::trunc);
            if (!sink) {
                throw std::runtime_error("Unable to create the rotated E-AC3 output file");
            }
            std::wcout << L"ROTATED " << command.path.wstring()
                       << L" frames=" << (extractor.Frames() - fileFrames)
                       << L" bytes=" << (extractor.PayloadBytes() - fileBytes) << L"\n";
            fileFrames = extractor.Frames();
            fileBytes = extractor.PayloadBytes();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= std::chrono::milliseconds(500)) {
            announced = now;
            std::wcout << L"STATUS frames=" << extractor.Frames()
                       << L" bytes=" << extractor.PayloadBytes()
                       << L" carrier=" << carrierBytes << L"\n";
        }

        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    sink.flush();
    sink.close();
    std::wcout << L"STOPPED frames=" << extractor.Frames()
               << L" bytes=" << extractor.PayloadBytes()
               << L" gaps=" << sequenceGaps << L"\n";
}

class MatMlpExtractor {
public:
    using Sink = std::function<void(const BYTE*, std::size_t)>;

    explicit MatMlpExtractor(Sink sink) : sink_(std::move(sink)) {}

    void Feed(const BYTE* data, std::size_t bytes) {
        pending_.insert(pending_.end(), data, data + bytes);
        Drain();
    }

    std::uint64_t PayloadBytes() const noexcept { return payloadBytes_; }
    std::uint64_t Bursts() const noexcept { return bursts_; }
    /// Drops any partially-buffered carrier so the next bytes are re-synced to a
    /// fresh IEC preamble. Called after a ring sequence gap: a dropped/inserted
    /// byte shifts the 16-bit word swap and scrambles everything after it, so the
    /// only safe move is to discard the misaligned tail and re-lock on the next
    /// preamble.
    void Reset() {
        pending_.clear();
    }

private:
    static constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);

    // The IEC 61937-12 MAT carrier delivers the MLP/TrueHD substream in fixed
    // 61440-byte bursts. Each burst begins with an 8-byte IEC preamble
    // (72 F8 1F 4E 16 00 ...) followed by a 24-byte MAT substream header, then
    // the byte-swapped (16-bit LE word) MLP payload, zero-padded out to the
    // burst size. The payload simply continues across bursts (one MLP frame may
    // span several bursts), so we drop only the 32-byte per-burst header and
    // byte-swap the remainder, concatenating every burst into one continuous
    // MLP/TrueHD stream the decoder can parse.
    void Drain() {
        std::size_t from = 0;
        while (true) {
            const std::size_t p = FindPreamble(from);
            if (p == kNoMatch) break;
            if (p + 32 > pending_.size()) { from = p; break; }
            const std::size_t q = FindPreamble(p + 32);
            if (q == kNoMatch) { from = p; break; }
            // Skip the 8-byte IEC preamble + 24-byte MAT substream header; the
            // rest is the byte-swapped MLP payload. Unswap and emit it.
            const std::size_t burstPayload = q - (p + 32);
            WriteBurst(&pending_[p + 32], burstPayload);
            from = q;
        }
        Trim(from);
    }

public:
    // Flush any trailing burst that has no following preamble (end of stream).
    void Finish() {
        std::size_t from = 0;
        while (true) {
            const std::size_t p = FindPreamble(from);
            if (p == kNoMatch) break;
            if (p + 32 > pending_.size()) {
                const std::size_t tail = pending_.size() - (p + 32);
                if (tail > 0) WriteBurst(&pending_[p + 32], tail);
                break;
            }
            const std::size_t q = FindPreamble(p + 32);
            if (q == kNoMatch) {
                WriteBurst(&pending_[p + 32], pending_.size() - (p + 32));
                break;
            }
            WriteBurst(&pending_[p + 32], q - (p + 32));
            from = q;
        }
        pending_.clear();
    }

private:
    std::size_t FindPreamble(std::size_t from) const {
        for (std::size_t i = from; i + 5 < pending_.size(); ++i) {
            if (pending_[i] == 0x72 && pending_[i + 1] == 0xF8 &&
                pending_[i + 2] == 0x1F && pending_[i + 3] == 0x4E &&
                pending_[i + 4] == 0x16 && pending_[i + 5] == 0x00) {
                return i;
            }
        }
        return kNoMatch;
    }

    void WriteBurst(const BYTE* data, std::size_t bytes) {
        const std::size_t n = bytes & ~static_cast<std::size_t>(1);
        swapped_.clear();
        swapped_.reserve(n);
        for (std::size_t i = 0; i < n; i += 2) {
            swapped_.push_back(data[i + 1]);
            swapped_.push_back(data[i]);
        }
        if (!swapped_.empty()) {
            sink_(swapped_.data(), swapped_.size());
            payloadBytes_ += swapped_.size();
            ++bursts_;
        }
    }

    void Trim(std::size_t from) {
        pending_.erase(pending_.begin(), pending_.begin() + from);
        constexpr std::size_t kKeep = 8 + 61440 * 2;
        if (pending_.size() > kKeep) {
            pending_.erase(pending_.begin(), pending_.end() - kKeep);
        }
    }

    Sink sink_;
    std::vector<BYTE> pending_;
    std::vector<BYTE> swapped_;
    std::uint64_t payloadBytes_ = 0;
    std::uint64_t bursts_ = 0;
};

void StreamMatToPipe(const std::wstring& pipeName, const DWORD pollMilliseconds) {
    const std::wstring fullName = L"\\\\.\\pipe\\" + pipeName;
    WinHandle pipe(CreateNamedPipeW(
        fullName.c_str(), PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        1024 * 1024, 1024 * 1024, 0, nullptr));
    if (!pipe.IsValid()) {
        throw std::runtime_error("Unable to create the MLP delivery pipe");
    }
    std::wcout << L"PIPE " << fullName << L"\n" << std::flush;

    // The decoder attaches as a client, exactly like the PCM ring: the engine
    // owns the endpoint and the decoder only produces samples.
    if (!ConnectNamedPipe(pipe.Get(), nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        throw std::runtime_error("The MLP delivery pipe never connected");
    }
    std::wcout << L"CONNECTED\n" << std::flush;

    auto writeAll = [&pipe](const BYTE* data, std::size_t bytes) {
        std::size_t offset = 0;
        while (offset < bytes) {
            DWORD written = 0;
            if (!WriteFile(pipe.Get(), data + offset,
                           static_cast<DWORD>(bytes - offset), &written, nullptr)) {
                throw std::runtime_error("The MLP delivery pipe closed");
            }
            offset += written;
        }
    };
    MatMlpExtractor extractor(writeAll);

    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);

    std::uint64_t carrierBytes = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t expectedSequence = 0;
    bool sawSubFormat = false;
    GUID subFormat = GUID_NULL;
    auto announced = std::chrono::steady_clock::now();

    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (!sawSubFormat) {
                sawSubFormat = true;
                subFormat = read.header.SubFormat;
                std::wcout << L"FORMAT " << SubformatName(subFormat) << L"\n" << std::flush;
            }
            if (carrierBytes != 0 &&
                read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
                // A dropped/inserted byte shifts the 16-bit word swap; discard the
                // misaligned tail and re-lock on the next IEC preamble.
                extractor.Reset();
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            carrierBytes += read.header.PayloadBytes;
            extractor.Feed(read.payload, read.header.PayloadBytes);
        }

        const Ec3Command command = ReadEc3Command();
        if (command.kind == Ec3CommandKind::Stop) break;

        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= std::chrono::milliseconds(500)) {
            announced = now;
            std::wcout << L"STATUS bursts=" << extractor.Bursts()
                       << L" bytes=" << extractor.PayloadBytes()
                       << L" carrier=" << carrierBytes << L"\n" << std::flush;
        }

        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    FlushFileBuffers(pipe.Get());
    std::wcout << L"STOPPED bursts=" << extractor.Bursts()
               << L" bytes=" << extractor.PayloadBytes()
               << L" gaps=" << sequenceGaps << L"\n" << std::flush;
}

void DemuxRawCarrier(const std::filesystem::path& rawPath,
                     const std::filesystem::path& outPath) {
    std::ofstream sink(outPath, std::ios::binary | std::ios::trunc);
    if (!sink) throw std::runtime_error("Unable to create the demux output file");
    MatMlpExtractor extractor([&sink](const BYTE* data, std::size_t bytes) {
        sink.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(bytes));
    });

    std::ifstream in(rawPath, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open the raw carrier file");
    std::vector<BYTE> buf(1 << 20);
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::size_t got = static_cast<std::size_t>(in.gcount());
        if (got > 0) extractor.Feed(buf.data(), got);
    }
    extractor.Finish();
    sink.flush();
    sink.close();
    std::wcout << L"DEMUXED bursts=" << extractor.Bursts()
               << L" bytes=" << extractor.PayloadBytes() << L"\n" << std::flush;
}

void RecordMatMlpStream(const std::filesystem::path& outputPath,
                        const DWORD pollMilliseconds,
                        const double durationSeconds) {
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);

    std::ofstream sink(outputPath, std::ios::binary | std::ios::trunc);
    if (!sink) {
        throw std::runtime_error("Unable to create the MLP output file");
    }
    // The same de-MAT used by mat-pipe: strip the IEC 61937/MAT 1.0 preamble
    // and byte-swap every 16-bit word to recover the bare MLP/TrueHD substream.
    MatMlpExtractor extractor([&sink](const BYTE* data, std::size_t bytes) {
        sink.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(bytes));
    });

    std::uint64_t carrierBytes = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t fileBursts = 0;
    std::uint64_t fileBytes = 0;
    bool sawSubFormat = false;
    GUID subFormat = GUID_NULL;
    auto announced = std::chrono::steady_clock::now();
    const auto startTime = std::chrono::steady_clock::now();

    std::wcout << L"RECORD " << outputPath.wstring() << L"\n" << std::flush;

    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (!sawSubFormat) {
                sawSubFormat = true;
                subFormat = read.header.SubFormat;
                std::wcout << L"FORMAT " << SubformatName(subFormat) << L"\n"
                           << std::flush;
                if (!IsEqualGUID(subFormat, kDolbyMlpMat10) &&
                    !IsEqualGUID(subFormat, kDolbyMat20) &&
                    !IsEqualGUID(subFormat, kDolbyMat21Profile3)) {
                    std::wcout << L"STATUS not-mlp\n" << std::flush;
                }
            }
            if (carrierBytes != 0 &&
                read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
                // A dropped/inserted byte shifts the 16-bit word swap; discard the
                // misaligned tail and re-lock on the next IEC preamble.
                extractor.Reset();
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            carrierBytes += read.header.PayloadBytes;
            extractor.Feed(read.payload, read.header.PayloadBytes);
        }

        if (durationSeconds > 0.0) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double>(now - startTime).count();
            if (elapsed >= durationSeconds) break;
        }
        const Ec3Command command = ReadEc3Command();
        if (command.kind == Ec3CommandKind::Stop) break;
        if (command.kind == Ec3CommandKind::Rotate) {
            // The capture device stays open, so no carrier bytes are lost
            // across a track change; only the destination file changes.
            sink.flush();
            sink.close();
            sink.clear();
            sink.open(command.path, std::ios::binary | std::ios::trunc);
            if (!sink) {
                throw std::runtime_error("Unable to create the rotated MLP output file");
            }
            std::wcout << L"ROTATED " << command.path.wstring()
                       << L" bursts=" << (extractor.Bursts() - fileBursts)
                       << L" bytes=" << (extractor.PayloadBytes() - fileBytes)
                       << L"\n" << std::flush;
            fileBursts = extractor.Bursts();
            fileBytes = extractor.PayloadBytes();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= std::chrono::milliseconds(500)) {
            announced = now;
            std::wcout << L"STATUS bursts=" << extractor.Bursts()
                       << L" bytes=" << extractor.PayloadBytes()
                       << L" carrier=" << carrierBytes << L"\n" << std::flush;
        }

        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    extractor.Finish();
    sink.flush();
    sink.close();
    std::wcout << L"STOPPED bursts=" << extractor.Bursts()
               << L" bytes=" << extractor.PayloadBytes()
               << L" gaps=" << sequenceGaps << L"\n" << std::flush;
}

void StreamEc3ToPipe(const std::wstring& pipeName,
                     const DWORD pollMilliseconds) {
    const std::wstring fullName = L"\\\\.\\pipe\\" + pipeName;
    WinHandle pipe(CreateNamedPipeW(
        fullName.c_str(), PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        1024 * 1024, 1024 * 1024, 0, nullptr));
    if (!pipe.IsValid()) {
        throw std::runtime_error("Unable to create the E-AC3 delivery pipe");
    }
    std::wcout << L"PIPE " << fullName << L"\n" << std::flush;

    // The decoder attaches as a client, exactly like the PCM ring: the engine
    // owns the endpoint and the decoder only produces samples.
    if (!ConnectNamedPipe(pipe.Get(), nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        throw std::runtime_error("The E-AC3 delivery pipe never connected");
    }
    std::wcout << L"CONNECTED\n" << std::flush;

    auto writeAll = [&pipe](const BYTE* data, std::size_t bytes) {
        std::size_t offset = 0;
        while (offset < bytes) {
            DWORD written = 0;
            if (!WriteFile(pipe.Get(), data + offset,
                           static_cast<DWORD>(bytes - offset), &written,
                           nullptr)) {
                throw std::runtime_error("The E-AC3 delivery pipe closed");
            }
            offset += written;
        }
    };
    Ec3FrameExtractor extractor(writeAll);

    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);

    std::uint64_t carrierBytes = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t expectedSequence = 0;
    bool sawSubFormat = false;
    GUID subFormat = GUID_NULL;
    auto announced = std::chrono::steady_clock::now();

    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (!sawSubFormat) {
                sawSubFormat = true;
                subFormat = read.header.SubFormat;
                std::wcout << L"FORMAT " << SubformatName(subFormat) << L"\n"
                           << std::flush;
                if (!(IsEqualGUID(subFormat, kDolbyDigitalPlus) ||
                      IsEqualGUID(subFormat, kDolbyDigitalPlusAtmos))) {
                    std::wcout << L"STATUS not-eac3\n" << std::flush;
                }
            }
            if (carrierBytes != 0 &&
                read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            carrierBytes += read.header.PayloadBytes;
            extractor.Feed(read.payload, read.header.PayloadBytes);
        }

        const Ec3Command command = ReadEc3Command();
        if (command.kind == Ec3CommandKind::Stop) break;

        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= std::chrono::milliseconds(500)) {
            announced = now;
            std::wcout << L"STATUS frames=" << extractor.Frames()
                       << L" bytes=" << extractor.PayloadBytes()
                       << L" carrier=" << carrierBytes << L"\n"
                       << std::flush;
        }

        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    FlushFileBuffers(pipe.Get());
    std::wcout << L"STOPPED frames=" << extractor.Frames()
               << L" bytes=" << extractor.PayloadBytes()
               << L" gaps=" << sequenceGaps << L"\n" << std::flush;
}

void PrintEc3Status() {
    WinHandle device = OpenMatCaptureDevice();
    const MAT_CAPTURE_STATS first = QueryMatCaptureStats(device.Get());
    Sleep(200);
    const MAT_CAPTURE_STATS second = QueryMatCaptureStats(device.Get());

    const bool known = !IsEqualGUID(second.SubFormat, GUID_NULL);
    const bool eac3 = IsEqualGUID(second.SubFormat, kDolbyDigitalPlus) ||
                      IsEqualGUID(second.SubFormat, kDolbyDigitalPlusAtmos);
    const std::uint64_t flowing = second.TotalBytesWritten > first.TotalBytesWritten
                                      ? second.TotalBytesWritten - first.TotalBytesWritten
                                      : 0;

    std::wcout << L"STATUS known=" << (known ? 1 : 0)
               << L" eac3=" << (eac3 ? 1 : 0)
               << L" subtype=" << SubformatName(second.SubFormat)
               << L" flowing=" << flowing << L"\n";
}

void CaptureMatRing(const double seconds, const std::filesystem::path& outputPath,
                    const DWORD pollMilliseconds) {
    CaptureIec61937Ring(seconds, outputPath, pollMilliseconds);
}

} // namespace dolby
