using System.Globalization;

namespace SpatialAudioLab.Core.Runtime;

public enum BridgeMode
{
    Atmos,
    NativeMat,
    DtsX,
    Pcm,
    /// <summary>
    /// NativeSpatialPipe delivers 7.1.4 over a shared-memory ring, replacing the
    /// virtual sink driver. Requires the capture side to be running; unlike the
    /// other modes no SysVAD endpoint is involved.
    /// </summary>
    Nsp,

    /// <summary>
    /// E-AC-3 passthrough: the CLI's ec3-pipe strips the raw stream out of the
    /// driver's IEC 61937 carrier, Eac3Pipe714 decodes it with Cavern and renders
    /// 7.1.4 into the SAL_EAC3_PCM714 shared-memory ring, and the engine reads
    /// that ring. No SysVAD endpoint, no spatial provider and no virtual cable is
    /// involved; the producer side has to be running before the engine starts.
    /// </summary>
    Eac3
}

public enum BridgeLatencyMode
{
    Safe,
    Balanced,
    Low
}

/// <param name="ProfilePath">
/// Layout file the engine loads, carried separately from <paramref name="Arguments"/>.
/// Modes do not all place it at the same argument index — process loopback puts the
/// source PID between the duration and the profile — so callers that need to check
/// the file must ask for it by name rather than indexing the argument list.
/// </param>
public sealed record BridgeCommand(
    string Executable,
    IReadOnlyList<string> Arguments,
    string LogPath,
    string ErrorLogPath,
    string PidPath,
    string ProfilePath,
    /// <summary>
    /// Full path of the crossover (bass management) config to hand to the engine,
    /// or null to leave the engine bypassed.
    ///
    /// It travels as an environment variable rather than an argument: the engine's
    /// argument lists are position-encoded per mode, and adding a seventh
    /// positional argument would have to be repeated in every branch. The engine
    /// stays soft on this - a missing file or a channel count that does not match
    /// the layout makes it bypass with one log line, never break playback.
    /// </summary>
    string? CrossoverConfigPath = null);

public static class BridgeCommandFactory
{
    public static BridgeCommand Create(
        RuntimePaths paths,
        BridgeMode mode,
        string profilePath,
        double gain,
        int prebufferMilliseconds,
        BridgeLatencyMode latencyMode,
        string? crossoverConfigPath = null)
    {
        ArgumentNullException.ThrowIfNull(paths);
        ArgumentException.ThrowIfNullOrWhiteSpace(profilePath);
        if (!double.IsFinite(gain) || gain is < 0 or > 1)
        {
            throw new ArgumentOutOfRangeException(nameof(gain));
        }

        if (prebufferMilliseconds is < 20 or > 500)
        {
            throw new ArgumentOutOfRangeException(nameof(prebufferMilliseconds));
        }

        (string engineCommand, string fileStem) = mode switch
        {
            BridgeMode.Atmos => ("live-layout", "atmos"),
            BridgeMode.NativeMat => ("live-layout", "native-mat"),
            BridgeMode.DtsX => ("live-dtsx-layout", "dtsx"),
            BridgeMode.Pcm => ("live-pcm-layout", "pcm"),
            BridgeMode.Nsp => ("live-nsp-layout", "nsp"),
            BridgeMode.Eac3 => ("live-eac3-layout", "eac3"),
            _ => throw new ArgumentOutOfRangeException(nameof(mode))
        };

        string normalizedProfilePath = Path.GetFullPath(profilePath);
        string latency = latencyMode.ToString().ToLowerInvariant();
        string gainText = gain.ToString("0.###", CultureInfo.InvariantCulture);
        string prebufferText = prebufferMilliseconds.ToString(CultureInfo.InvariantCulture);
        // The ring name is left implicit: live-eac3-layout defaults to
        // Local\SAL_EAC3_PCM714, which is what Eac3Pipe714 creates.
        string[] arguments = [
            engineCommand,
            "0",
            normalizedProfilePath,
            gainText,
            prebufferText,
            latency
        ];

        string? normalizedCrossoverPath = string.IsNullOrWhiteSpace(crossoverConfigPath)
            ? null
            : Path.GetFullPath(crossoverConfigPath);

        return new BridgeCommand(
            paths.EngineExecutable,
            arguments,
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.log"),
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.err.log"),
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.pid"),
            normalizedProfilePath,
            normalizedCrossoverPath);
    }
}
