#Requires -Version 5.1
<#
.SYNOPSIS
    重新编译并安装 SysVAD 虚拟声卡驱动，并校验"装的到底是哪一份二进制"。

.DESCRIPTION
    为什么需要这个脚本：
      * 之前出现过"编了新 .sys、但 DriverStore 里还是旧的那份"——设备跑的一直是旧二进制。
        本脚本构建时用 /t:Rebuild，并在安装后比对 DriverStore 里 .sys 的 SHA256 与
        编译产物是否一致，不一致就明确报出来。
      * 驱动 .inx 之前被改成过 Root\SpatialAudioLabVirtualSink。带那个身份构建会创建
        新设备（旧 ROOT\MEDIA\0000 仍在，且端点 GUID/container 全变）。用
        -RestoreOriginalIdentity 可以先从 .bak 恢复原身份，最小改动地把驱动装回去。

    流程：环境检查 -> (可选)恢复身份 -> 构建 -> 产物校验 -> 证书 -> 安装 -> 验证。

.EXAMPLE
    # 推荐：恢复原身份后重编重装（最小风险，设备仍是 ROOT\MEDIA\0000）
    .\Rebuild-SysvadDriver.ps1 -RestoreOriginalIdentity
.EXAMPLE
    # 只编译不安装
    .\Rebuild-SysvadDriver.ps1 -RestoreOriginalIdentity -SkipInstall
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = 'C:\Users\fanhu\Documents\GitHub\spatial-audio-lab',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$TargetPlatformVersion = '10.0.26100.0',
    [string]$MSBuildPath = '',
    [switch]$RestoreOriginalIdentity,
    [switch]$SkipInstall,
    [switch]$NoElevate
)

$ErrorActionPreference = 'Stop'

function Write-Section([string]$Title) {
    Write-Host ''
    Write-Host "=== $Title ===" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- 提权
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$isAdmin = $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $NoElevate) {
    Write-Host '需要管理员权限，正在重新以管理员启动...'
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-RepoRoot', "`"$RepoRoot`"", '-Configuration', $Configuration,
        '-TargetPlatformVersion', $TargetPlatformVersion)
    if ($RestoreOriginalIdentity) { $arguments += '-RestoreOriginalIdentity' }
    if ($SkipInstall) { $arguments += '-SkipInstall' }
    Start-Process -Verb RunAs -FilePath 'powershell.exe' -ArgumentList $arguments
    return
}

$sysvad = Join-Path $RepoRoot 'driver\windows-driver-samples\audio\sysvad'
$tablet = Join-Path $sysvad 'TabletAudioSample'
$package = Join-Path $sysvad 'Package'
# 实际输出布局：<project>\x64\<Configuration>\，暂存包在同级 package\ 子目录
$buildRoot = Join-Path $package "x64\$Configuration"
$stageDir = Join-Path $buildRoot 'package'

# ---------------------------------------------------------------- 1. 环境
Write-Section '1. 环境检查'

$msbuild = $null
if ($MSBuildPath) {
    # 显式指定优先：WDK 的 VS 集成有时只装在旧版 VS 上，vswhere -latest 会选到
    # 没有 WindowsKernelModeDriver10.0 工具集的那份，导致 km 头文件解析不出来。
    if (-not (Test-Path $MSBuildPath)) { throw "指定的 MSBuild 不存在：$MSBuildPath" }
    $msbuild = $MSBuildPath
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not $msbuild -and (Test-Path $vswhere)) {
    $vsPath = (& $vswhere -latest -products '*' -property installationPath | Select-Object -First 1)
    if ($vsPath) {
        $candidate = Join-Path $vsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
        if (Test-Path $candidate) { $msbuild = $candidate }
    }
}
if (-not $msbuild) {
    foreach ($candidate in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe')) {
        if (Test-Path $candidate) { $msbuild = $candidate; break }
    }
}
if (-not $msbuild) { throw 'MSBuild 未找到（需要 Visual Studio 2022 + WDK）。先跑 tools\Install-WdkToolchain.ps1。' }
"MSBuild : $msbuild"

$wdkTargets = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\build\$TargetPlatformVersion\WindowsDriver.Common.targets"
"WDK     : $(if (Test-Path $wdkTargets) { 'OK' } else { "缺失（$wdkTargets）" })"
if (-not (Test-Path $wdkTargets)) { throw 'WDK ' + $TargetPlatformVersion + ' 未安装。' }

$startOptions = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
$hvci = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -ErrorAction SilentlyContinue).Enabled
$bcd = (& "$env:SystemRoot\System32\bcdedit.exe" /enum '{current}' 2>&1 | Out-String)
$testSigning = ($startOptions -match 'TESTSIGNING') -or [bool]($bcd -match '(?m)^\s*testsigning\s+Yes')
"启动档  : $startOptions"
"测试签名: $testSigning    内存完整性(HVCI): $(if ($hvci) { 'ON' } else { 'OFF' })"

