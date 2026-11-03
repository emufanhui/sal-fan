$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$log = Join-Path $here 'diag-out.txt'
$probeLog = 'C:\Windows\Temp\sal_eac3_probe.log'
Start-Transcript -Path $log -Force | Out-Null

try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    "=== admin=$($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) user=$($identity.Name) ==="

    "audiodg before:"
    Get-Process audiodg -ErrorAction SilentlyContinue | Format-Table Id, StartTime | Out-String

    Remove-Item $probeLog -Force -ErrorAction SilentlyContinue
    Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue
    "killed audiodg"

    # Stop-Process is asynchronous: the old instance lingers for a moment and
    # would be picked up as the "respawned" one.
    for ($i = 0; $i -lt 80; $i++) {
        if (-not (Get-Process audiodg -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 250
    }
    "old instance gone, waiting for a new one (keep playing audio)"

    $process = $null
    for ($i = 0; $i -lt 160; $i++) {
        $candidate = Get-Process audiodg -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) { $process = $candidate; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $process) {
        "audiodg never respawned - no audio was playing"
        return
    }
    "audiodg respawned: pid=$($process.Id) at $($process.StartTime)"
    Start-Sleep -Milliseconds 1200

    $injector = Join-Path $here 'bin\SalEac3Injector.exe'
    $probe = Join-Path $here 'bin\SalEac3Probe_v3.dll'
    & $injector $process.Id $probe
    "injector exit=$LASTEXITCODE"

    Start-Sleep -Seconds 18

    if (Test-Path $probeLog) {
        $item = Get-Item $probeLog
        "probe log: $($item.Length) bytes, written $($item.LastWriteTime)"
        "--- head ---"
        Get-Content $probeLog -TotalCount 4
        "--- KsHeader ---"
        Select-String -Path $probeLog -Pattern 'KsHeader' | Select-Object -First 2 |
            ForEach-Object { $_.Line }
        "--- KsWrite / Fault ---"
        Select-String -Path $probeLog -Pattern 'KsWriteStream|KsWriteFault' | Select-Object -First 2 |
            ForEach-Object { $_.Line }
        "--- tail ---"
        Get-Content $probeLog -Tail 6
    } else {
        "probe log missing - probe never wrote"
    }
} finally {
    Stop-Transcript | Out-Null
}
