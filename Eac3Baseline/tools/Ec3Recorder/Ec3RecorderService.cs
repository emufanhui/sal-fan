using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Ec3Recorder;

internal sealed record StreamStatus(bool Known, bool IsEac3, string Subtype, long BytesPerProbe)
{
    /// A subtype alone only says what the driver last negotiated. "Flowing"
    /// counts bytes the driver wrote during the probe, which tells us the
    /// stream is live right now rather than a stale negotiation.
    public bool IsLive => Known && BytesPerProbe > 0;
}

internal sealed record RecordProgress(long Frames, long Bytes, long CarrierBytes);

/// Drives the CLI in two modes: a short-lived status probe, and a long-lived
/// ec3-record child that is told to finish by a line on its stdin.
internal sealed class Ec3RecorderService
{
    private static readonly Regex KnownPattern = new(@"known=(\d+)", RegexOptions.Compiled);
    private static readonly Regex Eac3Pattern = new(@"eac3=(\d+)", RegexOptions.Compiled);
    private static readonly Regex FlowingPattern = new(@"flowing=(\d+)", RegexOptions.Compiled);
    private static readonly Regex SubtypePattern = new(@"subtype=(.*?)\s+flowing=", RegexOptions.Compiled);
    private static readonly Regex FramesPattern = new(@"frames=(\d+)", RegexOptions.Compiled);
    private static readonly Regex BytesPattern = new(@"bytes=(\d+)", RegexOptions.Compiled);
    private static readonly Regex CarrierPattern = new(@"carrier=(\d+)", RegexOptions.Compiled);

    private Process? _recorder;

    public event Action<RecordProgress>? Progress;
    public event Action<string>? FormatDetected;
    public event Action<string>? Line;
    public event Action? RecordingExited;

    public bool IsRecording => _recorder is { HasExited: false };

    public static async Task<StreamStatus?> QueryStatus(string cliPath, CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo(cliPath, "ec3-status")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            // The CLI writes through std::wcout, which narrows to single-byte
            // characters when its stdout is a pipe. Decoding that as UTF-16
            // turns every token into garbage and every regex silently fails.
            StandardOutputEncoding = Encoding.UTF8
        };

        using var process = Process.Start(startInfo);
        if (process is null) return null;

        string output = await process.StandardOutput.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);

        var known = KnownPattern.Match(output);
        var eac3 = Eac3Pattern.Match(output);
        var flowing = FlowingPattern.Match(output);
        var subtype = SubtypePattern.Match(output);
        if (!known.Success || !eac3.Success || !flowing.Success) return null;

        return new StreamStatus(
            known.Groups[1].Value == "1",
            eac3.Groups[1].Value == "1",
            subtype.Success ? subtype.Groups[1].Value.Trim() : string.Empty,
            long.Parse(flowing.Groups[1].Value, CultureInfo.InvariantCulture));
    }

    public bool Start(string cliPath, string outputPath)
    {
        if (IsRecording) return false;

        var startInfo = new ProcessStartInfo(cliPath, $"ec3-record \"{outputPath}\" 2")
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            // The CLI writes through std::wcout, which narrows to single-byte
            // characters when its stdout is a pipe. Decoding that as UTF-16
            // turns every token into garbage and every regex silently fails.
            StandardOutputEncoding = Encoding.UTF8,
            // ROTATE carries a file path, which may be non-ASCII. A BOM must
            // not be emitted: StreamWriter only writes the preamble on the very
            // first write, so the first ROTATE would be prefixed with EF BB BF
            // and the recorder would silently treat it as an unknown command.
            StandardInputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false)
        };

        var process = Process.Start(startInfo);
        if (process is null) return false;

        _recorder = process;
        process.EnableRaisingEvents = true;
        process.OutputDataReceived += (_, args) =>
        {
            if (args.Data is { Length: > 0 } text) HandleLine(text);
        };
        process.ErrorDataReceived += (_, args) =>
        {
            if (args.Data is { Length: > 0 } text) Line?.Invoke(text);
        };
        process.Exited += (_, _) => RecordingExited?.Invoke();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        return true;
    }

    /// Closes the current output file and continues the same capture stream
    /// into `path`. The capture device is never reopened, so no carrier bytes
    /// are lost at the boundary.
    public bool Rotate(string path)
    {
        var process = _recorder;
        if (process is null || process.HasExited) return false;
        try
        {
            process.StandardInput.Write($"ROTATE {path}\n");
            process.StandardInput.Flush();
            return true;
        }
        catch (IOException)
        {
            return false;
        }
    }

    public void Stop()
    {
        var process = _recorder;
        _recorder = null;
        if (process is null || process.HasExited) return;

        // The child treats any byte on stdin as the stop signal.
        try
        {
            process.StandardInput.Write("STOP\n");
            process.StandardInput.Flush();
        }
        catch (IOException)
        {
        }
    }

    private void HandleLine(string text)
    {
        Line?.Invoke(text);

        if (text.StartsWith("FORMAT ", StringComparison.Ordinal))
        {
            FormatDetected?.Invoke(text["FORMAT ".Length..]);
            return;
        }

        if (!text.StartsWith("STATUS ", StringComparison.Ordinal)) return;

        var frames = FramesPattern.Match(text);
        var bytes = BytesPattern.Match(text);
        var carrier = CarrierPattern.Match(text);
        if (!frames.Success || !bytes.Success || !carrier.Success) return;

        Progress?.Invoke(new RecordProgress(
            long.Parse(frames.Groups[1].Value, CultureInfo.InvariantCulture),
            long.Parse(bytes.Groups[1].Value, CultureInfo.InvariantCulture),
            long.Parse(carrier.Groups[1].Value, CultureInfo.InvariantCulture)));
    }
}
