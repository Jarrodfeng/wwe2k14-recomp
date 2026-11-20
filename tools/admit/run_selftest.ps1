# Runs THIS checkout's out\tools\admit\admit.exe selftest (lane cqa_admit; a cmd path under out/ in a lane would be mapped to the MAIN checkout).
# 0 load: trivial children only, a fake memory file in %TEMP%; about 15 s.   pwsh -NoProfile -File tools\admit\run_selftest.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $repo 'out\tools\admit\admit.exe'
if (-not (Test-Path $exe)) { throw "$exe missing: run tools\admit\build_admit.ps1 first" }
& $exe selftest
exit $LASTEXITCODE
