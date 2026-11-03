using System.Diagnostics;
using System.IO;
using System.Text;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

/// <summary>
/// Owns the NativeSpatialPipe capture process for NSP mode.
///
/// NSP is the producer: it attaches to audiodg and publishes 7.1.4 into the
/// shared-memory ring that SpatialAudioLab.CLI reads. So it has to be running
/// before the bridge matters, and it has to be killed when the bridge stops or
/// it keeps holding the ring.
///
/// Unlike the other modes no SysVAD endpoint is involved, and the capture side
/// needs elevation to attach to audiodg — which the studio manifest now provides.
/// </summary>
internal sealed class NspCaptureService
{
    static readonly TimeSpan StartupGrace = TimeSpan.FromSeconds(3);

    Process? capture;
    string? capturePidPath;
    string? stopFilePath;

    /// <summary>
    /// Where the NativeSpatialPipe checkout lives. The environment variable wins,
    /// then a file next to the application, then the usual checkout location.
    /// </summary>
    internal static string? ResolveNspRoot(RuntimePaths paths)
    {
        string? fromEnvironment = Environment.GetEnvironmentVariable("NSP_HOME");
        if (!string.IsNullOrWhiteSpace(fromEnvironment) &&
            LooksLikeNspCheckout(fromEnvironment))
        {
            return Path.GetFullPath(fromEnvironment);
        }

        string hintPath = Path.Combine(paths.ApplicationRoot, "nsp-home.txt");
        if (File.Exists(hintPath))
        {
            string hint = File.ReadAllText(hintPath).Trim();
            if (hint.Length > 0 && LooksLikeNspCheckout(hint))
            {
                return Path.GetFullPath(hint);
            }
        }

        string userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (userProfile.Length == 0)
        {
            return null;
        }

        string codeBuddy = Path.Combine(userProfile, "CodeBuddy");
        if (!Directory.Exists(codeBuddy))
        {
            return null;
        }

        return Directory.EnumerateDirectories(codeBuddy, "NativeSpatialPipe", SearchOption.AllDirectories)
            .FirstOrDefault(LooksLikeNspCheckout);
    }

    static bool LooksLikeNspCheckout(string path) =>
        File.Exists(Path.Combine(path, "tools", "endpoint-probe", "Run-Native.py"));

    internal bool IsRunning => capture is { HasExited: false };

    internal async Task StartAsync(RuntimePaths paths, string nspRoot)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (string.IsNullOrWhiteSpace(nspRoot))
        {
            throw new InvalidOperationException(
                "找不到 NativeSpatialPipe。请设置环境变量 NSP_HOME，或在应用程序目录下放一个 " +
                "nsp-home.txt 写入它的路径。");
        }

        if (IsRunning)
        {
            return;
        }

        string runner = Path.Combine(nspRoot, "tools", "endpoint-probe", "Run-Native.py");
        if (!File.Exists(runner))
        {
            throw new FileNotFoundException("找不到 NSP 采集脚本。", runner);
        }

        int audioProcessId = ResolveAudioProcessId();

        // NSP refuses to write into a directory that already exists.
        string captureRoot = Path.Combine(paths.LogsRoot, "nsp-captures");
        Directory.CreateDirectory(captureRoot);
        string outputDirectory = Path.Combine(
            captureRoot,
            DateTime.Now.ToString("yyyyMMdd-HHmmss"));

        string logPath = Path.Combine(paths.LogsRoot, "nsp-capture.log");
        string errorLogPath = Path.Combine(paths.LogsRoot, "nsp-capture.err.log");
        capturePidPath = Path.Combine(paths.LogsRoot, "nsp-capture.pid");
        stopFilePath = Path.Combine(paths.LogsRoot, "nsp-capture.stop");
        foreach (string stale in new[] { logPath, errorLogPath, capturePidPath, stopFilePath })
        {
            File.Delete(stale);
        }

