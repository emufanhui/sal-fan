using System.Diagnostics;
using System.IO;
using System.Text;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

internal interface ISpatialProviderService
{
    Task ActivateAsync(BridgeMode mode, RuntimePaths paths);
}

/// <summary>
/// 起桥前把 Windows 空间音效提供程序武装到与模式匹配的状态。
/// 原来是空实现（Task.CompletedTask），于是“提供程序被中和”时桥会静默启动却收不到 MAT。
/// 具体逻辑见 SpatialProviderActivation.cs。
/// </summary>
internal sealed class DeferredSpatialProviderService : ISpatialProviderService
{
    public Task ActivateAsync(BridgeMode mode, RuntimePaths paths) =>
        SpatialProviderActivation.EnsureAsync(mode, paths);
}

internal sealed class BridgeProcessService
{
    public async Task StartAsync(BridgeCommand command)
    {
        if (!File.Exists(command.Executable))
        {
            throw new FileNotFoundException(
                "未找到 SpatialAudioLab 引擎。",
                command.Executable);
        }

        // Ask for the profile by name: process loopback puts the source PID at
        // index 2, so indexing the argument list here would check the PID.
        if (!File.Exists(command.ProfilePath))
        {
            throw new FileNotFoundException(
                "未找到活动配置文件。",
                command.ProfilePath);
        }

        Directory.CreateDirectory(Path.GetDirectoryName(command.LogPath)!);
        foreach (string path in new[] { command.LogPath, command.ErrorLogPath, command.PidPath })
        {
            File.Delete(path);
        }

        ProcessStartInfo start = new(command.Executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        foreach (string argument in command.Arguments)
        {
            start.ArgumentList.Add(argument);
        }

        // The crossover config rides along as an environment variable, which is
        // how the engine is told to load it. Leaving it unset is the bypass case.
        if (!string.IsNullOrWhiteSpace(command.CrossoverConfigPath) &&
            File.Exists(command.CrossoverConfigPath))
        {
            start.Environment["SAL_CDSP_CONFIG"] = command.CrossoverConfigPath;
        }

        Process process = Process.Start(start) ??
            throw new InvalidOperationException("无法启动 SpatialAudioLab 引擎。");
        File.WriteAllText(command.PidPath, $"{process.Id}\n", Encoding.ASCII);
        Task monitorTask = MonitorAsync(process, command);

        await Task.Delay(750);
        bool hasExited;
        try
        {
            process.Refresh();
            hasExited = process.HasExited;
        }
        catch (InvalidOperationException)
        {
            hasExited = true;
        }

        if (hasExited)
        {
            await monitorTask;
            string error = File.Exists(command.ErrorLogPath)
                ? await File.ReadAllTextAsync(command.ErrorLogPath)
                : string.Empty;
            throw new InvalidOperationException(
                string.IsNullOrWhiteSpace(error)
                    ? "Engine termino durante el inicio."
                    : error.Trim());
        }
    }

    public async Task StopAsync(string pidPath)
    {
        if (!File.Exists(pidPath))
        {
            return;
        }

        string value = await File.ReadAllTextAsync(pidPath);
        if (!int.TryParse(value.Trim(), out int processId))
        {
            File.Delete(pidPath);
            return;
        }

        try
        {
            using Process process = Process.GetProcessById(processId);
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
            }
        }
        catch (ArgumentException)
        {
        }
        finally
        {
            File.Delete(pidPath);
        }
    }

    private static async Task MonitorAsync(Process process, BridgeCommand command)
    {
        int processId = process.Id;
        try
        {
            await using FileStream output = new(
                command.LogPath,
                FileMode.Create,
                FileAccess.Write,
                FileShare.Read);
            await using FileStream error = new(
                command.ErrorLogPath,
                FileMode.Create,
                FileAccess.Write,
                FileShare.Read);
            Task outputTask = process.StandardOutput.BaseStream.CopyToAsync(output);
            Task errorTask = process.StandardError.BaseStream.CopyToAsync(error);
            await process.WaitForExitAsync();
            await Task.WhenAll(outputTask, errorTask);
        }
        finally
        {
            try
            {
                if (File.Exists(command.PidPath) &&
                    int.TryParse(
                        (await File.ReadAllTextAsync(command.PidPath)).Trim(),
                        out int pid) &&
                    pid == processId)
                {
                    File.Delete(command.PidPath);
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
            process.Dispose();
        }
    }
}
