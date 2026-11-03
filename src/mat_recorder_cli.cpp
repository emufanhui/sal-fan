// Standalone MAT (Dolby MAT / TrueHD) recorder CLI for SpatialAudioLab.
//
// Commands
//   mat-status [probe-ms]        Non-destructive gate probe. Uses
//                                IOCTL_MAT_CAPTURE_GET_STATS (read-only, never
//                                consumes the ring) and reports the MAT-specific
//                                status fields the auto-gater needs.
//   mat-recorder <out.mlp> [poll-ms]
//                                Long-lived recorder. Becomes the SOLE consumer
//                                of the capture ring (IOCTL_MAT_CAPTURE_READ) and
//                                de-MATs the IEC 61937/MAT 1.0 carrier into the
//                                bare MLP/TrueHD substream. Controlled by STOP /
//                                ROTATE lines on stdin.
//
// Design notes
//   * The gate (mat-status) and the recorder (mat-recorder) never compete for the
//     ring: the gate reads statistics, the recorder reads data. A gate probe may
//     even run while the recorder is active without disturbing it.
//   * MAT framing validation mirrors extract_mat.py (the authoritative WAV->THD
//     reference): Pa=0xF872, Pb=0x4E1F, Pc=0x0016 (TrueHD/MAT), Pd=61424. A burst
//     with Pc==0x0000 is a NULL burst and is never written as MLP (it cannot be
//     mistaken for valid TrueHD).
//   * The de-MAT keeps the 24-byte MAT substream header (it IS the TrueHD frame
//     header) and only strips the 8-byte IEC61937 preamble, then word-swaps every
//     16-bit word -- exactly what extract_mat.py does. This yields a TrueHD
//     elementary stream (.mlp/.thd) that validates against MAT_START/MIDDLE/END.
//     (Stripping 32 bytes, as MatMlpExtractor does, drops the frame header and
//     fails TrueHD parity -- do not replicate that mistake.)

#include <windows.h>
#include <devioctl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../driver/windows-driver-samples/audio/sysvad/matcapture.h"

