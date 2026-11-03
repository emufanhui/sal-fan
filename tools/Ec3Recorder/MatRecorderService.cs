using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Ec3Recorder;

/// <summary>
/// MAT (Dolby MAT / TrueHD) stream status. Mirrors the E-AC-3 <see cref="StreamStatus"/>
/// but with MAT-specific gating: a subtype alone only proves format negotiation;
/// <see cref="IsLive"/> additionally requires the MAT-family subtype and live bytes.
/// </summary>
internal sealed record MatStreamStatus(
    bool Known, bool IsMat, string Subtype, long BytesPerProbe, long DroppedBytes, long FormatChanges)
{
    /// A real MAT carrier is both negotiated and flowing into SysVAD right now.
    public bool IsLive => Known && IsMat && BytesPerProbe > 0;
}

internal sealed record MatRecordProgress(
    long ValidBursts, long Bytes, long CarrierBytes, long NullBursts, long OtherBursts,
    long DroppedBytes, long FormatChanges);

/// <summary>
/// Drives the MAT recorder CLI in two modes: a short-lived non-destructive status
/// probe (<c>mat-status</c>), and a long-lived <c>mat-recorder</c> child that is
/// told to finish by a line on its stdin. This mirrors <see cref="Ec3RecorderService"/>
/// exactly, only the command strings and the status grammar differ.
/// </summary>
internal sealed class MatRecorderService
{
    private static readonly Regex KnownPattern = new(@"known=(\d+)", RegexOptions.Compiled);
    private static readonly Regex MatPattern = new(@"mat=(\d+)", RegexOptions.Compiled);
    private static readonly Regex FlowingPattern = new(@"flowing=(\d+)", RegexOptions.Compiled);
    private static readonly Regex SubtypePattern = new(@"subtype=(.*?)\s+flowing=", RegexOptions.Compiled);
    private static readonly Regex DroppedPattern = new(@"droppedBytes=(\d+)", RegexOptions.Compiled);
    private static readonly Regex FormatChangesPattern = new(@"formatChanges=(\d+)", RegexOptions.Compiled);
    private static readonly Regex ValidPattern = new(@"bursts=(\d+)", RegexOptions.Compiled);
    private static readonly Regex BytesPattern = new(@"bytes=(\d+)", RegexOptions.Compiled);
    private static readonly Regex CarrierPattern = new(@"carrier=(\d+)", RegexOptions.Compiled);
    private static readonly Regex NullPattern = new(@"null=(\d+)", RegexOptions.Compiled);
    private static readonly Regex OtherPattern = new(@"other=(\d+)", RegexOptions.Compiled);
    // The recorder prints "dropped=" (not "droppedBytes=" like the gate probe).
    private static readonly Regex RecDroppedPattern = new(@"dropped=(\d+)", RegexOptions.Compiled);

    private Process? _recorder;

    public event Action<MatRecordProgress>? Progress;
    public event Action<string>? FormatDetected;
    public event Action<string>? Line;
    public event Action? RecordingExited;

    public bool IsRecording => _recorder is { HasExited: false };

    /// <summary>Runs the non-destructive gate probe and parses the MAT status.</summary>
    public static async Task<MatStreamStatus?> QueryStatus(string cliPath, CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo(cliPath, "mat-status 200")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            // The CLI writes through std::wcout, which narrows to single-byte
            // characters when its stdout is a pipe. Decode as UTF-8.
            StandardOutputEncoding = Encoding.UTF8
        };

        using var process = Process.Start(startInfo);
        if (process is null) return null;

        string output = await process.StandardOutput.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);

        var known = KnownPattern.Match(output);
        var mat = MatPattern.Match(output);
        var flowing = FlowingPattern.Match(output);
        var subtype = SubtypePattern.Match(output);
        var dropped = DroppedPattern.Match(output);
        var formatChanges = FormatChangesPattern.Match(output);
        if (!known.Success || !mat.Success || !flowing.Success) return null;

        return new MatStreamStatus(
            known.Groups[1].Value == "1",
            mat.Groups[1].Value == "1",
            subtype.Success ? subtype.Groups[1].Value.Trim() : string.Empty,
            long.Parse(flowing.Groups[1].Value, CultureInfo.InvariantCulture),
            dropped.Success ? long.Parse(dropped.Groups[1].Value, CultureInfo.InvariantCulture) : 0,
            formatChanges.Success ? long.Parse(formatChanges.Groups[1].Value, CultureInfo.InvariantCulture) : 0);
    }

    public bool Start(string cliPath, string outputPath)
    {
        if (IsRecording) return false;

        var startInfo = new ProcessStartInfo(cliPath, $"mat-recorder \"{outputPath}\" 2")
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            // ROTATE carries a file path, which may be non-ASCII. A BOM must not
            // be emitted: StreamWriter only writes the preamble on the first
            // write, so the first ROTATE would be prefixed with EF BB BF.
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

    /// <summary>
    /// Closes the current output file and continues the same capture stream into
    /// <paramref name="path"/>. The capture device is never reopened, so no
    /// carrier bytes are lost at the boundary.
    /// </summary>
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

        // The child treats any recognised command on stdin as the stop signal.
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

        var valid = ValidPattern.Match(text);
        var bytes = BytesPattern.Match(text);
        var carrier = CarrierPattern.Match(text);
        var nullB = NullPattern.Match(text);
        var other = OtherPattern.Match(text);
        var dropped = RecDroppedPattern.Match(text);
        var formatChanges = FormatChangesPattern.Match(text);
        if (!valid.Success || !bytes.Success || !carrier.Success) return;

        Progress?.Invoke(new MatRecordProgress(
            long.Parse(valid.Groups[1].Value, CultureInfo.InvariantCulture),
            long.Parse(bytes.Groups[1].Value, CultureInfo.InvariantCulture),
            long.Parse(carrier.Groups[1].Value, CultureInfo.InvariantCulture),
            nullB.Success ? long.Parse(nullB.Groups[1].Value, CultureInfo.InvariantCulture) : 0,
            other.Success ? long.Parse(other.Groups[1].Value, CultureInfo.InvariantCulture) : 0,
            dropped.Success ? long.Parse(dropped.Groups[1].Value, CultureInfo.InvariantCulture) : 0,
            formatChanges.Success ? long.Parse(formatChanges.Groups[1].Value, CultureInfo.InvariantCulture) : 0));
    }
}
