<#
.SYNOPSIS
    NSP -> SpatialAudioLab 7.1.4 共享内存环的一键端到端联调验证。

.DESCRIPTION
    生产端：调用 NSP 的 nsp_shm_sink.dll（nsp_shm_create / nsp_shm_push），
            按 ring 契约创建 "Local\NSP_PCM714"：
              magic "NSP7" / version 1 / channels 12 / 48000 Hz / format 1 (float32)
              capacity 4096 frames，layout = SAL(1)，即发布前把 ROAR 顺序
              置换为 SpatialAudioLab canonical 顺序（FL,FR,FC,LFE,BL,BR,SL,SR,TFL,TFR,TBL,TBR）。
            推送的信号：每 0.5 s 只点亮一个声道，按 ROAR 顺序轮转，
            每个声道一个独立频率，便于用耳朵定位声道映射。
    消费端：SpatialAudioLab.CLI.exe live-nsp-layout <seconds> <layout.ini> ...
            读取同一个环，经 MultiEndpointRenderer 送三张声卡。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\Test-NspBridge.ps1

.EXAMPLE
    .\tools\Test-NspBridge.ps1 -Seconds 24 -Gain 0.2
#>
[CmdletBinding()]
param(
    # 生产端推送时长（秒）。12 秒 = 两个完整的 12 声道轮转。
    [int]$Seconds = 12,
    # CLI 路径，默认自动挑选仓库里最新的 SpatialAudioLab.CLI.exe。
    [string]$Cli,
    # 声道布局，默认 configs\realtek-c1u-714.ini（恰好 12 声道，不做 downmix）。
    [string]$Layout,
    # NSP 共享内存 sink DLL。
    [string]$SinkDll,
    [ValidateRange(0.0, 1.0)][double]$Gain = 0.25,
    # renderer 预缓冲，单位 ms，CLI 只接受 20..500。
    [ValidateRange(20, 500)][int]$PrebufferMs = 40,
    [ValidateSet('safe', 'balanced', 'low')][string]$Latency = 'balanced',
    [string]$Ring = 'Local\NSP_PCM714',
    # 测试信号峰值振幅（再乘以 -Gain）。
    [ValidateRange(0.01, 0.9)][double]$Amplitude = 0.15,
    # 只跑生产端：建环、推数据、读计数，不起消费端。用来把
    # “NSP 侧正常” 和 “SAL 端点配错” 两类故障分开。
    [switch]$ProducerOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Resolve-FirstExisting {
    param([string[]]$Candidates, [string]$What)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "找不到$What，已尝试：`n  $($Candidates -join "`n  ")"
}

if (-not $PSBoundParameters.ContainsKey('Cli') -or -not $Cli) {
    $Cli = Resolve-FirstExisting -What ' SpatialAudioLab.CLI.exe' -Candidates @(
        (Join-Path $repo 'build-native\SpatialAudioLab.CLI.exe'),
        (Join-Path $repo 'build\SpatialAudioLab.CLI.exe'),
        (Join-Path $repo 'build-cli\Release\SpatialAudioLab.CLI.exe')
    )
}
if (-not $PSBoundParameters.ContainsKey('Layout') -or -not $Layout) {
    # 优先用 Studio 当前保存的版本 2 布局（Data\Profiles 里最新的一份），
    # 它带 endpoint_id / expected_channels，设备名变了也还能对上；
    # 没有就退回仓库自带的 configs 布局。
    $profiles = @()
    $profiles += Get-ChildItem -Path (Join-Path $repo 'Data\Profiles\*.ini') -ErrorAction SilentlyContinue
    $profiles += Get-ChildItem -Path (Join-Path (Split-Path -Parent $repo) '*\Data\Profiles\*.ini') -ErrorAction SilentlyContinue
    if (-not $profiles) {
        $profiles = Get-ChildItem -Path (Join-Path $repo 'configs\*.ini') -ErrorAction SilentlyContinue
    }
    $newest = $profiles | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $Layout = if ($newest) { $newest.FullName } else { Join-Path $repo 'configs\realtek-c1u-714.ini' }
}
if (-not $PSBoundParameters.ContainsKey('SinkDll') -or -not $SinkDll) {
    $SinkDll = Resolve-FirstExisting -What ' nsp_shm_sink.dll（NSP 共享内存 sink）' -Candidates @(
        $env:NSP_SHIM_DLL,
        'C:\Users\fanhu\CodeBuddy\20261017091833\NativeSpatialPipe\build\shm-sink\release\nsp_shm_sink.dll',
        (Join-Path $repo 'third_party\NativeSpatialPipe\build\shm-sink\release\nsp_shm_sink.dll')
    )
}
$Layout = (Resolve-Path -LiteralPath $Layout).Path
$SinkDll = (Resolve-Path -LiteralPath $SinkDll).Path
$sinkDir = Split-Path -Parent $SinkDll
# Rust 产物依赖 libunwind.dll，和 DLL 同目录；一并放进搜索路径。
$env:PATH = "$sinkDir;$env:PATH"

Write-Host ''
Write-Host '=== NSP 7.1.4 共享内存环 端到端检查 ===' -ForegroundColor Cyan
Write-Host ("  CLI      : {0}" -f $Cli)
Write-Host ("  layout   : {0}" -f $Layout)
Write-Host ("  sink dll : {0}" -f $SinkDll)
Write-Host ("  ring     : {0}  (4096 frames, float32, SAL order)" -f $Ring)
Write-Host ("  gain     : {0}   prebuffer: {1} ms   latency: {2}   duration: {3} s" -f $Gain, $PrebufferMs, $Latency, $Seconds)
Write-Host ''

$csharp = @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace NspBridgeTest
{
    public static class Sink
    {
        private const string Dll = @"__SINK_DLL__";

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern IntPtr nsp_shm_create(string name, uint ringFrames, uint format, uint layout);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern int nsp_shm_push(IntPtr sink, float[] samples, uint frames);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern UIntPtr nsp_shm_report(IntPtr sink, IntPtr buffer, UIntPtr size);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern int nsp_shm_close(IntPtr sink);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern int nsp_shm_error();

        public static int LastError() { return nsp_shm_error(); }

        public static IntPtr Create(string name, uint ringFrames, uint format, uint layout)
        {
            return nsp_shm_create(name, ringFrames, format, layout);
        }

        public static int Push(IntPtr sink, float[] samples, uint frames)
        {
            return nsp_shm_push(sink, samples, frames);
        }

        public static string ReportJson(IntPtr sink)
        {
            UIntPtr needed = nsp_shm_report(sink, IntPtr.Zero, UIntPtr.Zero);
            int size = (int)(ulong)needed;
            if (size <= 0 || size > 1024 * 1024) { return null; }
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                if ((int)(ulong)nsp_shm_report(sink, buffer, needed) != size) { return null; }
                // 报告不一定以 NUL 结尾，按字节长度复制，不能靠 PtrToStringAnsi 找结尾。
                byte[] bytes = new byte[size];
                Marshal.Copy(buffer, bytes, 0, size);
                return Encoding.ASCII.GetString(bytes);
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }

        public static int Close(IntPtr sink) { return nsp_shm_close(sink); }
    }

    public static class Producer
    {
        public const int SampleRate = 48000;
        public const int Channels = 12;

        // nsp_shm_push 的输入顺序是渲染器自己的 ROAR Layout714 顺序。
        private static readonly string[] Names =
        {
            "FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR", "TFL", "TFR", "TBL", "TBR"
        };
        private static readonly double[] Frequency =
        {
            440.0, 494.0, 523.0, 60.0, 587.0, 659.0, 698.0, 784.0, 880.0, 988.0, 1047.0, 1175.0
        };

        private static Thread _worker;
        private static volatile bool _stop;
        private static long _frames;
        private static int _error;

        public static long FramesPushed { get { return Interlocked.Read(ref _frames); } }
        public static bool PushFailed { get { return _error != 0; } }
        public static int PushError { get { return _error; } }

        public static string Describe()
        {
            StringBuilder text = new StringBuilder();
            for (int c = 0; c < Channels; c++)
            {
                text.AppendFormat("  ROAR[{0,2}] {1,-3} {2,6:0} Hz  -> 会轮到 0.5 s\n",
                    c, Names[c], Frequency[c]);
            }
            return text.ToString();
        }

        public static void Begin(IntPtr sink, int seconds, double amplitude)
        {
            _stop = false;
            _error = 0;
            Interlocked.Exchange(ref _frames, 0L);
            _worker = new Thread(delegate () { Run(sink, seconds, amplitude); });
            _worker.IsBackground = true;
            _worker.Priority = ThreadPriority.AboveNormal;
            _worker.Start();
        }

        public static void Stop()
        {
            _stop = true;
            Thread worker = _worker;
            if (worker != null) { worker.Join(3000); }
            _worker = null;
        }

        private static void Run(IntPtr sink, int seconds, double amplitude)
        {
            const int blockFrames = 480;
            const int segment = SampleRate / 2;      // 每个声道点亮 0.5 s
            const int fade = SampleRate / 200;       // 5 ms 淡入淡出，避免咔哒声
            double twoPi = Math.PI * 2.0;

            double[] step = new double[Channels];
            double[] phase = new double[Channels];
            for (int c = 0; c < Channels; c++)
            {
                step[c] = twoPi * Frequency[c] / SampleRate;
            }

            float[] block = new float[blockFrames * Channels];
            long sent = 0;
            Stopwatch clock = Stopwatch.StartNew();

            while (!_stop && clock.Elapsed.TotalSeconds < seconds)
            {
                long target = (long)(clock.Elapsed.TotalSeconds * SampleRate);
                while (!_stop && sent < target)
                {
                    int frames = (int)Math.Min((long)blockFrames, target - sent);
                    for (int i = 0; i < frames; i++)
                    {
                        long absolute = sent + i;
                        int active = (int)((absolute / segment) % Channels);
                        int within = (int)(absolute % segment);
                        double envelope = 1.0;
                        if (within < fade) { envelope = within / (double)fade; }
                        else if (within > segment - fade) { envelope = (segment - within) / (double)fade; }

                        double sample = Math.Sin(phase[active]) * amplitude * envelope;
                        phase[active] += step[active];
                        if (phase[active] > twoPi) { phase[active] -= twoPi; }

                        int at = i * Channels;
                        for (int c = 0; c < Channels; c++)
                        {
                            block[at + c] = (c == active) ? (float)sample : 0.0f;
                        }

                        // 非激活声道也要推进相位，切回来时波形才连续。
                        for (int c = 0; c < Channels; c++)
                        {
                            if (c == active) { continue; }
                            phase[c] += step[c];
                            if (phase[c] > twoPi) { phase[c] -= twoPi; }
                        }
                    }

                    int rc = Sink.Push(sink, block, (uint)frames);
                    if (rc != 0)
                    {
                        _error = rc;
                        _stop = true;
                        return;
                    }
                    sent += frames;
                    Interlocked.Exchange(ref _frames, sent);
                }
                Thread.Sleep(1);
            }
        }
    }
}
'@

$csharp = $csharp.Replace('__SINK_DLL__', $SinkDll)
if (-not ('NspBridgeTest.Sink' -as [type])) {
    Add-Type -TypeDefinition $csharp -Language CSharp | Out-Null
}

$sink = [NspBridgeTest.Sink]::Create($Ring, 4096, 1, 1)   # format f32 = 1, layout SAL = 1
if ($sink -eq [IntPtr]::Zero) {
    throw ("创建共享内存环失败，nsp_shm_error = {0}" -f [NspBridgeTest.Sink]::LastError())
}
Write-Host '共享内存环已创建，开始推送测试信号：' -ForegroundColor Green
Write-Host ([NspBridgeTest.Producer]::Describe())

$cliSeconds = $Seconds + 1

if ($ProducerOnly) {
    [NspBridgeTest.Producer]::Begin($sink, $Seconds, $Amplitude)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt ($Seconds + 2) -and -not [NspBridgeTest.Producer]::PushFailed) {
        Start-Sleep -Milliseconds 500
    }
    [NspBridgeTest.Producer]::Stop()
    $producerReport = [NspBridgeTest.Sink]::ReportJson($sink)
    [NspBridgeTest.Sink]::Close($sink) | Out-Null
    Write-Host ''
    Write-Host '--- 环计数（只跑生产端）---' -ForegroundColor Cyan
    Write-Host $producerReport
    if ([NspBridgeTest.Producer]::PushFailed) {
        Write-Host ("结果：FAIL —— nsp_shm_push 错误码 {0}" -f [NspBridgeTest.Producer]::PushError) -ForegroundColor Red
        exit 1
    }
    Write-Host '结果：PASS —— 生产端按契约把 12 声道 float32 推进了环。' -ForegroundColor Green
    Write-Host '注意：没有消费端时环每 push 都会溢出，droppedFrames 会一直涨，这个数字只有在消费端在场时才有意义。' -ForegroundColor DarkGray
    exit 0
}

$cliArgs = @(
    'live-nsp-layout'
    "$cliSeconds"
    ('"{0}"' -f $Layout)
    $Gain.ToString([Globalization.CultureInfo]::InvariantCulture)
    "$PrebufferMs"
    $Latency
    $Ring
)

Write-Host ''
Write-Host ("启动消费端：{0} {1}" -f (Split-Path -Leaf $Cli), ($cliArgs -join ' ')) -ForegroundColor Cyan

$startInfo = New-Object Diagnostics.ProcessStartInfo
$startInfo.FileName = $Cli
$startInfo.Arguments = ($cliArgs -join ' ')
$startInfo.WorkingDirectory = Split-Path -Parent $Cli
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
$startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
$process = [Diagnostics.Process]::Start($startInfo)
# 必须异步读，否则管道写满会把子进程卡死。
$cliOutTask = $process.StandardOutput.ReadToEndAsync()
$cliErrTask = $process.StandardError.ReadToEndAsync()

# 给 renderer 一点时间打开三张声卡，再开始喂数据。
Start-Sleep -Milliseconds 400
[NspBridgeTest.Producer]::Begin($sink, $Seconds, $Amplitude)

$lastReport = 0
$timedOut = $false
$watch = [Diagnostics.Stopwatch]::StartNew()
while (-not $process.HasExited) {
    Start-Sleep -Milliseconds 500
    $pushed = [NspBridgeTest.Producer]::FramesPushed
    if ($pushed -ne $lastReport) {
        $lastReport = $pushed
        Write-Host ("  已推送 {0:N0} frames ({1:N1} s)" -f $pushed, ($pushed / 48000.0))
    }
    if ($watch.Elapsed.TotalSeconds -gt ($cliSeconds + 20)) {
        Write-Host '消费端超时，强制结束。' -ForegroundColor Yellow
        $timedOut = $true
        try { $process.Kill() } catch { }
        break
    }
}

[NspBridgeTest.Producer]::Stop()
if (-not $process.HasExited) {
    $process.WaitForExit(10000) | Out-Null
    if (-not $process.HasExited) { try { $process.Kill() } catch { } }
}
$exitCode = -1
try { $exitCode = $process.ExitCode } catch { }

$reportJson = $null
try { $reportJson = [NspBridgeTest.Sink]::ReportJson($sink) } catch { }
[NspBridgeTest.Sink]::Close($sink) | Out-Null

Write-Host ''
Write-Host '--- 消费端输出 ---' -ForegroundColor Cyan
$cliText = ''
$cliError = ''
try { $cliText = $cliOutTask.Result } catch { }
try { $cliError = $cliErrTask.Result } catch { }
if ($cliText) { Write-Host $cliText.TrimEnd() }
if ($cliError) { Write-Host $cliError.TrimEnd() -ForegroundColor Yellow }

Write-Host ''
Write-Host '--- 环计数（生产端视图）---' -ForegroundColor Cyan
$report = $null
if ($reportJson) {
    Write-Host $reportJson
    try { $report = $reportJson | ConvertFrom-Json } catch { }
}
else {
    Write-Host '(拿不到 report)'
}
if ([NspBridgeTest.Producer]::PushFailed) {
    Write-Host ("nsp_shm_push 失败，错误码 {0}" -f [NspBridgeTest.Producer]::PushError) -ForegroundColor Red
}

$problems = New-Object System.Collections.Generic.List[string]
if ($exitCode -ne 0) { $problems.Add("消费端退出码 $exitCode") }
if ($cliText -notmatch 'Live NSP playback complete') { $problems.Add('消费端没有走到 "Live NSP playback complete"') }
if (($cliText + $cliError) -match 'endpoint filter must match exactly one active device') {
    $problems.Add("布局里的端点在本机没有唯一匹配：$Layout。用 -Layout 指向当前 Studio 正在用的 profile，或先在设备管理器里确认三张声卡都在。")
}
if ($timedOut -and -not $cliText -and -not $cliError) {
    $problems.Add('消费端一直没有任何输出也没退出，两种可能：卡在 WASAPI 端点初始化，或者卡在等环（那句等待提示走的是 wcout，重定向时会丢掉）。先用 SpatialAudioLab.CLI.exe render-test 1 单独确认本机 WASAPI 是否正常。')
}
if ($cliText -match 'Waiting for NSP ring') {
    $problems.Add('消费端在等环：它没认出生产端建的映射，检查 -Ring 名字是否与生产端一致。')
}
if (-not $report) { $problems.Add('取不到环报告') }
else {
    if ([int64]$report.droppedFrames -gt 0) { $problems.Add("droppedFrames = $($report.droppedFrames)，消费端跟不上") }
    if ([int64]$report.readFrames -le 0) { $problems.Add('readFrames = 0，消费端从没读过环') }
    if ([int64]$report.writtenFrames -le 0) { $problems.Add('writtenFrames = 0，生产端没推上去') }
}

Write-Host ''
if ($problems.Count -eq 0) {
    Write-Host '结果：PASS —— 环契约、SAL 声道顺序、消费端渲染全部打通。' -ForegroundColor Green
    Write-Host '请确认：12 个声道依次各响 0.5 s，方向与声道名一致（TFL/TFR/TBL/TBR 应来自头顶）。'
}
else {
    Write-Host '结果：FAIL' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host ("  - {0}" -f $problem) -ForegroundColor Red }
}

if ($problems.Count -ne 0) { exit 1 }
