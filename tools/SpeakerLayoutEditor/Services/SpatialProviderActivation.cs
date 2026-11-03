using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Win32;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

/// <summary>
/// 起桥之前把 Windows 的“空间音效提供程序”调成与 <see cref="BridgeMode"/> 匹配的状态。
///
/// 背景（作者文档 TECHNICAL_NOTES 100-117 行）：在这个 SysVAD 端点上，WinRT 的
/// SpatialAudioDeviceConfiguration 会返回 AccessDenied / NotSupportedOnAudioEndpoint，
/// 所以只能直接写 MMDevices 的私有属性；而现代面板“显示 Atmos”并不代表提供程序已激活。
/// 状态一旦被别的模式（Pcm / NativeMat）中和，端点报告的 Maximum dynamic objects 就变成 0，
/// 空间音频流会建不起来（SPTLAUDCLNT_E_INTERNAL），桥虽然“已启动”却收不到任何 MAT
/// —— 用户看到的就是没声音、没电平。
///
/// 流程与 tools\Set-SpatialProvider.ps1 一致：
///   1) 解析默认渲染端点，探测；已就绪立刻返回（不重启音频服务，避免无谓断音）
///   2) 未就绪：中和 → 重建音频服务 → 按模式写状态 / 设设备格式 → 再重建 → 探测确认
///   3) 仍不通过：pnputil 重新枚举该设备 → 再探测
///   4) 任何失败都回滚到进入前的 5 个值，并抛出可直接展示给用户的错误
///
/// 注意：SpatialAudioLab.CLI 的这些子命令**不接受空过滤器**（帮助里写“空过滤器=默认端点”，
/// 实测会报 "No endpoint matched the requested filter"），所以要显式传端点的裸 GUID。
/// </summary>
[SupportedOSPlatform("windows")]
internal static class SpatialProviderActivation
{
    const string MmDevicesPrefix =
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\";

    // 端点 Properties 下的 5 个私有值（与 Set-SpatialProvider.ps1:327-333 一致）
    const string EnabledValue = "{6737016f-5360-48ee-af05-e616c8ff27a7},2";
    const string ActiveValue = "{fd8a7b27-0b18-4025-ab1c-bdd6b32e1604},2";
    const string ProviderValue = "{908dba32-edff-4c28-8e45-c918561f6748},2";
    const string SelectionValue = "{8a845654-d6c3-4cd7-b4eb-243d4bd99032},2";
    const string CarrierValue = "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0";

    // 设备实例（用于 pnputil /restart-device 兜底）
    const string DeviceInstanceValue = "{b3f8fa53-0004-438e-9003-51a46e139bfc},2";

    static readonly string[] ValueNames =
        [EnabledValue, ActiveValue, ProviderValue, SelectionValue, CarrierValue];

    // 空间格式 GUID（Set-SpatialProvider.ps1:181-185）
    const string AtmosGuid = "5D7389A23EFA354E9D7DB6F896ACB2E7";
    const string DtsXGuid = "4A1B201022336749BF402CAA9BAFCA44";

    // 作者文档给出的验收判据
    const uint AtmosMask = 0xC1FFE;
    const uint DtsXMask = 0xFFFFE;
    const int AtmosObjects = 20;
    const int DtsXObjects = 32;

    static readonly TimeSpan EndpointTimeout = TimeSpan.FromSeconds(20);
    static readonly TimeSpan ServiceSettleDelay = TimeSpan.FromMilliseconds(1500);

