# Session 38: the native renderer's comparison loop. Plays the s21_lb replay (Brock's and Bryan's entrances, then the
# match; route short\s21_replay_time.txt, which logs scene=tron_343 / tron_324 / match) with the native renderer beside
# the emulated one, then prints parity and cost per scene (tools\native\compare.py) into out\native\compare_RUN.txt.
#   & .\tools\native\compare.ps1 [-Name s21_lb] [-Shading 1] [-Draws 1] [-Scale 1] [-More '--x=1'] [-RealTime]
# Untimed by default (fast-forwarded, lanes OK): the parity is per frame and does not depend on speed; the GPU times
# are the GPU's own and are comparable between runs at the same speed. -RealTime for timings at normal speed.
# In-process only (call with & from PowerShell; pwsh -File splits -More).
# Session 39: -Mode 2 = replace (gpu_native 2), with -VerifyEvery N side-car frames for the parity numbers.
param([string]$Name = 's21_lb', [int]$Shading = 1, [int]$Draws = 1, [double]$Scale = 1.0, [string]$More = '',
      [switch]$RealTime, [int]$Mode = 1, [int]$VerifyEvery = 0)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\s21_replay_time.txt"
$extra = "--wwe2k14_replay_play=$Name --gpu_native=$Mode --gpu_native_verify_every=$VerifyEvery " +
         "--gpu_native_shading=$Shading --gpu_native_draws=$Draws " +
         "--gpu_native_scale=$Scale --gpu_native_report_frames=30 $More"
$sw = @{ RealTime = [bool]$RealTime }
$out = & "$R\tools\bench\run_list.ps1" -Scripts @($route) @sw -Extra $extra.Trim() -WaitSeconds 1500 2>&1
$line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
"$line"
if ("$line" -match 'wwe2k14_(\d+)') {
    $run = $Matches[1]
    py "$R\tools\native\compare.py" $run --out "$R\out\native\compare_$run.txt"
}
