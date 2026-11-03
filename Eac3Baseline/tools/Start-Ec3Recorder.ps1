[CmdletBinding()]
param(
    [switch]$Build
)

# The recorder is a standalone tool: it runs out of its own build folder and
# finds the CLI by walking up to the repo, so it never needs a publish layout.
$projectRoot = Join-Path $PSScriptRoot 'Ec3Recorder'

# The target framework moniker carries the Windows SDK version, so the output
# folder is discovered rather than hardcoded.
function Find-Recorder {
    $releaseRoot = Join-Path $projectRoot 'bin\Release'
    if (-not (Test-Path -LiteralPath $releaseRoot)) { return $null }
    $found = Get-ChildItem $releaseRoot -Recurse -File -Filter 'SpatialAudioLab.Ec3Recorder.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}

$executable = Find-Recorder
$sources = @(Get-ChildItem $projectRoot -Recurse -File -Include *.cs, *.xaml, *.csproj)

$needsBuild = $Build -or -not $executable
if (-not $needsBuild) {
    $exeTime = (Get-Item -LiteralPath $executable).LastWriteTimeUtc
    $needsBuild = [bool]($sources | Where-Object { $_.LastWriteTimeUtc -gt $exeTime })
}

if ($needsBuild) {
    Write-Host 'Building SpatialAudioLab.Ec3Recorder...'
    & dotnet build (Join-Path $projectRoot 'Ec3Recorder.csproj') -c Release -v q --nologo
    if ($LASTEXITCODE -ne 0) { throw "Recorder build failed: $LASTEXITCODE" }
    $executable = Find-Recorder
}

if (-not $executable -or -not (Test-Path -LiteralPath $executable)) {
    throw "Recorder executable was not produced under $projectRoot\bin\Release"
}

Write-Host "Starting: $executable"
Start-Process -FilePath $executable -Verb RunAs