    internal static async Task EnsureAsync(
        BridgeMode mode,
        RuntimePaths paths,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(paths);
        string engine = paths.EngineExecutable;
        if (string.IsNullOrWhiteSpace(engine) || !File.Exists(engine))
        {
            throw new FileNotFoundException("找不到 SpatialAudioLab Engine。", engine);
        }

        // NSP does not use the virtual sink at all: it captures from audiodg on
        // whichever endpoint is default, and any spatial format Windows builds
        // there can be tried. The registry state machine below is written for the
        // SysVAD endpoint and would clobber what Windows configured on a physical
        // one, so NSP is never reconfigured here — the caller decides whether to
        // warn, via DescribeNspSpatialFormatAsync.
        // Neither ring-fed mode uses the virtual sink: NSP captures from audiodg,
        // and E-AC-3 passthrough decodes the driver's bitstream carrier. The
        // registry state machine below is written for the SysVAD endpoint and
        // would clobber whatever Windows configured on a physical one.
        if (mode == BridgeMode.Nsp || mode == BridgeMode.Eac3)
        {
            return;
        }

        string? endpointGuid = await ResolveDefaultEndpointGuidAsync(engine, cancellationToken)
            .ConfigureAwait(false);
        if (endpointGuid is null)
        {
            throw new InvalidOperationException(
                "无法确定默认渲染端点（list-endpoints-json 没有给出 isDefault 的端点）。" +
                "请先在“声音”设置里把虚拟声卡设为默认播放设备。");
        }

        string filter = "{" + endpointGuid + "}";
        SpatialProbe probe = await ProbeAsync(engine, filter, cancellationToken).ConfigureAwait(false);
        if (IsSatisfied(mode, probe))
        {
            return;
        }

        if (!IsElevated())
        {
            throw new InvalidOperationException(
                $"{ModeName(mode)} 需要重新配置空间音效提供程序（当前：{Describe(probe)}）。" +
                "请以管理员身份运行 Studio，或先手动执行：" +
                $"\n\ntools\\Set-SpatialProvider.ps1 -Mode {mode}");
        }

        string subkey = MmDevicesPrefix + "{" + endpointGuid + "}\\Properties";
        Dictionary<string, byte[]?> original = ReadValues(subkey);
        string? instanceId = ReadDeviceInstanceId(subkey);

        try
        {
            // 第一阶段：先中和，再按模式武装（避免状态机停在中间态）
            WriteNeutralized(subkey, original);
            await RestartAudioServicesAsync(engine, filter, cancellationToken).ConfigureAwait(false);

            if (mode is BridgeMode.Atmos or BridgeMode.DtsX)
            {
                string codec = mode == BridgeMode.Atmos ? "atmos" : "dtsx";
                EngineResult codecResult = await RunEngineAsync(
                    engine,
                    ["set-codec-format", codec, filter],
                    cancellationToken).ConfigureAwait(false);
                if (codecResult.ExitCode != 0)
                {
                    throw new InvalidOperationException(
                        $"set-codec-format {codec} 失败（退出码 {codecResult.ExitCode}）：\n{codecResult.Output}");
                }

                WriteArmed(subkey, mode);
            }
            else if (mode == BridgeMode.NativeMat)
            {
                await RunEngineAsync(engine, ["set-codec-format", "mat10", filter], cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                await RunEngineAsync(engine, ["set-pcm714-format", filter], cancellationToken)
                    .ConfigureAwait(false);
            }

            await RestartAudioServicesAsync(engine, filter, cancellationToken).ConfigureAwait(false);

            probe = await WaitForProbeAsync(engine, mode, filter, cancellationToken)
                .ConfigureAwait(false);
            if (!IsSatisfied(mode, probe) && instanceId is not null)
            {
                // 第二阶段：服务重建不够，重新枚举设备（对应作者的 Restart-SpatialDevice）
                await RestartDeviceAsync(instanceId, engine, filter, cancellationToken)
                    .ConfigureAwait(false);
                probe = await WaitForProbeAsync(engine, mode, filter, cancellationToken)
                    .ConfigureAwait(false);
            }

            if (!IsSatisfied(mode, probe))
            {
                throw new InvalidOperationException(
                    $"{ModeName(mode)} 的空间音效提供程序没有就绪。\n" +
                    $"当前端点状态：{Describe(probe)}\n" +
                    $"期望：{Expected(mode)}\n\n" +
                    "手动排查：\n" +
                    $"  1) SpatialAudioLab.CLI probe-spatial-metadata {filter}\n" +
                    $"  2) tools\\Set-SpatialProvider.ps1 -Mode {mode} -EndpointFilter '<虚拟声卡端点>' -DeviceInstanceId 'ROOT\\MEDIA\\0000'");
            }
        }
        catch
        {
            // 回滚到进入前的状态，避免把端点留在半成品态
            WriteValues(subkey, original);
            try
            {
                await RestartAudioServicesAsync(engine, filter, CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch
            {
                // 回滚尽力而为：原始异常更重要
            }

            throw;
        }
    }

    /// <summary>
    /// Reports the spatial format of the default endpoint as NSP will see it, or
    /// null when it is one NSP is known to handle. Never blocks: the format is
    /// chosen in Windows, and trying an untested one is the only way to find out
    /// how it behaves, so the caller warns and lets the operator decide.
    /// </summary>
    internal static async Task<string?> DescribeNspSpatialFormatAsync(
        RuntimePaths paths,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(paths);
        string engine = paths.EngineExecutable;
        if (string.IsNullOrWhiteSpace(engine) || !File.Exists(engine))
        {
            return null;
        }

        string? endpointGuid =
            await ResolveDefaultEndpointGuidAsync(engine, cancellationToken).ConfigureAwait(false);
        if (endpointGuid is null)
        {
            return "无法确定默认渲染端点。";
        }

        string filter = "{" + endpointGuid + "}";
        SpatialProbe probe = await ProbeAsync(engine, filter, cancellationToken).ConfigureAwait(false);
        if (!probe.CommandSucceeded)
        {
            return $"未能探测到默认端点的空间音效格式。\n{probe.Raw}";
        }

        if (probe.Mask == AtmosMask && probe.Objects == AtmosObjects)
        {
            return null;
        }

        return $"当前：{Describe(probe)}\n" +
               $"Dolby Atmos for home theater：mask=0x{AtmosMask:X}, dynamic objects={AtmosObjects}";
    }

    // ---------------------------------------------------------------- 探测

    sealed record SpatialProbe(bool CommandSucceeded, uint Mask, int Objects, string Raw);

    static bool IsSatisfied(BridgeMode mode, SpatialProbe probe)
    {
        if (!probe.CommandSucceeded)
        {
            return false;
        }

        return mode switch
        {
            BridgeMode.Atmos => probe.Mask == AtmosMask && probe.Objects == AtmosObjects,
            BridgeMode.DtsX => probe.Mask == DtsXMask && probe.Objects == DtsXObjects,
            // NSP 读的是 Atmos 家院版生成的床，所以判据与 Atmos 相同
            BridgeMode.Nsp => probe.Mask == AtmosMask && probe.Objects == AtmosObjects,
            // Pcm / NativeMat 需要的是“提供程序关闭”
            _ => probe.Objects == 0
        };
    }

    static string Expected(BridgeMode mode) => mode switch
    {
        BridgeMode.Atmos => $"mask=0x{AtmosMask:X}, dynamic objects={AtmosObjects}",
        BridgeMode.DtsX => $"mask=0x{DtsXMask:X}, dynamic objects={DtsXObjects}",
        _ => "dynamic objects=0（提供程序关闭）"
    };

    static string Describe(SpatialProbe probe) => probe.CommandSucceeded
        ? $"mask=0x{probe.Mask:X}, dynamic objects={probe.Objects}"
        : $"探测失败：{FirstLine(probe.Raw)}";

    static string ModeName(BridgeMode mode) => mode switch
    {
        BridgeMode.Atmos => "Dolby Atmos（Windows）",
        BridgeMode.NativeMat => "Dolby MAT 原生 (BF1)",
        BridgeMode.DtsX => "DTS:X",
        BridgeMode.Nsp => "NSP 7.1.4（共享内存）",
        _ => "PCM 7.1.4"
    };

    static string FirstLine(string text)
    {
        int index = text.IndexOfAny(['\r', '\n']);
        string line = index < 0 ? text : text[..index];
        return line.Trim();
    }

    static async Task<SpatialProbe> ProbeAsync(
        string engine,
        string filter,
        CancellationToken cancellationToken)
    {
        EngineResult result = await RunEngineAsync(
            engine,
            ["probe-spatial-metadata", filter],
            cancellationToken).ConfigureAwait(false);

        Match mask = Regex.Match(result.Output, @"Native static object mask:\s*0[xX]([0-9a-fA-F]+)");
        Match objects = Regex.Match(result.Output, @"Maximum dynamic objects:\s*(\d+)");
        if (!mask.Success || !objects.Success)
        {
            return new SpatialProbe(false, 0, 0, result.Output);
        }

        return new SpatialProbe(
            true,
            uint.Parse(mask.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture),
            int.Parse(objects.Groups[1].Value, CultureInfo.InvariantCulture),
            result.Output);
    }

    static async Task<SpatialProbe> WaitForProbeAsync(
        string engine,
        BridgeMode mode,
        string filter,
        CancellationToken cancellationToken)
    {
        DateTimeOffset deadline = DateTimeOffset.UtcNow + EndpointTimeout;
        SpatialProbe probe = await ProbeAsync(engine, filter, cancellationToken).ConfigureAwait(false);
        while (!IsSatisfied(mode, probe) && DateTimeOffset.UtcNow < deadline)
        {
            await Task.Delay(500, cancellationToken).ConfigureAwait(false);
            probe = await ProbeAsync(engine, filter, cancellationToken).ConfigureAwait(false);
        }

        return probe;
    }

    static async Task<string?> ResolveDefaultEndpointGuidAsync(
        string engine,
        CancellationToken cancellationToken)
    {
        EngineResult result = await RunEngineAsync(
            engine,
            ["list-endpoints-json"],
            cancellationToken).ConfigureAwait(false);

        // 输出是 NDJSON：一行一个对象
        foreach (string line in result.Output.Split('\n'))
        {
            string trimmed = line.Trim();
            if (!trimmed.StartsWith('{'))
            {
                continue;
            }

            try
            {
                using JsonDocument document = JsonDocument.Parse(trimmed);
                JsonElement root = document.RootElement;
                if (!root.TryGetProperty("isDefault", out JsonElement isDefault) ||
                    isDefault.ValueKind != JsonValueKind.True)
                {
                    continue;
                }

                if (!root.TryGetProperty("id", out JsonElement id) ||
                    id.ValueKind != JsonValueKind.String)
                {
                    continue;
                }

                Match match = Regex.Match(
                    id.GetString() ?? string.Empty,
                    @"\{[0-9a-fA-F-]{36}\}\s*$");
                if (match.Success)
                {
                    return match.Value.Trim('{', '}');
                }
            }
            catch (JsonException)
            {
                // 忽略解析不了的行
            }
        }

        return null;
    }

    // ---------------------------------------------------------------- 注册表

    static bool IsElevated()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity)
            .IsInRole(WindowsBuiltInRole.Administrator);
    }

    static Dictionary<string, byte[]?> ReadValues(string subkey)
    {
        Dictionary<string, byte[]?> values = new(StringComparer.OrdinalIgnoreCase);
        using RegistryKey? key = Registry.LocalMachine.OpenSubKey(subkey, writable: false);
        foreach (string name in ValueNames)
        {
            values[name] = key?.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames)
                as byte[];
        }

        return values;
    }

