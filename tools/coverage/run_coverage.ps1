# Runs coverage routes on the coverage build (Session 8 task 6), then rebuilds the coverage tables.
#   pwsh -NoProfile -File tools\coverage\run_coverage.ps1                      all routes in tools\coverage\routes
#   pwsh -NoProfile -File tools\coverage\run_coverage.ps1 -Routes cov_logo,cov_caw
# Needs the coverage exe: configure once (tools\coverage\configure.ps1 under the run lock), then
#   pwsh -NoProfile -File tools\build.ps1 -Preset win-amd64-coverage -Run cov_NNN
# run_list.ps1 takes the run lock, mirrors the frozen user data before each run and kills a run after -WaitSeconds.
# Output: out\harness\coverage\<run>\ per run, then out\harness\coverage\coverage.json + summary.txt (cov.py build).
# Checks: make_routes.py must succeed (else the old route files would run) and every named route must exist (else
# the game would sit at the title until the kill). A run counts only if it EXITED with exit code 0, no failure lines,
# and its coverage folder has the script's quit window (NN_quit.txt); any other run's folder is renamed to
# <run>_failed, which cov.py does not read, and the script exits 1 after rebuilding the tables from the good runs.
param([string[]]$Routes = @(), [int]$WaitSeconds = 720, [string]$Extra = '')
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$exe = "$repo\out\build\win-amd64-coverage\wwe2k14.exe"
$covRoot = "$repo\out\harness\coverage"
if (-not (Test-Path $exe)) { "run_coverage: no coverage exe at $exe (configure + build the win-amd64-coverage preset first)"; exit 2 }
py "$repo\tools\coverage\make_routes.py"
if ($LASTEXITCODE -ne 0) { "run_coverage: make_routes.py failed (exit $LASTEXITCODE): nothing run"; exit 2 }
$names = if ($Routes.Count) { $Routes | ForEach-Object { $_ -split ',' } | Where-Object { $_ } } else {
    Get-ChildItem "$repo\tools\coverage\routes\cov_*.txt" | ForEach-Object BaseName }
$names = @($names | ForEach-Object { $_ -replace '\.txt$', '' })
$missing = @($names | Where-Object { -not (Test-Path "$repo\tools\coverage\routes\$_.txt") })
if ($missing.Count) { "run_coverage: no such route(s): $($missing -join ', ') (see tools\coverage\routes): nothing run"; exit 2 }
$scripts = @($names | ForEach-Object { "..\..\..\tools\coverage\routes\$_.txt" })
$lines = @(& "$repo\tools\bench\run_list.ps1" -Scripts $scripts -Exe $exe -WaitSeconds $WaitSeconds -Extra ("--wwe2k14_coverage=true $Extra").Trim())
$lines
$bad = 0
foreach ($l in $lines) {
    $text = "$l"
    if ($text -notmatch '^(\S+) -> wwe2k14_(\d+)') { continue }
    $route = $Matches[1]; $run = $Matches[2]
    $dir = Join-Path $covRoot $run
    $why = @()
    if ($text -notmatch 'EXITED after [\d.,]+s, exit code 0\b') { $why += 'did not exit cleanly' }
    if ($text -match 'script FAILED|\]\[crash\]|device lost|Failed to await') { $why += 'failure lines' }
    if (-not (Test-Path $dir)) { $why += 'no coverage folder' }
    elseif (-not (Get-ChildItem $dir -Filter '*_quit.txt' -ErrorAction SilentlyContinue)) { $why += 'no quit window (script did not reach quit)' }
    if ($why.Count) {
        $bad++
        if (Test-Path $dir) { Rename-Item $dir "${run}_failed" -ErrorAction SilentlyContinue }
        "run_coverage: run $run ($route) NOT used: $($why -join '; ')$(if (Test-Path "${dir}_failed") { " (folder renamed to ${run}_failed)" })"
    } else { "run_coverage: run $run ($route) ok" }
}
py "$repo\tools\coverage\cov.py" build
if ($bad) { exit 1 }
