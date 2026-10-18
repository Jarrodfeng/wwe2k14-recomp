# Session 21: correctness pair for a setting. Plays one replay recording (tools\input_scripts\short\s21_hash.txt:
# both entrances + 2,000 match frames) with the setting off and on, side by side in shared lanes, with every frame
# from Brock's titantron to match +2,000 hashed in full (frame-locked clip, src/clip_capture.h), then compares the
# hashes per guest frame (tools\perf\hash_compare.py). Fast-forwarded unless -RealTime.
#   & .\tools\perf\s21_hash_pair.ps1 -Off '--x=false' -On '--x=true' [-Common '...'] [-AA] [-Name s21_lb] [-RealTime] [-Record]
# -Common: flags for every run. Rendering is only deterministic with --async_shader_compilation=false (runs 1499-1501:
#   ~1 % of frames differed between two runs with the same settings: draws skipped while their pipeline compiles).
# -AA: a third run, setting off again (off vs off must be identical for the pair to mean anything).
# -Record: first records the replay NAME (out\replays\NAME.rec, never overwritten) from the same route.
# In-process only (pwsh -File mangles the strings, Session 20).
param([string]$Off = '', [string]$On = '', [string]$Common = '', [string]$Name = 's21_lb', [switch]$AA,
      [switch]$RealTime, [switch]$Record)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\s21_hash.txt"
$clips = "$R\out\harness\step21\clips"
New-Item -ItemType Directory -Force $clips | Out-Null
$clip = "--wwe2k14_clip_dir=$clips --wwe2k14_clip_frame_locked=true --wwe2k14_clip_hash_log=true $Common".Trim()
$sw = @{ RealTime = [bool]$RealTime }   # hashtable splat (an empty array splat passes a positional $null: run 1463)
if ($Record) {
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($route) @sw -Extra "--wwe2k14_launch=lesnar_bryan --wwe2k14_replay_record=$Name $clip" -WaitSeconds 1500 2>&1
    $out | Where-Object { "$_" -match ' -> ' } | ForEach-Object { "record: $_" }
}
$offExtra = "--wwe2k14_replay_play=$Name $clip $Off".Trim()
$onExtra = "--wwe2k14_replay_play=$Name $clip $On".Trim()
$scripts = @($route, $route); $extras = @($offExtra, $onExtra)
if ($AA) { $scripts += $route; $extras += $offExtra }
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras @sw -Parallel $scripts.Count -WaitSeconds 1500 2>&1
$lines = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' })
$lines | ForEach-Object { "pair: $_" }
$runs = @($lines | ForEach-Object { [void]("$_" -match 'wwe2k14_(\d+)'); $Matches[1] })
if ($runs.Count -ne $scripts.Count) { "pair: expected $($scripts.Count) runs, got $($runs.Count)"; $out | Select-Object -Last 15; return }
# Parallel lines may come in finishing order: the "on" run is the one whose log has -On's first flag as set.
$first = ($On -split '\s+')[0]
if ($first -notmatch '^--([^=]+)=(.*)$') { "pair: -On must start with --name=value"; return }
$marker = "modified: $($Matches[1]) = '$($Matches[2])'"
$onRuns = @(); $offRuns = @()
foreach ($runNo in $runs) {
    if (Select-String -LiteralPath "$R\out\build\win-amd64-release\logs\wwe2k14_$runNo.log" -SimpleMatch $marker -Quiet) { $onRuns += $runNo } else { $offRuns += $runNo }   # not $r: names are case-insensitive ($R)
}
if ($onRuns.Count -ne 1) { "pair: cannot tell the arms apart by '$marker' (on runs: $($onRuns -join ','))"; return }
"off = run(s) $($offRuns -join ', ') ($Off), on = run $($onRuns[0]) ($On)"
if ($offRuns.Count -eq 2) {
    "--- off vs off (A/A)"
    py "$R\tools\perf\hash_compare.py" $offRuns[0] $offRuns[1] --dir $clips
}
"--- off vs on"
py "$R\tools\perf\hash_compare.py" $offRuns[0] $onRuns[0] --dir $clips