    static string? ReadDeviceInstanceId(string subkey)
    {
        using RegistryKey? key = Registry.LocalMachine.OpenSubKey(subkey, writable: false);
        if (key?.GetValue(DeviceInstanceValue) is not string raw)
        {
            return null;
        }

        // 形如 "{1}.ROOT\MEDIA\0000"
        Match match = Regex.Match(raw, @"^\{[^}]*\}\.(?<instance>.+)$");
        return match.Success ? match.Groups["instance"].Value.Trim() : raw.Trim();
    }

    static void WriteValues(string subkey, Dictionary<string, byte[]?> values)
    {
        foreach ((string name, byte[]? value) in values)
        {
            if (value is null)
            {
                DeleteRegistryValue(subkey, name);
            }
            else
            {
                SetRegistryBinary(subkey, name, value);
            }
        }
    }

    /// <summary>“提供程序关闭”的中性状态（对应 Set-SpatialProvider.ps1:214-233）。</summary>
    static void WriteNeutralized(string subkey, Dictionary<string, byte[]?> original)
    {
        int providerLength = Math.Max(88, original[ProviderValue]?.Length ?? 0);
        byte[] provider = new byte[providerLength];
        FromHex("41000000010000000200005A").CopyTo(provider, 0);

        byte[] selection = new byte[32];
        FromHex("41000000010000000000000000000000").CopyTo(selection, 0);

        WriteValues(subkey, new Dictionary<string, byte[]?>(StringComparer.OrdinalIgnoreCase)
        {
            [EnabledValue] = FromHex("02000000010000000000"),
            [ActiveValue] = FromHex("410000000100000000"),
            [ProviderValue] = provider,
            [SelectionValue] = selection
        });
    }

