#pragma once

#include "audio_platform.h"

#include "../driver/windows-driver-samples/audio/sysvad/matcapture.h"

#include <filesystem>
#include <vector>

namespace dolby {

struct MatCaptureReadView {
    MAT_CAPTURE_READ_HEADER header{};
    const BYTE* payload{};
};

MAT_CAPTURE_STATS QueryMatCaptureStats(HANDLE device);
WinHandle OpenMatCaptureDevice();
void ResetMatCapture(HANDLE device);
MatCaptureReadView ReadMatCapture(HANDLE device, std::vector<BYTE>& request);
void ResetDriverFormatLog();
void PrintDriverFormatLog();
void CaptureMatRing(double seconds, const std::filesystem::path& outputPath,
                    DWORD pollMilliseconds);
void CaptureIec61937Ring(double seconds, const std::filesystem::path& outputPath,
                        DWORD pollMilliseconds);
/// Streams E-AC3 syncframes straight to .ec3 until the parent writes to stdin.
void RecordEc3Stream(const std::filesystem::path& outputPath,
                     DWORD pollMilliseconds);
/// Streams the same bare E-AC3 syncframes into a named pipe so a decoder can
/// render them live. `pipeName` is the bare name, without the \\.\pipe\ prefix.
void StreamEc3ToPipe(const std::wstring& pipeName, DWORD pollMilliseconds);
/// De-MAT's the live IEC 61937/MLP (MAT 1.0) carrier into the bare MLP/TrueHD
/// substream and records it to a .mlp file until the parent writes STOP/ROTATE
/// to stdin. The recorded bytes are exactly what `mat-pipe` would push into the
/// decoder pipe, so the file can be validated offline with truehdd/cavern.
void RecordMatMlpStream(const std::filesystem::path& outputPath,
                        DWORD pollMilliseconds,
                        double durationSeconds = 0.0);
/// De-MAT's the live IEC 61937/MLP (MAT 1.0) carrier into the bare MLP/TrueHD
/// substream and streams it into a named pipe so truehd-stream can decode it
/// live. pipeName is the bare name, without the prefix.
void StreamMatToPipe(const std::wstring& pipeName, DWORD pollMilliseconds);
/// Offline de-MAT: reads a raw IEC 61937-12 MAT carrier file (as captured by the
/// diagnostic ring dump) and writes the recovered bare MLP/TrueHD substream.
/// Reuses the exact same extraction as the live path so behaviour matches.
void DemuxRawCarrier(const std::filesystem::path& rawPath,
                     const std::filesystem::path& outPath);
/// Reports the driver's current IEC 61937 subtype without consuming ring data.
void PrintEc3Status();

} // namespace dolby