namespace {

// IEC 61937 Dolby MAT burst markers (little-endian 16-bit samples in the carrier).
constexpr std::uint16_t kIecPa = 0xF872;
constexpr std::uint16_t kIecPb = 0x4E1F;
constexpr std::uint16_t kPcMat = 0x0016;   // TrueHD / MAT data type
constexpr std::uint16_t kPcNull = 0x0000;  // NULL burst (silent carrier)

// Self-contained zero GUID (avoids depending on initguid/guiddef include order).
static const GUID kNullGuid = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

// IEC 61937 / MAT burst geometry (from extract_mat.py). Only the 8-byte IEC
// preamble is stripped; the following 24-byte MAT substream header is part of the
// TrueHD frame and is kept. Each burst carries MAT_SIZE (61424) payload bytes and
// the bursts are BURST_STRIDE (61440) bytes apart in the carrier.
constexpr std::size_t kIecHeaderBytes = 8;
constexpr std::size_t kMatPayloadBytes = 61424; // Pd
constexpr std::size_t kBurstsStride = 61440;    // BURST_SIZE

// MAT-family subformat GUIDs (from audio_platform.h). E-AC-3 / DTS are NOT here.
constexpr GUID kDolbyMlpMat10 = {
    0x0000000c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kDolbyMat20 = {
    0x0000010c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kDolbyMat21Profile3 = {
    0x0000030c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kDolbyMat21Profile4 = {
    0x0000070c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool IsMatSubFormat(const GUID& g) {
    return IsEqualGUID(g, kDolbyMlpMat10) || IsEqualGUID(g, kDolbyMat20) ||
           IsEqualGUID(g, kDolbyMat21Profile3) || IsEqualGUID(g, kDolbyMat21Profile4);
}

std::wstring SubformatName(const GUID& g) {
    if (IsEqualGUID(g, kDolbyMlpMat10)) return L"Dolby MLP / MAT 1.0";
    if (IsEqualGUID(g, kDolbyMat20)) return L"Dolby MAT 2.0";
    if (IsEqualGUID(g, kDolbyMat21Profile3)) return L"Dolby MAT 2.1 Profile 3";
    if (IsEqualGUID(g, kDolbyMat21Profile4)) return L"Dolby MAT 2.1 Profile 4";
    if (IsEqualGUID(g, kNullGuid)) return L"(none)";
    wchar_t buf[40];
    swprintf(buf, static_cast<std::size_t>(40),
             L"%08X-%04X-%04X", g.Data1, g.Data2, g.Data3);
    return std::wstring(buf);
}

// ---- Legacy (protocol v2) structs, kept local for driver compatibility. ----
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

HANDLE OpenDevice() {
    HANDLE device = CreateFileW(MAT_CAPTURE_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "Could not open \\\\.\\DolbyDecoderMat (Win32 " + std::to_string(GetLastError()) +
            "). Install and load the capture driver as administrator.");
    }
    return device;
}

void ResetCapture(HANDLE device) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_RESET, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        throw std::runtime_error("IOCTL_MAT_CAPTURE_RESET failed (Win32 " +
                                 std::to_string(GetLastError()) + ')');
    }
}

MAT_CAPTURE_STATS QueryStats(HANDLE device) {
    std::array<BYTE, sizeof(MAT_CAPTURE_STATS)> response{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_GET_STATS, nullptr, 0, response.data(),
                         static_cast<DWORD>(response.size()), &returned, nullptr)) {
        throw std::runtime_error("IOCTL_MAT_CAPTURE_GET_STATS failed (Win32 " +
                                 std::to_string(GetLastError()) + ')');
    }
    if (returned < sizeof(ULONG)) {
        throw std::runtime_error("Capture driver returned truncated stats");
    }
    const ULONG version = *reinterpret_cast<const ULONG*>(response.data());
    if (version == MAT_CAPTURE_PROTOCOL_VERSION && returned == sizeof(MAT_CAPTURE_STATS)) {
        return *reinterpret_cast<const MAT_CAPTURE_STATS*>(response.data());
    }
    if (version == MAT_CAPTURE_PROTOCOL_VERSION_IEC61937 && returned == sizeof(MatCaptureStatsV2)) {
        const auto& v2 = *reinterpret_cast<const MatCaptureStatsV2*>(response.data());
        MAT_CAPTURE_STATS stats{};
        stats.Version = v2.version;
        stats.CapacityBytes = v2.capacityBytes;
        stats.AvailableBytes = v2.availableBytes;
        stats.Reserved = v2.reserved;
        stats.TotalBytesWritten = v2.totalBytesWritten;
        stats.TotalBytesRead = v2.totalBytesRead;
        stats.DroppedBytes = v2.droppedBytes;
        stats.SubFormat = v2.subFormat;
        stats.FormatChanges = v2.formatChanges;
        return stats;
    }
    throw std::runtime_error("Capture driver returned incompatible stats (version=" +
                             std::to_string(version) + ", bytes=" + std::to_string(returned) + ')');
}

struct ReadResult {
    MAT_CAPTURE_READ_HEADER header{};
    const BYTE* payload{};
};

ReadResult ReadCapture(HANDLE device, std::vector<BYTE>& request) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_READ, nullptr, 0, request.data(),
                         static_cast<DWORD>(request.size()), &returned, nullptr)) {
        throw std::runtime_error("IOCTL_MAT_CAPTURE_READ failed (Win32 " +
                                 std::to_string(GetLastError()) + ')');
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
        const auto& v2 = *reinterpret_cast<const MatCaptureReadHeaderV2*>(request.data());
        if (v2.headerBytes < sizeof(MatCaptureReadHeaderV2) ||
            static_cast<std::uint64_t>(v2.headerBytes) + v2.payloadBytes > returned) {
            throw std::runtime_error("Capture driver returned an invalid v2 read result");
        }
        MAT_CAPTURE_READ_HEADER header{};
        header.Version = v2.version;
        header.HeaderBytes = v2.headerBytes;
        header.PayloadBytes = v2.payloadBytes;
        header.FirstByteSequence = v2.firstByteSequence;
        header.TotalBytesWritten = v2.totalBytesWritten;
        header.DroppedBytes = v2.droppedBytes;
        header.SubFormat = v2.subFormat;
        header.FormatChanges = v2.formatChanges;
        return {header, request.data() + v2.headerBytes};
    }
    throw std::runtime_error("Capture driver returned an unsupported protocol version");
}