    /// <summary>Atmos / DTS:X 的武装状态（对应 Set-SpatialProvider.ps1:180-212）。</summary>
    static void WriteArmed(string subkey, BridgeMode mode)
    {
        bool atmos = mode == BridgeMode.Atmos;
        string guid = atmos ? AtmosGuid : DtsXGuid;
        string objectState = atmos
            ? "FE1F0C000100000014000000"
            : "FEFF0F000100000020000000";
        string carrierSubtype = atmos ? "0C030000" : "0B010000";

        WriteValues(subkey, new Dictionary<string, byte[]?>(StringComparer.OrdinalIgnoreCase)
        {
            [EnabledValue] = FromHex("02000000010000000100"),
            [ActiveValue] = FromHex(
                "41000000010000000200005A" + guid + objectState +
                "A0400000A0400000A0400000000000000000000000000000F0410000" +
                "B442000007430000344200003442000007430000803F0000803F0000" +
                "803F0000803F0000803F0000803F0000803F0000803F96433B3FBF" +
                "0E1C3F0000803FBF0E1C3FD39FFD3ED39FFD3E0000803F00000000" +
                "00000000"),
            [ProviderValue] = FromHex(
                "41000000010000000200005A010000000000000001000000" +
                guid + guid +
                "00000000000000000000000000000000010000000000000001000000"),
            [SelectionValue] = FromHex(
                "41000000010000000000000000000000" + guid),
            [CarrierValue] = FromHex(
                "4100000001000000FEFF080000EE020000E02E001000100016001000" +
                "3F060000" + carrierSubtype + "EA0C1000800000AA00389B71")
        });
    }

