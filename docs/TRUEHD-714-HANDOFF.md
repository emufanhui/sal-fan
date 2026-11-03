# TrueHD / MAT 7.1.4 handoff

## Current result

This branch adds an offline capture-to-Atmos-7.1.4 reference workflow. It does **not** yet add a live TrueHD decoder to the Studio bridge.

Two previously uploaded test files were inspected:

- ring_truehd.mat: 30,712,000 bytes = 500 × 61,424-byte MAT payload frames. All 500 frames passed the fixed START, MIDDLE, and END marker checks.
- ring_truehd_experimental.thd: FFprobe identifies TrueHD, profile Dolby TrueHD + Dolby Atmos, 48 kHz, 8 channels / 7.1. FFmpeg decoded about 5.18 seconds to PCM. This validates a real TrueHD AU stream, not 7.1.4 object rendering.

## Existing capture path

SpatialAudioLab.MatRecorder is the standalone reader for \\.\DolbyDecoderMat:

1. Open the existing MAT capture device.
2. Read the existing IOCTL_MAT_CAPTURE_READ ring; no driver/IOCTL changes are required.
3. Find IEC 61937 preambles 72 F8 1F 4E.
4. Accept only Pc=0x0016 (MAT) and Pd=61424.
5. Strip the 8-byte IEC preamble and word-swap the 61,424-byte payload.
6. Write the complete MAT payload frame, including its MAT framing markers.

That output is a MAT payload file, not yet a TrueHD elementary stream. Use .mat as its extension. The existing recorder UI's .mlp extension is misleading for this intermediate.

## MAT payload -> TrueHD AU stream

mat_truehd_extractor_v03.py is the current experimental extractor for the observed 4-substream FBA capture:

1. Require file size to be a multiple of 61,424.
2. Validate each frame's fixed START marker at offset 0, MIDDLE marker at 30,708, and END marker at 61,408.
3. Remove 20-byte START + 12-byte MIDDLE + 16-byte END markers. Each MAT payload frame contributes 61,376 bytes to the de-framed scan buffer.
4. Scan and validate TrueHD access units using AU length, parity, and CRC checks.
5. Select a non-overlapping AU sequence beginning at the first valid Major Sync and write those AUs contiguously to .thd.

Important: simply stripping the IEC header, swapping words, and concatenating every 61,424-byte MAT payload is not the final .thd elementary stream. MAT padding and framing remain. The v0.3 AU selection stage is required for the observed capture. It currently rejects substream layouts other than the observed four-substream layout; do not claim general MAT compatibility.

## Offline decode and render

The new Prepare-TrueHdAtmos714.ps1 accepts:
- .mat or .mlp: complete MAT payload frames emitted by SpatialAudioLab.MatRecorder.
- .thd: already extracted TrueHD access units.

It runs:
1. mat_truehd_extractor_v03.py when input is MAT payload.
2. truehdd info for validation.
3. truehdd decode --presentation 3 to create the Dolby Atmos Master Format files (.atmos, .atmos.audio, .atmos.metadata).
4. Cavern714 to render the Atmos master to 12-channel ref714 PCM16 WAV.

Commands:

~~~powershell
# One-time build of the standalone capture CLI (does not build/install the driver)
.\tools\Build-MatRecorder.ps1

# Start TrueHD bitstream playback, then capture 30 seconds and render 7.1.4
.\tools\Capture-TrueHdAtmos714.ps1 -DurationSeconds 30 -KeepIntermediate

# Or process an existing MAT payload capture
.\tools\Prepare-TrueHdAtmos714.ps1 -InputFile .\captures\ring_truehd.mat -KeepIntermediate

# Or process the already extracted AU stream
.\tools\Prepare-TrueHdAtmos714.ps1 -InputFile .\captures\ring_truehd_experimental.thd -KeepIntermediate
~~~

Expected output: captures\truehd-mat-<timestamp>-714.wav or <OutputBase>-714.wav, 12 channels, 7.1.4. The current workflow is file-based and therefore has capture/decode latency; it is a functional checkpoint, not a live bridge.

## Live Studio integration still required

The existing E-AC-3 bridge is:

CLI ec3-pipe -> Eac3Pipe714 (Cavern decode/object render) -> Local\SAL_EAC3_PCM714 (12ch float32) -> SpatialAudioLab.CLI live-eac3-layout.

The equivalent TrueHD path needs:

MAT capture -> correct MAT de-framing + AU extraction -> truehdd streaming parser/decoder -> OAMD/object rendering -> the same 12ch float32 PCM ring -> live-eac3-layout or a format-neutral renamed consumer.

Do not send TrueHD to Cavern.Format.MeridianLosslessPackingDecoder: that implementation currently returns an empty frame and explicitly says MLP is unsupported. Use the third_party/truehdd-src/truehd parser/decoder for TrueHD. Its decoded access units contain 24-bit PCM, channel labels, and OAMD payloads. A live wrapper must preserve decoder/parser state across access units and reset both together after a fatal parse/decode error.

The repository has a RuntimePaths.TrueHdExecutable path for Tools\truehdd\truehd-stream.exe, but the corresponding streaming executable/source is not present in the tracked tree. The bundled truehdd CLI reads stdin but writes completed decode outputs to files; it is not itself the live PCM bridge.

## Integration defects to fix next

- MatRecorderService / MatRecorderController exist but are not wired into Ec3Recorder/MainWindow.xaml.cs.
- MatRecorderService expects mat-status and mat-recorder; those commands belong to SpatialAudioLab.MatRecorder.exe, while CliLocator.Find() currently locates SpatialAudioLab.CLI.exe. A future UI integration must resolve the standalone recorder executable separately.
- src/mat_capture_client.cpp contains a separate MatMlpExtractor used by mat-pipe / mat-record; its existing 32-byte stripping logic is not equivalent to the validated v0.3 MAT marker removal + AU extraction path. Do not use its output as a verified .thd until it is replaced and tested against the sample.
- The new Cavern714 project and PowerShell pipeline are not built or executed in this Linux tool environment. Build/run them on the Windows development machine and retain the generated .mat, .thd, .atmos*, and WAV files for comparison.
- No driver source, IOCTL contract, ring protocol, driver package, or installed driver was changed by this branch.

## Acceptance checklist

- [ ] MAT recorder reports nonzero valid bursts and zero sequence gaps.
- [ ] .mat size is divisible by 61,424; all MAT markers validate.
- [ ] v0.3 AU extractor reports validated Major Sync and AU candidates; truehdd info succeeds.
- [ ] truehdd decode --presentation 3 produces .atmos and metadata.
- [ ] Cavern714 produces a 12-channel WAV with nonzero RMS in the expected height channels.
- [ ] Compare the output against the existing E-AC-3 Eac3Pipe714 12-channel ring format.
- [ ] Only after offline validation, implement and test the live TrueHD producer for Studio.