// Scans IEC 61937 Dolby bursts, classifies each by Pc, and de-MATs only the
// genuine MAT (Pc==0x0016) bursts. NULL (Pc==0) and other bursts are counted but
// never emitted as MLP.
class MatRecorder {
public:
    using Sink = std::function<void(const BYTE*, std::size_t)>;

    explicit MatRecorder(Sink sink) : sink_(std::move(sink)) {}

    void Feed(const BYTE* data, std::size_t bytes) {
        pending_.insert(pending_.end(), data, data + bytes);
        Drain();
    }

    // Flush any trailing burst that has no following preamble (end of stream).
    void Finish() {
        FlushTail();
        pending_.clear();
    }

    void Reset() { pending_.clear(); }

    std::uint64_t ValidBursts() const noexcept { return validBursts_; }
    std::uint64_t NullBursts() const noexcept { return nullBursts_; }
    std::uint64_t OtherBursts() const noexcept { return otherBursts_; }
    std::uint64_t PayloadBytes() const noexcept { return payloadBytes_; }

private:
    static constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);

    std::size_t FindSync(std::size_t from) const {
        for (std::size_t i = from; i + 5 < pending_.size(); ++i) {
            if (pending_[i] == 0x72 && pending_[i + 1] == 0xF8 && pending_[i + 2] == 0x1F &&
                pending_[i + 3] == 0x4E) {
                return i;
            }
        }
        return kNoMatch;
    }

    void Drain() {
        std::size_t from = 0;
        while (true) {
            const std::size_t p = FindSync(from);
            if (p == kNoMatch) break;
            // Need the full 8-byte IEC preamble plus the 61424-byte MAT payload
            // before we can emit this burst.
            if (p + kIecHeaderBytes + kMatPayloadBytes > pending_.size()) {
                from = p;
                break;
            }
            const std::uint16_t pc = PcAt(p);
            const std::uint16_t pd = PdAt(p);
            // The next burst starts after this payload; re-lock on its sync rather
            // than assuming a fixed stride, so a dropped byte cannot desync us.
            const std::size_t q = FindSync(p + kIecHeaderBytes + kMatPayloadBytes);
            if (q == kNoMatch) {
                from = p;
                break;
            }
            if (pc == kPcMat && pd == kMatPayloadBytes) {
                Emit(p + kIecHeaderBytes, kMatPayloadBytes);
                ++validBursts_;
            } else if (pc == kPcNull) {
                ++nullBursts_;
            } else {
                ++otherBursts_;
            }
            from = q;
        }
        Trim(from);
    }

    void FlushTail() {
        std::size_t from = 0;
        while (true) {
            const std::size_t p = FindSync(from);
            if (p == kNoMatch) break;
            // Emit only complete trailing bursts (a partial frame is invalid TrueHD).
            if (p + kIecHeaderBytes + kMatPayloadBytes <= pending_.size() &&
                PcAt(p) == kPcMat && PdAt(p) == kMatPayloadBytes) {
                Emit(p + kIecHeaderBytes, kMatPayloadBytes);
            }
            const std::size_t q = FindSync(p + kIecHeaderBytes + kMatPayloadBytes);
            if (q == kNoMatch) break;
            from = q;
        }
    }

    std::uint16_t PcAt(std::size_t p) const {
        return static_cast<std::uint16_t>(pending_[p + 4] | (pending_[p + 5] << 8));
    }

    std::uint16_t PdAt(std::size_t p) const {
        return static_cast<std::uint16_t>(pending_[p + 6] | (pending_[p + 7] << 8));
    }

    // Strip the 8-byte IEC61937 preamble and byte-swap every 16-bit word to recover
    // the TrueHD/MAT elementary stream (matches extract_mat.py's swap_words).
    void Emit(std::size_t offset, std::size_t bytes) {
        const std::size_t n = bytes & ~static_cast<std::size_t>(1);
        swapped_.clear();
        swapped_.reserve(n);
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            swapped_.push_back(pending_[offset + i + 1]);
            swapped_.push_back(pending_[offset + i]);
        }
        if (!swapped_.empty()) {
            sink_(swapped_.data(), swapped_.size());
            payloadBytes_ += swapped_.size();
        }
    }

    void Trim(std::size_t from) {
        if (from > 0) pending_.erase(pending_.begin(), pending_.begin() + from);
        constexpr std::size_t kKeep = 8 + 61440 * 2;
        if (pending_.size() > kKeep) {
            pending_.erase(pending_.begin(), pending_.end() - kKeep);
        }
    }

    Sink sink_;
    std::vector<BYTE> pending_;
    std::vector<BYTE> swapped_;
    std::uint64_t validBursts_ = 0;
    std::uint64_t nullBursts_ = 0;
    std::uint64_t otherBursts_ = 0;
    std::uint64_t payloadBytes_ = 0;
};