    static byte[] FromHex(string hex)
    {
        if ((hex.Length & 1) != 0)
        {
            throw new ArgumentException("Invalid hex string.", nameof(hex));
        }

        byte[] result = new byte[hex.Length / 2];
        for (int index = 0; index < result.Length; ++index)
        {
            result[index] = byte.Parse(
                hex.AsSpan(index * 2, 2),
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture);
        }

        return result;
    }

    // ---------------------------------------------------------------- 注册表写入（原生）
    //
    // 为什么不用 Registry.CreateSubKey / OpenSubKey(writable: true)：
    // 该键的 ACL 是 BUILTIN\Administrators = SetValue + ReadKey（没有 CreateSubKey），
    // OpenSubKey(writable:true) 会要 KEY_WRITE 全量权限（含 CreateSubKey/CreateLink），
    // 于是即使以管理员运行也会得到 “Access to the registry key ... is denied”。
    // 作者脚本用 RegOpenKeyEx(KEY_SET_VALUE) + RegSetValueEx 正是为了只申请需要的那点权限。

    static readonly IntPtr HkeyLocalMachine = new(unchecked((int)0x80000002));

    const int KeyQueryValue = 0x0001;
    const int KeySetValue = 0x0002;
    const int RegBinary = 3;
    const int ErrorSuccess = 0;
    const int ErrorAccessDenied = 5;

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegOpenKeyExW(
        IntPtr hKey,
        string subKey,
        int options,
        int samDesired,
        out IntPtr result);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegSetValueExW(
        IntPtr hKey,
        string valueName,
        int reserved,
        int type,
        byte[] data,
        int dataSize);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegDeleteValueW(IntPtr hKey, string valueName);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern int RegCloseKey(IntPtr hKey);

    static void SetRegistryBinary(string subkey, string name, byte[] value)
    {
        IntPtr key = OpenRegistryKey(subkey);
        try
        {
            int status = RegSetValueExW(key, name, 0, RegBinary, value, value.Length);
            if (status != ErrorSuccess)
            {
                throw CreateRegistryError(status, subkey, name);
            }
        }
        finally
        {
            RegCloseKey(key);
        }
    }

    static void DeleteRegistryValue(string subkey, string name)
    {
        IntPtr key = OpenRegistryKey(subkey);
        try
        {
            int status = RegDeleteValueW(key, name);
            if (status != ErrorSuccess && status != 2 /* ERROR_FILE_NOT_FOUND */)
            {
                throw CreateRegistryError(status, subkey, name);
            }
        }
        finally
        {
            RegCloseKey(key);
        }
    }

    static IntPtr OpenRegistryKey(string subkey)
    {
        int status = RegOpenKeyExW(
            HkeyLocalMachine,
            subkey,
            0,
            KeyQueryValue | KeySetValue,
            out IntPtr key);
        if (status != ErrorSuccess)
        {
            throw CreateRegistryError(status, subkey, null);
        }

        return key;
    }

