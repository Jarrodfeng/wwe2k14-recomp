# Session 15 (determinism focus): a same-seed pair (or N runs) on the fast route det_launch_fast.txt, at max speed by
# default, then det_compare.py on the first two. One game at a time (run_list, one hold).
#   pwsh -NoProfile -File tools\determinism\run_det_fast.ps1 [-Count 2] [-Seed 1234] [-Normal] [-More '--x=1 --y=2'] [-Trace]
# -Entrances: play the entrances (default: skipped). -Script: another route (default det_launch_fast.txt). -Trace: det_serial scheduler trace into out\harness\determinism\trace (det_serial_<pid>.txt)
# -Normal: keep vsync on (60 Hz) instead of max speed. -More: extra flags for every run (ONE string).
param([int]$Count = 2, [int]$Seed = 1234, [switch]$Normal, [string]$More = '', [int]$RawEvery = 30,
      [string]$Spec = 'default,objects', [switch]$Trace, [string]$Script = '', [switch]$Entrances)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$script = if ($Script) { (Resolve-Path $Script).Path } else { "$R\tools\determinism\det_launch_fast.txt" }
$extra = "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_skip_entrances=$(if ($Entrances) { 'false' } else { 'true' }) --wwe2k14_fixed_seed=$Seed " +
         "--wwe2k14_det_hash=$Spec --wwe2k14_det_hash_raw_every=$RawEvery"
if (-not $Normal) { $extra += ' --vsync=false --resolution_scale=1' }
if ($More) { $extra += " $More" }
if ($Trace) { New-Item -ItemType Directory -Force "$R\out\harness\determinism\trace" | Out-Null; $extra += " --det_serial_trace=$R\out\harness\determinism\trace" }
$scripts = @(); for ($i = 0; $i -lt $Count; $i++) { $scripts += $script }
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -Extra $extra -WaitSeconds 600 2>&1
$lines = @($out | Where-Object { "$_" -match '->' })
$lines | ForEach-Object { "$_" }
$runs = @($lines | ForEach-Object { if ("$_" -match 'wwe2k14_(\d+)') { [int]$Matches[1] } })
if ($runs.Count -ge 2) {
    py "$R\tools\determinism\det_compare.py" $runs[0] $runs[1] --slack 1 2>&1 | Out-Null
    "compare: out\harness\determinism\compare_$($runs[0])_$($runs[1])_slack1.txt"
}