// ---- stdin control: STOP / ROTATE <path> (mirrors ReadEc3Command). ----
enum class RecCommandKind { None, Stop, Rotate };

struct RecCommand {
    RecCommandKind kind = RecCommandKind::None;
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

RecCommand ReadCommand() {
    static std::string buffer;

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) return {};

    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return {RecCommandKind::Stop, {}};
        return {};
    }
    if (available == 0) return {};

    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!ReadFile(input, chunk.data(), available, &read, nullptr) || read == 0) return {};
    buffer.append(chunk.data(), read);

    std::size_t newline = buffer.find('\n');
    while (newline != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line == "STOP") return {RecCommandKind::Stop, {}};
        constexpr char kRotatePrefix[] = "ROTATE ";
        if (line.rfind(kRotatePrefix, 0) == 0) {
            return {RecCommandKind::Rotate, Utf8ToPath(line.substr(sizeof(kRotatePrefix) - 1))};
        }
        newline = buffer.find('\n');
    }
    return {};
}

// Minimal RAII wrapper around a HANDLE (kept local to avoid pulling audio_platform).
struct WinHandleRAII {
    explicit WinHandleRAII(HANDLE h) : h_(h) {}
    ~WinHandleRAII() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    HANDLE Get() const { return h_; }
    WinHandleRAII(const WinHandleRAII&) = delete;
    WinHandleRAII& operator=(const WinHandleRAII&) = delete;
    HANDLE h_;
};

void RunStatus(int probeMs) {
    if (probeMs <= 0) probeMs = 200;
    if (probeMs > 5000) probeMs = 5000;

    const WinHandleRAII device(OpenDevice());
    const MAT_CAPTURE_STATS first = QueryStats(device.Get());
    Sleep(probeMs);
    const MAT_CAPTURE_STATS second = QueryStats(device.Get());

    const bool known = !IsEqualGUID(second.SubFormat, kNullGuid);
    const bool mat = IsMatSubFormat(second.SubFormat);
    const std::uint64_t flowing =
        second.TotalBytesWritten > first.TotalBytesWritten
            ? second.TotalBytesWritten - first.TotalBytesWritten
            : 0;

    std::wcout << L"STATUS known=" << (known ? 1 : 0) << L" mat=" << (mat ? 1 : 0)
               << L" subtype=" << SubformatName(second.SubFormat) << L" flowing=" << flowing
               << L" bytesPerProbe=" << flowing << L" droppedBytes=" << second.DroppedBytes
               << L" formatChanges=" << second.FormatChanges << L"\n"
               << std::flush;
}