    static Exception CreateRegistryError(int status, string subkey, string? name)
    {
        string target = name is null ? subkey : $"{subkey} :: {name}";
        if (status == ErrorAccessDenied)
        {
            return new InvalidOperationException(
                "写入空间音效状态被拒绝（拒绝访问）。\n" +
                $"注册表位置：HKLM\\{target}\n\n" +
                "请以管理员身份运行 Studio（右键 → 以管理员身份运行），然后重试。");
        }

        return new InvalidOperationException(
            $"写入注册表失败（Win32 {status}：{new Win32Exception(status).Message}）。\n" +
            $"注册表位置：HKLM\\{target}");
    }

    // ---------------------------------------------------------------- 服务与设备

    static async Task RestartAudioServicesAsync(
        string engine,
        string filter,
        CancellationToken cancellationToken)
    {
        // 顺序与作者脚本一致：先停 Audiosrv，再停 AudioEndpointBuilder，反序启动
        await RunToolAsync("sc.exe", ["stop", "Audiosrv"], cancellationToken).ConfigureAwait(false);
        await RunToolAsync("sc.exe", ["stop", "AudioEndpointBuilder"], cancellationToken)
            .ConfigureAwait(false);
        await Task.Delay(ServiceSettleDelay, cancellationToken).ConfigureAwait(false);
        await RunToolAsync("sc.exe", ["start", "AudioEndpointBuilder"], cancellationToken)
            .ConfigureAwait(false);
        await RunToolAsync("sc.exe", ["start", "Audiosrv"], cancellationToken).ConfigureAwait(false);
        await Task.Delay(ServiceSettleDelay, cancellationToken).ConfigureAwait(false);

        await WaitForEndpointAsync(engine, filter, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// 轮询直到端点回来（对应作者的 Wait-SpatialEndpoint）。
    /// 只看退出码，不解析 sc.exe 的本地化文本。
    /// </summary>
    static async Task WaitForEndpointAsync(
        string engine,
        string filter,
        CancellationToken cancellationToken)
    {
        DateTimeOffset deadline = DateTimeOffset.UtcNow + EndpointTimeout;
        while (true)
        {
            EngineResult result = await RunEngineAsync(engine, ["set-default", filter], cancellationToken)
                .ConfigureAwait(false);
            if (result.ExitCode == 0)
            {
                return;
            }

            if (DateTimeOffset.UtcNow >= deadline)
            {
                throw new InvalidOperationException(
                    "音频服务重启后默认端点没有回来。\n" + result.Output);
            }

            await Task.Delay(500, cancellationToken).ConfigureAwait(false);
        }
    }

    static async Task RestartDeviceAsync(
        string instanceId,
        string engine,
        string filter,
        CancellationToken cancellationToken)
    {
        EngineResult result = await RunToolAsync(
            "pnputil.exe",
            ["/restart-device", instanceId],
            cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"pnputil /restart-device {instanceId} 失败（退出码 {result.ExitCode}）：\n{result.Output}");
        }

        await WaitForEndpointAsync(engine, filter, cancellationToken).ConfigureAwait(false);
    }

    // ---------------------------------------------------------------- 进程

    sealed record EngineResult(int ExitCode, string Output);

    static Task<EngineResult> RunEngineAsync(
        string engine,
        IReadOnlyList<string> arguments,
        CancellationToken cancellationToken) =>
        RunToolAsync(engine, arguments, cancellationToken);

    static async Task<EngineResult> RunToolAsync(
        string fileName,
        IReadOnlyList<string> arguments,
        CancellationToken cancellationToken)
    {
        ProcessStartInfo start = new(fileName)
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

        using Process process = Process.Start(start)
            ?? throw new InvalidOperationException($"无法启动 {fileName}。");

        Task<string> outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        Task<string> errorTask = process.StandardError.ReadToEndAsync(cancellationToken);

        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(60));
        try
        {
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
            }

            throw new InvalidOperationException($"{Path.GetFileName(fileName)} 超时未返回。");
        }

        string output = await outputTask.ConfigureAwait(false);
        string error = await errorTask.ConfigureAwait(false);
        string combined = string.IsNullOrWhiteSpace(error) ? output : output + "\n" + error;
        return new EngineResult(process.ExitCode, combined.Trim());
    }
}
