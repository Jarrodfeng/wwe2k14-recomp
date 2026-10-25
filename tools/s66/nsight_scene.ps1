# Session 66: one scene of the per-scene "iGPU cost card" (docs\native\S66_HARNESS.md section 4, item 6.3).
# Runs (1) a cheap companion run of the scene route with the file trace and --gpu_record_stats=30 (no Nsight: scene
# start frames + the census), then (2) an exclusive Nsight Graphics GPU Trace of Frames frames at the scene's start +
# offset, then moves the trace to out\s66\cards\<Tag>_<scene>\ (s52_nsight_trace overwrites its BASE folder).
# In-process only (call with & from PowerShell, so -Extra stays one string):
#   & .\tools\s66\nsight_scene.ps1 -Scene brock|bryan|match|taker -Tag s65_base [-Extra '--gpu_x=true'] [-Frames 3] [-Offset N] [-Run NNN]
# Scenes (replay scenes: --wwe2k14_replay_play=s21_lb --wwe2k14_preset_low=true; start = first open of the marker file
# in the companion log, 'frame=N', plus the offset, deterministic in a replay):
#   brock 343.bik2 +200   bryan 324.bik2 +200   match M3.PAC +600
#   taker (launched, NOT a replay: --wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=343,103
#          --wwe2k14_preset_low=true, route tools\s66\cost_scene_taker.txt) 103.bik2 +300
# taker: DOES NOT WORK. The companion run (fast-forwarded) reaches 103.bik2 at frame 7630, but the Nsight run
#   (launched, not deterministic, not fast-forwarded) ended before start frame 7930, so ngfx (--no-timeout in
#   s52_nsight_trace.ps1) waited forever with no game running (2026-10-25 22:55, killed by hand). Not retried.
# Wall clock: -TimeoutSeconds (default 360) makes s52_nsight_trace kill the game and ngfx; the finally block kills both again.
# -Run NNN: reuse a companion run (skip step 1). Every run passes --wwe2k14_settings_file=out\s66\settings_test.json;
# -Extra goes to BOTH runs (an item's flags). Outputs: out\s66\cards\<Tag>_<scene>\ with BASE\ (the Nsight export),
# meta.json (scene, start frame, companion run) and ngfx_stdout.txt. Then: py tools\s66\cost_card.py out\s66\cards\<Tag>_<scene>
param([Parameter(Mandatory)] [ValidateSet('brock', 'bryan', 'match', 'taker')] [string]$Scene, [Parameter(Mandatory)] [string]$Tag,
      [string]$Extra = '', [int]$Frames = 3, [int]$Offset = -1, [int]$Run = 0, [int]$TimeoutSeconds = 360)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$logs = "$R\out\build\win-amd64-release\logs"
$nsDir = "$R\out\harness\step20\nsight"
$settings = "--wwe2k14_settings_file=$R\out\s66\settings_test.json"
$def = @{
    brock = @{ marker = '343.bik2'; off = 200; replay = $true }
    bryan = @{ marker = '324.bik2'; off = 200; replay = $true }
    match = @{ marker = 'M3.PAC'; off = 600; replay = $true }
    taker = @{ marker = '103.bik2'; off = 300; replay = $false }
}[$Scene]
if ($Offset -lt 0) { $Offset = $def.off }
$common = if ($def.replay) { "--wwe2k14_replay_play=s21_lb --wwe2k14_preset_low=true" }
          else { "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=343,103 --wwe2k14_preset_low=true" }
$route = if ($def.replay) { 'tools\s66\cost_scene_replay.txt' } else { 'tools\s66\cost_scene_taker.txt' }
$card = "$R\out\s66\cards\${Tag}_$Scene"
$t0 = Get-Date

# 1. companion run
if ($Run -le 0) {
    $o = & "$R\tools\bench\run_list.ps1" -Scripts @($route) -Extra "$common --wwe2k14_file_trace=true --gpu_record_stats=30 $settings $Extra".Trim() -WaitSeconds 1200 2>&1
    $o | Select-Object -Last 3 | ForEach-Object { "$_" }
    $line = @($o | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
    if ("$line" -notmatch 'wwe2k14_(\d+)') { throw 'companion run: no run number' }
    $Run = [int]$Matches[1]
}
$log = "$logs\wwe2k14_$Run.log"
$marker = [regex]::Escape($def.marker)
$hit = Select-String -Path $log -Pattern "NtCreateFile '[^']*$marker' open .* frame=(\d+)" -CaseSensitive:$false | Select-Object -First 1
if (-not $hit) { throw "no open of $($def.marker) in $log" }
$start = [int]$hit.Matches[0].Groups[1].Value + $Offset
"scene ${Scene}: $($def.marker) opened at frame $($hit.Matches[0].Groups[1].Value), trace from frame $start (+$Frames), companion run $Run"

# 2. Nsight trace under the exclusive lock (in-process, so -Extra stays whole)
. "$R\tools\runlock.ps1"
$h = Enter-RunLock -Name "s66-nsight-$Tag-$Scene"
$tLock = Get-Date
try {
    Remove-Item "$nsDir\BASE" -Recurse -Force -ErrorAction SilentlyContinue
    & "$R\tools\perf\s52_nsight_trace.ps1" -Extra "$common $settings $Extra".Trim() -StartAfterFrames $start -Frames $Frames `
        -Route $route -Launch lesnar_bryan -TimeoutSeconds $TimeoutSeconds
} finally {
    # never leave ngfx behind holding the game; only processes this trace started (a game Jarrod opened earlier stays)
    Get-Process ngfx, wwe2k14 -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $tLock } | Stop-Process -Force
    Exit-RunLock $h
}

# 3. move the output
if (-not (Test-Path "$nsDir\BASE\GPUTRACE_FRAME.xls")) { throw "no BASE\GPUTRACE_FRAME.xls under $nsDir (trace failed; see ngfx_stdout.txt)" }
Remove-Item $card -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $card | Out-Null
Move-Item "$nsDir\BASE" "$card\BASE"
Copy-Item "$nsDir\ngfx_stdout.txt" $card -ErrorAction SilentlyContinue
Get-ChildItem $nsDir -Filter *.ngfx-gputrace | Where-Object { $_.LastWriteTime -ge $t0 } | Move-Item -Destination $card
[ordered]@{ scene = $Scene; tag = $Tag; marker = $def.marker; offset = $Offset; start = $start; frames = $Frames; run = $Run;
            flags = "$common $Extra".Trim(); date = (Get-Date -Format 'yyyy-MM-dd HH:mm') } | ConvertTo-Json | Set-Content "$card\meta.json"
"card: $card"
& py "$R\tools\s66\cost_card.py" $card
