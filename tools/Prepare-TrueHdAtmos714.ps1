[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InputFile,

    [ValidateRange(0, 86400)]
    [double]$DurationSeconds = 0,

    [string]$OutputBase = '',

    [ValidateRange(0.0, 2.0)]
    [double]$Gain = 0.707,

    [switch]$KeepIntermediate
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$inputPath = (Resolve-Path -LiteralPath $InputFile).Path
$extension = [IO.Path]::GetExtension($inputPath).ToLowerInvariant()

if ([string]::IsNullOrWhiteSpace($OutputBase)) {
    $OutputBase = Join-Path $repoRoot 'captures\\truehd-atmos-714'
} elseif (-not [IO.Path]::IsPathRooted($OutputBase)) {
    $OutputBase = Join-Path (Get-Location) $OutputBase
}
$OutputBase = [IO.Path]::GetFullPath($OutputBase)
$outputDirectory = Split-Path $OutputBase -Parent
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$truehdd = Join-Path $PSScriptRoot 'truehdd\\truehdd.exe'
$extractor = Join-Path $repoRoot 'mat_truehd_extractor_v03.py'
$cavernProject = Join-Path $PSScriptRoot 'Cavern714\\Cavern714.csproj'
$cavernDll = Join-Path $PSScriptRoot 'Cavern714\\bin\\Release\\net8.0\\Cavern714.dll'

if (-not (Test-Path -LiteralPath $truehdd)) {
    throw "truehdd.exe not found. Run .\\tools\\Install-Truehdd.ps1 first."
}
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'dotnet SDK was not found.'
}

$trueHdPath = $inputPath
if ($extension -ne '.thd') {
    if ($extension -notin @('.mat', '.mlp')) {
        throw 'Input must be a raw MAT payload file (.mat/.mlp) or an extracted TrueHD AU stream (.thd).'
    }
    if (-not (Test-Path -LiteralPath $extractor)) {
        throw "MAT AU extractor not found: $extractor"
    }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw 'Python was not found. Install Python and NumPy to run mat_truehd_extractor_v03.py.'
    }
    & $python.Source -c 'import numpy' 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw 'NumPy is required by mat_truehd_extractor_v03.py. Install it with: python -m pip install numpy'
    }
    $trueHdPath = "$OutputBase.thd"
    Write-Host 'MAT payload -> validated TrueHD access units...'
    & $python.Source $extractor $inputPath --write-thd --output $trueHdPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $trueHdPath)) {
        throw "MAT-to-TrueHD AU extraction failed (exit $LASTEXITCODE)."
    }
}

Write-Host 'Validating TrueHD stream...'
& $truehdd info $trueHdPath
if ($LASTEXITCODE -ne 0) {
    throw "truehdd info failed (exit $LASTEXITCODE)."
}

$damfBase = "$OutputBase-damf"
$atmosPath = "$damfBase.atmos"
$wavePath = "$OutputBase-714.wav"
Remove-Item "$damfBase.atmos", "$damfBase.atmos.audio", "$damfBase.atmos.metadata", $wavePath -Force -ErrorAction SilentlyContinue

Write-Host 'Decoding Dolby Atmos presentation 3...'
& $truehdd decode --progress --presentation 3 $trueHdPath --output-path $damfBase
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $atmosPath)) {
    throw 'truehdd did not produce presentation 3 DAMF. Check that the stream contains Atmos object metadata.'
}

Write-Host 'Building the 7.1.4 renderer...'
& dotnet build $cavernProject -c Release
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cavernDll)) {
    throw "Cavern714 build failed (exit $LASTEXITCODE)."
}

Write-Host 'Rendering Atmos objects to 12-channel 7.1.4 PCM...'
$gainText = $Gain.ToString([Globalization.CultureInfo]::InvariantCulture)
if ($DurationSeconds -gt 0) {
    $durationText = $DurationSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    & dotnet $cavernDll render $atmosPath $wavePath $durationText $gainText
} else {
    & dotnet $cavernDll render $atmosPath $wavePath 0 $gainText
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wavePath)) {
    throw "Cavern714 render failed (exit $LASTEXITCODE)."
}

if (-not $KeepIntermediate -and $extension -ne '.thd') {
    Remove-Item $trueHdPath -Force -ErrorAction SilentlyContinue
}
if (-not $KeepIntermediate) {
    Remove-Item "$damfBase.atmos", "$damfBase.atmos.audio", "$damfBase.atmos.metadata" -Force -ErrorAction SilentlyContinue
}

Write-Host "Ready: $wavePath"
Write-Host 'Output: 12-channel, 48 kHz (source rate), PCM16, 7.1.4.'
