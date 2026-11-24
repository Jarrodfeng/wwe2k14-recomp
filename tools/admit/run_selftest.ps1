# Runs THIS checkout's out\tools\admit\admit.exe selftest (lane cqa_admit; a cmd path under out/ in a lane would be mapped to the MAIN checkout).
# 0 load: trivial children only, a fake memory file in %TEMP%; about 3 min.   pwsh -NoProfile -File tools\admit\run_selftest.ps1
# S106 C5: -OnlyNew runs just the new cases (44 and up; 52 / 53 are the alone pause's); a missing exe is built first (tools\admit\build_admit.ps1, never through admit).
# S109 GATE: -OnlyGate runs the bounded wait / refusal cases (about 30 s); -Target EXE runs them against another exe (the planted twin: a pre-GATE exe fails
# every one); -NoReal skips the cases that allocate real memory or read the real PC (47, 49, the overhead case). The exe is (re)built here when its
# sha256 stamp is not this checkout's admit.cpp (build_admit.ps1 -IfStale).
param([switch]$OnlyNew, [switch]$OnlyGate, [switch]$NoReal, [string]$Target = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $repo 'out\tools\admit\admit.exe'
if (-not (Test-Path $exe)) { & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build_admit.ps1'); if ($LASTEXITCODE -ne 0) { throw 'admit build failed' } }
else { & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build_admit.ps1') -IfStale; if ($LASTEXITCODE -ne 0) { throw 'admit rebuild failed or was not admitted (see the [admit] line)' } }
if (-not (Test-Path $exe)) { throw "$exe missing: run tools\admit\build_admit.ps1 first" }
$a = @('selftest')
if ($OnlyGate) { $a += '--only-gate' } elseif ($OnlyNew) { $a += '--only-new' }
if ($NoReal) { $a += '--no-real' }
if ($Target) { $a += @('--target', $Target) }
& $exe @a
exit $LASTEXITCODE
