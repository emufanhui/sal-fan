[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $repoRoot 'build-mat-recorder'
$publishRoot = Join-Path $repoRoot 'build'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake was not found. Install CMake and the Visual Studio C++ workload.'
}

New-Item -ItemType Directory -Path $publishRoot -Force | Out-Null
Write-Host 'Configuring standalone MAT recorder (no driver build or install)...'
& cmake -S $repoRoot -B $buildRoot -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

Write-Host 'Building only SpatialAudioLab.MatRecorder...'
& cmake --build $buildRoot --config Release --target SpatialAudioLab.MatRecorder
if ($LASTEXITCODE -ne 0) { throw "MAT recorder build failed: $LASTEXITCODE" }

$exe = Get-ChildItem -LiteralPath $buildRoot -Filter 'SpatialAudioLab.MatRecorder.exe' -File -Recurse |
    Select-Object -First 1
if ($null -eq $exe) { throw 'Build succeeded but SpatialAudioLab.MatRecorder.exe was not found.' }

$destination = Join-Path $publishRoot 'SpatialAudioLab.MatRecorder.exe'
Copy-Item -LiteralPath $exe.FullName -Destination $destination -Force
Write-Host "Built: $destination"
