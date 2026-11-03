using System.Diagnostics;
using System.IO;
using System.Text;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

/// <summary>
/// 拥有“接收 E-AC-3”模式的生产者链路。
///
/// 两段进程：CLI 的 <c>ec3-pipe</c> 从驱动的 IEC 61937 载波里剥出裸 E-AC-3 写进命名管道，
/// <c>Eac3Pipe714</c> 读那条管道、用 Cavern 解 JOC 对象并渲染 7.1.4，顺手建出
/// <c>Local\SAL_EAC3_PCM714</c> 共享内存环。引擎（<c>live-eac3-layout</c>）只是这条环的
/// 读者，而 NspRingReader 只在构造时打开映射一次，所以两段都必须在桥启动之前就位。
/// </summary>
internal sealed class Eac3ProducerService : IDisposable
{
    /// <summary>
    /// 与 CLI <c>ec3-pipe</c> 的默认管道名保持一致。
    /// </summary>
    const string PipeName = "SpatialAudioLab.Eac3";

    Process? pipe;
    Process? decoder;
    string? pipePidPath;
    string? decoderPidPath;

    internal bool IsRunning => decoder is { HasExited: false };

    /// <summary>
    /// Eac3Pipe714 在发布包里位于 Engine\Eac3Bridge\，源码树里在它自己的 bin 下。
    /// </summary>
    internal static string? ResolveDecoderExecutable(RuntimePaths paths)
    {
        ArgumentNullException.ThrowIfNull(paths);
        string[] candidates =
        [
            Path.Combine(paths.ApplicationRoot, "Engine", "Eac3Bridge", "Eac3Pipe714.exe"),
            Path.Combine(paths.ApplicationRoot, "Engine", "Eac3Pipe714.exe"),
            Path.Combine(paths.ApplicationRoot, "Eac3Bridge", "Eac3Pipe714.exe")
        ];
        foreach (string candidate in candidates)
        {
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    internal async Task StartAsync(RuntimePaths paths)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (IsRunning) return;

        string decoderPath = ResolveDecoderExecutable(paths)
            ?? throw new InvalidOperationException(
                "找不到 Eac3Pipe714.exe。发布包里它应在 Engine\\Eac3Bridge\\ 下。");
        if (!File.Exists(paths.EngineExecutable))
        {
            throw new FileNotFoundException("未找到 SpatialAudioLab 引擎。", paths.EngineExecutable);
        }

        await StopAsync().ConfigureAwait(true);
        Directory.CreateDirectory(paths.LogsRoot);
        pipePidPath = Path.Combine(paths.LogsRoot, "eac3-pipe.pid");
        decoderPidPath = Path.Combine(paths.LogsRoot, "eac3-decoder.pid");

        // ec3-pipe 是管道的服务端，先起来客户端才有地方连。
        pipe = Launch(paths.EngineExecutable,
            ["ec3-pipe", PipeName], "eac3-pipe", paths, pipePidPath);
        await Task.Delay(400).ConfigureAwait(true);

        // 解码器连上管道之后才会建环，所以它必须活到桥启动。
        decoder = Launch(decoderPath,
            ["--input-pipe", PipeName, "--out-ring"], "eac3-decoder", paths, decoderPidPath);
        await Task.Delay(900).ConfigureAwait(true);

        if (pipe.HasExited)
        {
            throw new InvalidOperationException(
                "ec3-pipe 启动后立即退出。\n\n" + ReadTail(paths.LogsRoot, "eac3-pipe.err.log"));
        }
        if (decoder.HasExited)
        {
            throw new InvalidOperationException(
                "Eac3Pipe714 启动后立即退出。\n\n" + ReadTail(paths.LogsRoot, "eac3-decoder.err.log"));
        }
    }

    internal Task StopAsync()
    {
        // 先杀解码器：它是环的生产者，早一拍释放映射，引擎那边收尾时不会读到半帧。
        Kill(decoder, decoderPidPath);
        Kill(pipe, pipePidPath);
        decoder = null;
        pipe = null;
        decoderPidPath = null;
        pipePidPath = null;
        return Task.CompletedTask;
    }

    static Process Launch(
        string executable,
        IReadOnlyList<string> arguments,
        string stem,
        RuntimePaths paths,
        string pidPath)
    {
        string logPath = Path.Combine(paths.LogsRoot, stem + ".log");
        string errorLogPath = Path.Combine(paths.LogsRoot, stem + ".err.log");
        foreach (string stale in new[] { logPath, errorLogPath, pidPath })
        {
            File.Delete(stale);
        }

        ProcessStartInfo start = new(executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        foreach (string argument in arguments)
        {
            start.ArgumentList.Add(argument);
        }

        Process process = Process.Start(start)
            ?? throw new InvalidOperationException($"无法启动 {Path.GetFileName(executable)}。");
        File.WriteAllText(pidPath, $"{process.Id}\n", Encoding.ASCII);
        _ = PumpAsync(process, logPath, errorLogPath);
        return process;
    }

    /// <summary>
    /// 把子进程的输出落到文件。不读走的话，管道写满会把子进程堵死在输出上。
    /// </summary>
    static async Task PumpAsync(Process process, string logPath, string errorLogPath)
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
        catch (Exception error) when (error is IOException or InvalidOperationException or ObjectDisposedException)
        {
            // 停桥时进程被强杀，这里必然抛；不是错误。
        }
    }

    static void Kill(Process? process, string? pidPath)
    {
        try
        {
            if (process is not null && !process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(2000);
            }
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
        }
        finally
        {
            process?.Dispose();
            if (pidPath is not null)
            {
                try
                {
                    File.Delete(pidPath);
                }
                catch (IOException)
                {
                }
            }
        }
    }

    static string ReadTail(string logsRoot, string fileName)
    {
        string path = Path.Combine(logsRoot, fileName);
        try
        {
            return File.Exists(path) ? File.ReadAllText(path).Trim() : string.Empty;
        }
        catch (IOException)
        {
            return string.Empty;
        }
    }

    public void Dispose() => _ = StopAsync();
}