$ciOk = $testSigning -or (($startOptions -match 'DISABLE_INTEGRITY_CHECKS') -and -not $hvci)
if (-not $ciOk) {
    Write-Warning '当前完整性设置下，测试签名驱动很可能加载失败（Code 52）。先关掉内存完整性后进 F7 档，或 bcdedit /set testsigning on。'
}

$devcon = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Tools\$TargetPlatformVersion\x64\devcon.exe"
"devcon  : $(if (Test-Path $devcon) { 'OK' } else { '缺失' })"

# ---------------------------------------------------------------- 2. 身份
Write-Section '2. 驱动身份'

$inxFiles = @('ComponentizedAudioSample.inx', 'ComponentizedAudioSampleExtension.inx', 'ComponentizedApoSample.inx')
if ($RestoreOriginalIdentity) {
    foreach ($name in $inxFiles) {
        $path = Join-Path $tablet $name
        $backup = "$path.bak"
        if (Test-Path $backup) {
            Copy-Item $backup $path -Force
            Write-Host "  已从 .bak 恢复：$name"
        }
        else {
            Write-Host "  $name 没有 .bak（可能本来就是原身份）"
        }
    }
}
foreach ($name in $inxFiles) {
    $path = Join-Path $tablet $name
    if (-not (Test-Path $path)) { continue }
    $text = [IO.File]::ReadAllText($path)
    $hwids = [regex]::Matches($text, 'Root\\[A-Za-z0-9_]+') | ForEach-Object { $_.Value } | Select-Object -Unique
    "  {0,-38} {1}" -f $name, ($hwids -join ', ')
}

# ---------------------------------------------------------------- 3. 构建
Write-Section '3. 构建（/m:1 /t:Rebuild）'

$projects = @(
    (Join-Path $sysvad 'EndpointsCommon\EndpointsCommon.vcxproj'),
    (Join-Path $tablet 'TabletAudioSample.vcxproj'),
    (Join-Path $package 'package.vcxproj')
)
$stagedSysBefore = Join-Path $stageDir 'TabletAudioSample.sys'
$beforeTime = if (Test-Path $stagedSysBefore) { (Get-Item $stagedSysBefore).LastWriteTime } else { $null }
if ($beforeTime) { "  构建前暂存包里的 .sys 时间戳: $beforeTime" }

