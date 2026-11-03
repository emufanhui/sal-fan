<#
.SYNOPSIS
    实时把播放器直通（bitstream）出来的 E-AC-3 码流解成 7.1.4，输出到三张声卡。

.DESCRIPTION
    链路：
      Apple Music / 其它播放器 --bitstream--> SysVAD 虚拟驱动 --> \\.\DolbyDecoderMat
        --> SpatialAudioLab.CLI ec3-pipe        剥出裸 E-AC-3 syncframe
        --> Engine\Eac3Bridge\Eac3Pipe714.exe   Cavern 解码 + 对象渲染 -> ref714
        --> \\.\pipe\SpatialAudioLab.Pcm714
        --> SpatialAudioLab.CLI omniphony-pcm   交叉分频 + 三卡输出

    必须以管理员运行：\\.\DolbyDecoderMat 的 IOCTL 会返回 ERROR_ACCESS_DENIED(5)。

.EXAMPLE
    .\Start-Eac3Live.ps1
.EXAMPLE
    .\Start-Eac3Live.ps1 -Layout C:\path\profile.ini -Gain 0.2
#>
[CmdletBinding()]
param(
    # 12 声道（7.1.4）布局。留空则用最新的 Studio profile。
    [string]$Layout,
    # 0 = 一直跑（引擎里 seconds==0 就是 unlimited）。给正数会在到点后
    # 主动关管道，桥跟着退出，看起来就像“跑一会儿突然停了”。
    [ValidateRange(0, 86400)][int]$Seconds = 0,
    [ValidateRange(0.0, 1.0)][double]$Gain = 0.25,
    [ValidateRange(20, 500)][int]$PrebufferMs = 40,
    [ValidateSet('safe', 'balanced', 'low')][string]$Latency = 'balanced',
    # 码流管道名（CLI ec3-pipe 建服务端，解码桥连上来）。
    [string]$StreamPipe = 'SpatialAudioLab.Eac3'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '需要管理员权限（\\.\DolbyDecoderMat 要求提权），正在重新以管理员启动 ...' -ForegroundColor Yellow
    $forward = ($PSBoundParameters.GetEnumerator() | ForEach-Object {
        if ($_.Value -is [switch]) { "-$($_.Key)" } else { "-$($_.Key) `"$($_.Value)`"" }
    }) -join ' '
    Start-Process powershell -Verb RunAs -ArgumentList (
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" $forward")
    return
}

function Resolve-First {
    param([string[]]$Candidates, [string]$What)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "找不到$What，已尝试：`n  $($Candidates -join "`n  ")"
}

# 脚本可能放在发布包根目录，也可能放在源码树的 tools\ 下。
$roots = @($PSScriptRoot, (Split-Path -Parent $PSScriptRoot))
$cli = Resolve-First -What ' SpatialAudioLab.CLI.exe' -Candidates ($roots | ForEach-Object {
        @((Join-Path $_ 'Engine\SpatialAudioLab.CLI.exe'),
          (Join-Path $_ 'build-native\SpatialAudioLab.CLI.exe'),
          (Join-Path $_ 'build\SpatialAudioLab.CLI.exe'))
    } | ForEach-Object { $_ })
$bridge = Resolve-First -What ' Eac3Pipe714.exe' -Candidates ($roots | ForEach-Object {
        @((Join-Path $_ 'Engine\Eac3Bridge\Eac3Pipe714.exe'),
          (Join-Path $_ 'tools\Eac3Pipe714\bin\Release\net8.0\Eac3Pipe714.exe'))
    } | ForEach-Object { $_ })

if (-not $Layout) {
    # 先认名字：这是这套三卡（USB 8ch + 光纤 2ch + KT 2ch）实际在用的那份。
    # 不要按“最新修改时间”猜——Studio 每存一次就多一份副本，
    # 猜中的往往是别人拷来试的残留。
    $preferredPath = $null
    foreach ($root in $roots) {
        $candidate = Join-Path $root 'Data\Profiles\USB3+OP+KT.ini'
        if (Test-Path -LiteralPath $candidate) { $preferredPath = $candidate; break }
    }
    if ($preferredPath) {
        $Layout = $preferredPath
    } else {
        $profiles = @()
        foreach ($root in $roots) {
            $profiles += Get-ChildItem -Path (Join-Path $root 'Data\Profiles\*.ini') -ErrorAction SilentlyContinue
            $profiles += Get-ChildItem -Path (Join-Path $root 'configs\*.ini') -ErrorAction SilentlyContinue
        }
        $newest = $profiles | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $newest) { throw '找不到任何布局 ini，请用 -Layout 指定。' }
        $Layout = $newest.FullName
    }
}
$Layout = (Resolve-Path -LiteralPath $Layout).Path

Write-Host ''
Write-Host '=== E-AC-3 实时解码 -> 7.1.4 三卡 ===' -ForegroundColor Cyan
Write-Host ("  CLI     : {0}" -f $cli)
Write-Host ("  解码桥  : {0}" -f $bridge)
Write-Host ("  layout  : {0}" -f $Layout)
Write-Host ("  码流管道: \\.\pipe\{0}" -f $StreamPipe)
Write-Host ''
Write-Host '启动后，请在 Apple Music 播放一首 Atmos 曲目（设置里开启杜比全景声），' -ForegroundColor Yellow
Write-Host '并把系统输出设备切到 SysVAD 虚拟驱动。Ctrl+C 结束。' -ForegroundColor Yellow
Write-Host ''

$children = New-Object System.Collections.Generic.List[object]
try {
    # 顺序有讲究：PCM 管道是 CLI 当服务端，解码桥连它；码流管道也是 CLI 当服务端。
    $children.Add((Start-Process -FilePath $cli -NoNewWindow -PassThru -ArgumentList @(
                'omniphony-pcm', "$Seconds", "`"$Layout`"",
                $Gain.ToString([Globalization.CultureInfo]::InvariantCulture),
                "$PrebufferMs", $Latency)))
    Start-Sleep -Milliseconds 1200

    $children.Add((Start-Process -FilePath $bridge -NoNewWindow -PassThru -ArgumentList @(
                '--input-pipe', $StreamPipe)))
    Start-Sleep -Milliseconds 1200

    $children.Add((Start-Process -FilePath $cli -NoNewWindow -PassThru -ArgumentList @(
                'ec3-pipe', $StreamPipe)))

    while ($true) {
        Start-Sleep -Milliseconds 500
        $dead = $children | Where-Object { $_.HasExited }
        if ($dead) {
            Write-Host ''
            Write-Host ("有进程已退出：{0}" -f (($dead | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) -join ', ')) -ForegroundColor Yellow
            break
        }
    }
}
catch {
    Write-Host ''
    Write-Host ("启动失败：{0}" -f $_.Exception.Message) -ForegroundColor Red
    Write-Host '常见原因：布局 ini 里的 endpoint_id 过期、虚拟驱动没装、或某个端点被独占。' -ForegroundColor Yellow
    # 提权窗口一关就什么都看不到了，这里停住让人看清错误。
    if ($Host.Name -eq 'ConsoleHost') { Read-Host '按回车关闭窗口' | Out-Null }
}
finally {
    Write-Host '清理进程 ...' -ForegroundColor DarkGray
    foreach ($child in $children) {
        if (-not $child.HasExited) { try { $child.Kill() } catch { } }
    }
    if ($Host.Name -eq 'ConsoleHost') {
        Read-Host '按回车关闭窗口' | Out-Null
    }
}
