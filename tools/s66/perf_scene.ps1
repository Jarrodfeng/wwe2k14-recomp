# Session 66: per-scene performance of a LAUNCHED Lesnar vs Bryan run (not a replay: replays serialize the game's
# threads), NATIVE (--wwe2k14_preset_low=true), fast-forwarded, with the per-thread CPU sampler alongside.
# Purpose: fps, frames over 20 / 33 ms, GPU ms, command-thread busy ms and per-thread CPU ms per frame for Brock's
# entrance, Bryan's entrance and the match (-Taker adds Undertaker's WM XXX entrance), and, with -Flag, an alternating
# base / base+flag comparison with medians and the delta.
# Usage (call in-process so the flag string stays whole):
#   & .\tools\s66\perf_scene.ps1                                  # baseline, one run
#   & .\tools\s66\perf_scene.ps1 -Flag '--gpu_x=true' -Runs 3     # base,flag,base,flag,base,flag + medians + delta
#   & .\tools\s66\perf_scene.ps1 -Taker                           # also the Undertaker WM XXX run(s)
#   -Base '--y=1'   extra flags in BOTH arms.
# Routes: tools\input_scripts\short\profile_lesnar_bryan.txt with --wwe2k14_launch=lesnar_bryan --vsync=false
#   --vulkan_gpu_timestamps=true --wwe2k14_file_trace=true; Taker: short\bench_launch_taker_wm.txt with
#   --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=343,103.
# Every run passes --wwe2k14_settings_file=out\s66\settings_test.json (never Jarrod's launcher settings).
# Outputs: the tables on the console and out\s66\perf\perf_<stamp>.txt; the runs' own logs / CSVs as usual under
#   out\build\win-amd64-release\logs (wwe2k14_NNN.log, frames_, timeline_, threads_NNN.csv). Numbers come from
#   perf_scene.py (reuses bench_report.py scene ranges, GPU ms and command-thread busy).
# Notes: fast-forwarded, so fps is the PC's speed (compare arms, do not compare against 60). The thread sampler is
#   1 Hz and picks the first wwe2k14.exe it sees: the script waits for other games to end first (up to 15 min) and
#   warns if one starts mid-run. Separate runs share the 4090's clock-state noise: a verdict needs the alternation
#   and the medians (-Runs 3 or more); in-run A/Bs (ab_gpu.py) decide GPU effects.
param([string]$Flag = '', [int]$Runs = 1, [switch]$Taker, [string]$Base = '', [int]$WaitSeconds = 900)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$runList = "$R\tools\bench\run_list.ps1"
$short = "$R\tools\input_scripts\short"
$settings = "--wwe2k14_settings_file=$R\out\s66\settings_test.json"
$common = "--wwe2k14_preset_low=true --vsync=false --vulkan_gpu_timestamps=true --wwe2k14_file_trace=true $settings"
New-Item -ItemType Directory -Force "$R\out\s66\perf" | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outFile = "$R\out\s66\perf\perf_$stamp.txt"
function Say([string]$s) { $s; Add-Content $outFile $s }

function Invoke-One([string]$route, [string]$extra) {
    $t0 = Get-Date
    while ((Get-Process wwe2k14 -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalMinutes -lt 15) { Start-Sleep -Seconds 5 }
    $job = Start-Job { & pwsh -NoProfile -File 'C:\Users\Jarrod\wwe2k14-recomp\tools\perf\thread_busy.ps1' -WaitForStart 900 }
    $out = & $runList -Scripts @($route) -Extra $extra -WaitSeconds $WaitSeconds 2>&1
    Wait-Job $job -Timeout 60 | Out-Null
    Remove-Job $job -Force
    $n = $null
    foreach ($l in $out) { if ("$l" -match '\S+\.txt -> wwe2k14_(\d+)') { $n = [int]$Matches[1] } }
    if (-not $n) { $out | Select-Object -Last 12 | ForEach-Object { "  run_list: $_" } }
    elseif (-not (Test-Path "$R\out\build\win-amd64-release\logs\threads_$n.csv")) { Write-Host "  warning: no threads_$n.csv (sampler found no game)" }
    $n
}

function Invoke-Arms([string]$route, [string]$extra, [string]$title) {
    $baseRuns = @(); $flagRuns = @()
    for ($i = 1; $i -le $Runs; $i++) {
        Say "[$title] run $i of $Runs, base"
        $n = Invoke-One $route "$extra $Base".Trim(); if ($n) { $baseRuns += $n; Say "  -> run $n" }
        if ($Flag) {
            Say "[$title] run $i of $Runs, base + $Flag"
            $n = Invoke-One $route "$extra $Base $Flag".Trim(); if ($n) { $flagRuns += $n; Say "  -> run $n" }
        }
    }
    if (-not $baseRuns) { Say "[$title] no run succeeded"; return }
    $pyArgs = @("$R\tools\s66\perf_scene.py", '--base', ($baseRuns -join ','))
    if ($flagRuns) { $pyArgs += @('--flag', ($flagRuns -join ','), '--label-flag', $Flag) }
    Say "======== $title ========"
    py @pyArgs 2>&1 | ForEach-Object { Say "$_" }
}

$t = Get-Date
Say "perf_scene $stamp  base flags: $common"
if ($Flag) { Say "item flag: $Flag, $Runs alternating run(s) per arm" }
$lb = $common -replace '^', '--wwe2k14_launch=lesnar_bryan '
Invoke-Arms "$short\profile_lesnar_bryan.txt" $lb 'Lesnar vs Bryan'
if ($Taker) {
    Invoke-Arms "$short\bench_launch_taker_wm.txt" "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=343,103 $common" 'Undertaker WM XXX'
}
Say ("total {0:N1} min; saved {1}" -f ((Get-Date) - $t).TotalMinutes, $outFile)
