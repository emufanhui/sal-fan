param(
  [string]$Clang = 'C:\Program Files\LLVM\bin\clang.exe',
  [string]$MinHook = 'C:\Users\fanhu\CodeBuddy\20261024120430\NativeSpatialPipe\work\minhook'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not (Test-Path (Join-Path $MinHook 'include\MinHook.h'))) {
  throw "MinHook 源码不在 $MinHook，用 -MinHook 指定"
}

& $Clang -O2 -fms-extensions -shared -Wall -Wextra -I (Join-Path $MinHook 'include') `
  (Join-Path $here 'SalEac3Probe.c') `
  (Join-Path $MinHook 'src\hook.c') `
  (Join-Path $MinHook 'src\buffer.c') `
  (Join-Path $MinHook 'src\trampoline.c') `
  (Join-Path $MinHook 'src\hde\hde64.c') `
  -o (Join-Path $out 'SalEac3Probe.dll')
if ($LASTEXITCODE) { throw 'probe build failed' }

& $Clang -O2 -municode -Wall -Wextra (Join-Path $here 'SalEac3Injector.c') `
  -ladvapi32 -o (Join-Path $out 'SalEac3Injector.exe')
if ($LASTEXITCODE) { throw 'injector build failed' }

Get-ChildItem $out | Select-Object Name, Length
