# Session 38: does the native renderer leave the game's behaviour alone? Plays one replay recording twice, native
# renderer off then on (every native option on: the game's shaders, all scene draws, the native picture shown), with
# the determinism hashes of tools\replay\run_replay_check.ps1, and compares them (tools\determinism\det_compare.py).
#   & .\tools\native\determinism.ps1 [-Name replay_check] [-Mode 2]
# -Mode 2 (Session 39): the second run replaces the scene (gpu_native 2) instead of the side-car.
# -Extra (Session 42): more flags for the second run (e.g. '--gpu_native_post=true --gpu_native_after_scene=true').
# In-process only (call with & from PowerShell).
param([string]$Name = 'replay_check', [int]$Mode = 1, [string]$Extra = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$script = "$R\tools\replay\replay_check.txt"
$det = "--wwe2k14_replay_play=$Name --wwe2k14_det_hash=default,objects --wwe2k14_det_hash_raw_every=30 --resolution_scale=1"
$on = if ($Mode -eq 2) { '--gpu_native=2' } else { '--gpu_native=1 --gpu_native_shading=1 --gpu_native_draws=1 --gpu_native_view=1' }
if ($Extra) { $on = "$on $Extra" }
$runs = @()
foreach ($native in '--gpu_native=0', $on) {
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra "$det $native" -WaitSeconds 900 2>&1
    $line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
    "$native -> $line"
    if ("$line" -match 'wwe2k14_(\d+)') { $runs += [int]$Matches[1] }
}
if ($runs.Count -eq 2) {
    py "$R\tools\determinism\det_compare.py" $runs[0] $runs[1]
    "compare: out\harness\determinism\compare_$($runs[0])_$($runs[1]).txt"
}