foreach ($project in $projects) {
    if (-not (Test-Path $project)) { Write-Warning "跳过（不存在）：$project"; continue }
    Write-Host "  -> $(Split-Path $project -Leaf)"
    & $msbuild $project /m:1 /t:Rebuild /p:Configuration=$Configuration /p:Platform=x64 `
        /p:TargetPlatformVersion=$TargetPlatformVersion /nologo /v:m
    if ($LASTEXITCODE -ne 0) { throw "构建失败（退出码 $LASTEXITCODE）：$project" }
}

# ---------------------------------------------------------------- 4. 产物校验
Write-Section '4. 产物校验'

$stagedInf = Join-Path $stageDir 'ComponentizedAudioSample.inf'
$stagedSys = Join-Path $stageDir 'TabletAudioSample.sys'
$stagedCat = Join-Path $stageDir 'sysvad.cat'
foreach ($f in @($stagedInf, $stagedSys, $stagedCat)) {
    if (-not (Test-Path $f)) { throw "产物缺失：$f" }
}
$stagedHash = (Get-FileHash $stagedSys -Algorithm SHA256).Hash
"  暂存 .sys : $((Get-Item $stagedSys).LastWriteTime)  $stagedHash"
if ($beforeTime -and (Get-Item $stagedSys).LastWriteTime -eq $beforeTime) {
    Write-Warning '暂存 .sys 的时间戳没变——可能没有真正重新链接，请检查构建输出。'
}

$infText = [IO.File]::ReadAllText($stagedInf)
$hardwareId = ([regex]::Match($infText, 'Root\\[A-Za-z0-9_]+')).Value
$devicedesc = ([regex]::Match($infText, 'SYSVAD_SA\.DeviceDesc\s*=\s*"([^"]*)"')).Groups[1].Value
"  构建出的硬件 ID : $(if ($hardwareId) { $hardwareId } else { '未找到' })"
"  设备描述        : $devicedesc"
if (-not $hardwareId) { throw '无法从构建出的 INF 里解析硬件 ID。' }

if ($SkipInstall) {
    Write-Host ''
    Write-Host '已按 -SkipInstall 跳过安装。' -ForegroundColor Yellow
    return
}

# ---------------------------------------------------------------- 5. 安装
Write-Section '5. 安装'

$certificate = Join-Path $buildRoot 'package.cer'
if (-not (Test-Path $certificate)) { throw "找不到测试证书：$certificate" }
$x509 = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate)
foreach ($storeName in 'Root', 'TrustedPublisher') {
    $storePath = "Cert:\LocalMachine\$storeName\$($x509.Thumbprint)"
    if (Test-Path $storePath) {
        "  证书已在 LocalMachine\$storeName"
    }
    else {
        Import-Certificate -FilePath $certificate -CertStoreLocation "Cert:\LocalMachine\$storeName" | Out-Null
        "  已导入 LocalMachine\$storeName"
    }
}

$existing = @()
if (Test-Path $devcon) {
    $existing = @(& $devcon find $hardwareId 2>&1 | Where-Object { $_ -match ':\s' })
}
$instanceId = $null
if ($existing.Count -gt 0) {
    $instanceId = ($existing[0] -split '\s*:\s*')[0].Trim()
    "  已存在该硬件 ID 的设备：$instanceId —— 走更新路径"
    & pnputil.exe /add-driver $stagedInf /install | Out-Host
    & pnputil.exe /scan-devices | Out-Host
    if ($instanceId) { & pnputil.exe /restart-device $instanceId | Out-Host }
}
else {
    "  没有该硬件 ID 的设备 —— 创建根枚举设备"
    if (-not (Test-Path $devcon)) { throw '需要 devcon 来创建根枚举设备，但没找到。' }
    Push-Location $stageDir
    try { & $devcon install 'ComponentizedAudioSample.inf' $hardwareId | Out-Host } finally { Pop-Location }
}

# 环保持启用，但不落诊断 WAV
$parametersPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\sysvad_componentizedaudiosample\Parameters'
New-Item -Path $parametersPath -Force | Out-Null
New-ItemProperty -Path $parametersPath -Name 'DoNotCreateDataFiles' -PropertyType DWord -Value 1 -Force | Out-Null
"  DoNotCreateDataFiles=1（实时环保持启用）"

Start-Sleep -Seconds 3

# ---------------------------------------------------------------- 6. 验证
Write-Section '6. 验证'

$service = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\sysvad_componentizedaudiosample' -ErrorAction SilentlyContinue
if ($service -and $service.ImagePath) {
    $installed = $service.ImagePath -replace '^\\SystemRoot', $env:SystemRoot
    if (Test-Path $installed) {
        $installedHash = (Get-FileHash $installed -Algorithm SHA256).Hash
        "  DriverStore 里的 .sys : $((Get-Item $installed).LastWriteTime)  $installedHash"
        if ($installedHash -eq $stagedHash) {
            Write-Host '  PASS：设备绑定的是刚编译出的那份二进制。' -ForegroundColor Green
        }
        else {
            Write-Warning '  仍是旧二进制！重启一次，或在设备管理器里"更新驱动程序"指向构建产物目录。'
        }
    }
}
$problem = (& pnputil.exe /enum-devices /problem 2>&1 | Out-String)
$blocks = ($problem -split '(?m)^(?=Instance ID:)') | Where-Object { $_ -match 'SYSVAD|SpatialAudioLab|Root\\SpatialAudioLab' }
if ($blocks) {
    Write-Warning '  设备仍报问题：'
    $blocks | ForEach-Object { $_.Trim() }
    Write-Host '  查拦截原因：Get-WinEvent -LogName Microsoft-Windows-CodeIntegrity/Operational | ? Id -eq 3004 -First 3'
}
else {
    Write-Host '  PASS：问题设备列表里没有它。' -ForegroundColor Green
}
$svcState = Get-Service sysvad_componentizedaudiosample -ErrorAction SilentlyContinue
"  服务状态 : $($svcState.Status)"

Write-Host ''
Write-Host '下一步（这一步才决定 Studio 能不能收到 MAT）：' -ForegroundColor Yellow
Write-Host '  1) 该端点的空间音效/输出格式必须是 Dolby Atmos for Home Theater'
Write-Host "     .\tools\Set-SpatialProvider.ps1 -Mode Atmos -EndpointFilter '1 - HISENSE (SYSVAD (with APO Extensions))' -DeviceInstanceId 'ROOT\MEDIA\0000'"
Write-Host "  2) 验收判据（作者文档）：Native static mask 0xC1FFE, dynamic objects: 20"
Write-Host "     .\tools\Test-SpatialProvider.ps1 -Mode Atmos -EndpointFilter '1 - HISENSE (SYSVAD (with APO Extensions))'"
Write-Host '  3) 挂上电平监视，播点东西，看 updates 是否增长：'
Write-Host '     powershell -ExecutionPolicy Bypass -File .\Watch-BridgeMeters.ps1'
