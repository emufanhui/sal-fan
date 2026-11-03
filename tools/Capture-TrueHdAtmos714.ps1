[CmdletBinding()]
param(
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 30,

    [string]$OutputBase = '',

    [ValidateRange(0.0, 2.0)]
    [double]$Gain = 0.707,

    [switch]$KeepIntermediate
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The MAT capture device is restricted to administrators.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$recorder = Join-Path $repoRoot 'build\SpatialAudioLab.MatRecorder.exe'
if (-not (Test-Path -LiteralPath $recorder)) {
    & (Join-Path $PSScriptRoot 'Build-MatRecorder.ps1')
}
if (-not (Test-Path -LiteralPath $recorder)) {
    throw "MAT recorder executable was not created: $recorder"
}

if ([string]::IsNullOrWhiteSpace($OutputBase)) {
    $OutputBase = Join-Path $repoRoot ("captures\truehd-mat-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
} elseif (-not [IO.Path]::IsPathRooted($OutputBase)) {
    $OutputBase = Join-Path (Get-Location) $OutputBase
}
$OutputBase = [IO.Path]::GetFullPath($OutputBase)
$outputDirectory = Split-Path $OutputBase -Parent
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$matPath = "$OutputBase.mat"

Write-Host "Capturing MAT for $DurationSeconds seconds."
Write-Host 'Start the player and begin TrueHD bitstream playback now.'
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $recorder
$psi.Arguments = 'mat-recorder "' + $matPath + '" 2'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.CreateNoWindow = $false

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $psi
if (-not $process.Start()) { throw 'Could not start SpatialAudioLab.MatRecorder.exe.' }

try {
    Start-Sleep -Seconds $DurationSeconds
    if (-not $process.HasExited) {
        $process.StandardInput.WriteLine('STOP')
        $process.StandardInput.Flush()
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            throw 'MAT recorder did not stop within 10 seconds.'
        }
    }
} finally {
    $process.Dispose()
}

if (-not (Test-Path -LiteralPath $matPath) -or (Get-Item -LiteralPath $matPath).Length -eq 0) {
    throw 'No MAT payload was captured. Confirm the player is sending Dolby MAT/TrueHD to the SysVAD capture endpoint.'
}
Write-Host "Captured MAT payload: $matPath ($((Get-Item -LiteralPath $matPath).Length) bytes)"

$prepare = Join-Path $PSScriptRoot 'Prepare-TrueHdAtmos714.ps1'
& $prepare -InputFile $matPath -OutputBase $OutputBase -Gain $Gain -KeepIntermediate:$KeepIntermediate
if ($LASTEXITCODE -ne 0) {
    throw "TrueHD/Atmos 7.1.4 preparation failed: $LASTEXITCODE"
}
