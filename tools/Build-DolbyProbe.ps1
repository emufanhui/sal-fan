[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $repoRoot 'build-native'
$publishRoot = Join-Path $repoRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 with the C++ workload.'
}
$visualStudio = & $vswhere -latest -products '*' -property installationPath
if (-not $visualStudio) {
    throw 'Visual Studio 2022 was not found.'
}
$devCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Path $buildRoot, $publishRoot -Force | Out-Null

$sources = @(
    'audio_platform.cpp',
    'bridge_meter.cpp',
    'capture_commands.cpp',
    'crossover_stage.cpp',
    'dolby_probe.cpp',
    'dtsx_analysis.cpp',
    'layout_mix.cpp',
    'media_foundation_probe.cpp',
    'mat_analysis.cpp',
    'mat_capture_client.cpp',
    'mat_format.cpp',
    'mat_pipeline.cpp',
    'multi_endpoint_renderer.cpp',
    'nsp_ring_reader.cpp',
    'omniphony_pcm_bridge.cpp',
    'pcm_pipeline.cpp',
    'speaker_layout.cpp',
    'spatial_audio_sample.cpp',
    'wave_io.cpp'
) | ForEach-Object { '"..\src\' + $_ + '"' }

$compile = @(
    # /O2 matches the CMake Release build the published CLI comes from. Without
    # it this path produces an unoptimised binary, which is not what a 12-channel
    # realtime renderer should run on.
    'cl /nologo /std:c++20 /EHsc /W4 /permissive- /O2',
    '/DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
    ($sources -join ' '),
    '/Fe:SpatialAudioLab.CLI.exe',
    # shell32: SHGetKnownFolderPath, used to find the crossover config.
    '/link ole32.lib shell32.lib runtimeobject.lib windowsapp.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib',
    'mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib'
) -join ' '
$command = '"' + $devCmd + '" -arch=x64 -host_arch=x64 >nul && cd /d "' +
    $buildRoot + '" && ' + $compile

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "SpatialAudioLab CLI build failed: $LASTEXITCODE"
}
Copy-Item (Join-Path $buildRoot 'SpatialAudioLab.CLI.exe') `
    (Join-Path $publishRoot 'SpatialAudioLab.CLI.exe') -Force -ErrorAction Stop
Write-Host "Built: $(Join-Path $publishRoot 'SpatialAudioLab.CLI.exe')"