        ProcessStartInfo start = new(FindPython())
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = nspRoot
        };
        start.ArgumentList.Add("-3");
        start.ArgumentList.Add(runner);
        start.ArgumentList.Add(outputDirectory);
        start.ArgumentList.Add(audioProcessId.ToString());
        // --duration 0 means "run until stopped": the native watchdog then waits
        // on the stop event only, and the studio kills the process on stop
        // anyway. Any nonzero value caps the capture (NSP allows at most 1200).
        start.ArgumentList.Add("--duration");
        start.ArgumentList.Add("0");
        start.ArgumentList.Add("--shm-ring");
        // A stop file makes NSP wind down through its own shutdown path, which
        // unloads the hook it injected into audiodg. Killing the process tree
        // instead leaves that hook resident, and the next start then fails once
        // with "already started" before its cleanup clears it — the restart
        // failure the studio used to show on every stop/start cycle.
        start.ArgumentList.Add("--stop-file");
        start.ArgumentList.Add(stopFilePath);

        Process process = Process.Start(start)
            ?? throw new InvalidOperationException("无法启动 NSP 采集进程。");
        capture = process;
        File.WriteAllText(capturePidPath, $"{process.Id}\n", Encoding.ASCII);
        _ = MonitorAsync(process, logPath, errorLogPath, capturePidPath);

        await Task.Delay(StartupGrace).ConfigureAwait(false);
        if (HasExited(process))
        {
            capture = null;
            string error = File.Exists(errorLogPath)
                ? await File.ReadAllTextAsync(errorLogPath).ConfigureAwait(false)
                : string.Empty;
            throw new InvalidOperationException(
                string.IsNullOrWhiteSpace(error)
                    ? "NSP 采集进程启动后立即退出。"
                    : $"NSP 采集进程启动后立即退出：\n\n{error.Trim()}");
        }
    }

    internal async Task StopAsync()
    {
        Process? process = capture;
        string? stopFile = stopFilePath;
        capture = null;
        stopFilePath = null;
        if (process is null)
        {
            return;
        }

        try
        {
            if (!HasExited(process))
            {
                // Ask NSP to finish first and give it time to walk its own
                // shutdown path. Only fall back to a hard kill when it does not
                // leave, so the injected hook stays unloaded between runs.
                if (stopFile is not null)
                {
                    try
                    {
                        File.WriteAllText(stopFile, string.Empty);
                    }
                    catch (IOException)
                    {
                    }

                    for (int waited = 0; waited < 40 && !HasExited(process); ++waited)
                    {
                        await Task.Delay(200).ConfigureAwait(false);
                    }
                }

                if (!HasExited(process))
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().ConfigureAwait(false);
                }
            }
        }
        catch (InvalidOperationException)
        {
        }
        finally
        {
            process.Dispose();
            if (capturePidPath is not null)
            {
                File.Delete(capturePidPath);
            }
        }
    }

    /// <summary>
    /// A process that exits quickly is disposed by the monitor on another thread,
    /// and touching it afterwards throws instead of reporting the exit. Treat any
    /// such failure as "gone", so callers can still read the error log and show
    /// what actually went wrong rather than an unrelated internal message.
    /// </summary>
    static bool HasExited(Process process)
    {
        try
        {
            return process.HasExited;
        }
        catch (InvalidOperationException)
        {
            return true;
        }
    }

    static async Task MonitorAsync(
        Process process,
        string logPath,
        string errorLogPath,
        string pidPath)
    {
        try
        {
            await using FileStream output = new(logPath, FileMode.Create, FileAccess.Write, FileShare.Read);
            await using FileStream error = new(errorLogPath, FileMode.Create, FileAccess.Write, FileShare.Read);
            Task outputTask = process.StandardOutput.BaseStream.CopyToAsync(output);
            Task errorTask = process.StandardError.BaseStream.CopyToAsync(error);
            await process.WaitForExitAsync().ConfigureAwait(false);
            await Task.WhenAll(outputTask, errorTask).ConfigureAwait(false);
        }
        catch (IOException)
        {
        }
        finally
        {
            try
            {
                File.Delete(pidPath);
            }
            catch (IOException)
            {
            }

            process.Dispose();
        }
    }

    /// <summary>
    /// NSP attaches to the Windows audio device graph process.
    /// </summary>
    static int ResolveAudioProcessId()
    {
        Process[] processes = Process.GetProcessesByName("audiodg");
        try
        {
            return processes.Length > 0
                ? processes[0].Id
                : throw new InvalidOperationException(
                    "找不到 audiodg 进程。请先播放一次音频让 Windows 启动音频设备图，再启动 NSP。");
        }
        finally
        {
            foreach (Process process in processes)
            {
                process.Dispose();
            }
        }
    }

    static string FindPython()
    {
        // The launcher is registered by every python.org install and is what the
        // NSP docs use; "python" is only on PATH for store installs.
        foreach (string candidate in new[] { "py", "python" })
        {
            if (IsOnPath(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException("找不到 Python（py 或 python）。NSP 采集需要 Python 3。");
    }

    static bool IsOnPath(string executable)
    {
        string? path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(path))
        {
            return false;
        }

        string[] extensions =
            Environment.GetEnvironmentVariable("PATHEXT")?.Split(';', StringSplitOptions.RemoveEmptyEntries)
            ?? [".EXE"];

        return path.Split(';', StringSplitOptions.RemoveEmptyEntries)
            .Select(directory => Path.Combine(directory.Trim(), executable))
            .Any(basePath =>
                File.Exists(basePath) ||
                extensions.Any(extension => File.Exists(basePath + extension)));
    }
}