void RunRecorder(const std::filesystem::path& outputPath, DWORD pollMilliseconds) {
    const WinHandleRAII device(OpenDevice());
    ResetCapture(device.Get());

    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);

    std::ofstream sink(outputPath, std::ios::binary | std::ios::trunc);
    if (!sink) throw std::runtime_error("Unable to create the MLP output file");

    MatRecorder recorder([&sink](const BYTE* data, std::size_t bytes) {
        sink.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    });

    std::uint64_t carrierBytes = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t fileValid = 0;
    std::uint64_t fileBytes = 0;
    std::uint64_t fileNull = 0;
    std::uint64_t fileOther = 0;
    bool sawSubFormat = false;
    auto announced = std::chrono::steady_clock::now();

    std::wcout << L"RECORD " << outputPath.wstring() << L"\n" << std::flush;

    while (true) {
        const ReadResult read = ReadCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (!sawSubFormat) {
                sawSubFormat = true;
                std::wcout << L"FORMAT " << SubformatName(read.header.SubFormat) << L"\n"
                           << std::flush;
            }
            if (carrierBytes != 0 && read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
                // A dropped/inserted byte shifts the 16-bit word swap; discard the
                // misaligned tail and re-lock on the next IEC preamble.
                recorder.Reset();
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            carrierBytes += read.header.PayloadBytes;
            recorder.Feed(read.payload, read.header.PayloadBytes);
        }

        const RecCommand command = ReadCommand();
        if (command.kind == RecCommandKind::Stop) break;
        if (command.kind == RecCommandKind::Rotate) {
            sink.flush();
            sink.close();
            sink.clear();
            sink.open(command.path, std::ios::binary | std::ios::trunc);
            if (!sink) throw std::runtime_error("Unable to create the rotated MLP output file");
            std::wcout << L"ROTATED " << command.path.wstring() << L" valid="
                       << (recorder.ValidBursts() - fileValid) << L" bytes="
                       << (recorder.PayloadBytes() - fileBytes) << L" null="
                       << (recorder.NullBursts() - fileNull) << L" other="
                       << (recorder.OtherBursts() - fileOther) << L"\n"
                       << std::flush;
            fileValid = recorder.ValidBursts();
            fileBytes = recorder.PayloadBytes();
            fileNull = recorder.NullBursts();
            fileOther = recorder.OtherBursts();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= std::chrono::milliseconds(500)) {
            announced = now;
            std::wcout << L"STATUS bursts=" << recorder.ValidBursts() << L" bytes="
                       << recorder.PayloadBytes() << L" carrier=" << carrierBytes << L" null="
                       << recorder.NullBursts() << L" other=" << recorder.OtherBursts()
                       << L" dropped=" << read.header.DroppedBytes << L" formatChanges="
                       << read.header.FormatChanges << L"\n"
                       << std::flush;
        }

        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    recorder.Finish();
    sink.flush();
    sink.close();
    std::wcout << L"STOPPED bursts=" << recorder.ValidBursts() << L" bytes="
               << recorder.PayloadBytes() << L" null=" << recorder.NullBursts() << L" other="
               << recorder.OtherBursts() << L" gaps=" << sequenceGaps << L"\n"
               << std::flush;
}

void PrintUsage() {
    std::wcerr << L"Usage:\n"
               << L"  SpatialAudioLab.MatRecorder mat-status [probe-ms]\n"
               << L"  SpatialAudioLab.MatRecorder mat-recorder <out.mlp> [poll-ms]\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc < 2) {
            PrintUsage();
            return 1;
        }
        const std::wstring command = argv[1];
        if (command == L"mat-status") {
            const int probeMs = argc >= 3 ? std::stoi(std::wstring(argv[2])) : 200;
            RunStatus(probeMs);
            return 0;
        }
        if (command == L"mat-recorder") {
            if (argc < 3) throw std::runtime_error("mat-recorder requires an output .mlp path");
            const std::filesystem::path output = argv[2];
            const DWORD pollMilliseconds =
                argc >= 4 ? static_cast<DWORD>(std::stoul(std::wstring(argv[3]))) : 2;
            if (pollMilliseconds == 0 || pollMilliseconds > 1000) {
                throw std::runtime_error("mat-recorder requires 0 < poll-ms <= 1000");
            }
            RunRecorder(output, pollMilliseconds);
            return 0;
        }
        PrintUsage();
        return 1;
    } catch (const std::exception& e) {
        std::wcerr << L"ERROR: " << e.what() << L"\n";
        return 1;
    }
}
