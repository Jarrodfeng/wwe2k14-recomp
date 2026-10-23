# Session 39: GPU time per frame with the native renderer replacing the scene (gpu_native 2) against the emulated
# renderer alone (gpu_native 0), on the s21_lb replay (Brock's and Bryan's entrances, the match; route
# short\s21_replay_time.txt). Exclusive (T3: nothing else on the GPU), fast-forwarded so the GPU runs flat out; GPU
# time per CP frame from the submissions' timestamps (--vulkan_gpu_timestamps, the timeline), per scene by
# tools\bench\bench_report.py. Announce to Jarrod before running it.
#   & .\tools\native\ab.ps1 [-Name s21_lb] [-More '--x=1'] [-Modes 0,2]
# In-process only (call with & from PowerShell).
param([string]$Name = 's21_lb', [string]$More = '', [int[]]$Modes = @(0, 2))
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\s21_replay_time.txt"
$runs = @()
foreach ($mode in $Modes) {
    $extra = "--wwe2k14_replay_play=$Name --vsync=false --vulkan_gpu_timestamps=true --gpu_native=$mode " +
             "--gpu_native_report_frames=60 $More"
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($route) -Exclusive -Extra $extra.Trim() -WaitSeconds 1500 2>&1
    $line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
    "gpu_native=$mode -> $line"
    if ("$line" -match 'wwe2k14_(\d+)') { $runs += $Matches[1] }
}
if ($runs.Count -eq $Modes.Count) {
    py "$R\tools\bench\bench_report.py" report @runs
}
