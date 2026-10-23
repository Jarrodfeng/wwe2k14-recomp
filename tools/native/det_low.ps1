# Session 46: determinism LOW vs LOW (the kickoff's rule): the replay_check recording twice - LOW with the native
# renderer off (--gpu_native=0 --wwe2k14_preset_low=true), then LOW with -Extra (e.g. '--gpu_native_truth=true') - and
# tools\determinism\det_compare.py on the pair. Run it alone (no other game: fixed-seed routes go alone,
# docs\harness\lanes.md). In-process only (call with & from PowerShell).
#   & .\tools\native\det_low.ps1 -Extra '--gpu_native_truth=true'
param([string]$Name = 'replay_check', [string]$Extra = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$script = "$R\tools\replay\replay_check.txt"
$det = "--wwe2k14_replay_play=$Name --wwe2k14_det_hash=default,objects --wwe2k14_det_hash_raw_every=30 --resolution_scale=1 --wwe2k14_preset_low=true"
$runs = @()
foreach ($flags in '--gpu_native=0', $Extra) {
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra "$det $flags".Trim() -WaitSeconds 900 2>&1
    $line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
    "$flags -> $line"
    if ("$line" -match 'wwe2k14_(\d+)') { $runs += [int]$Matches[1] }
}
if ($runs.Count -eq 2) {
    py "$R\tools\determinism\det_compare.py" $runs[0] $runs[1] | Out-Null
    "compare: out\harness\determinism\compare_$($runs[0])_$($runs[1]).txt"
    Select-String -Path "$R\out\harness\determinism\compare_$($runs[0])_$($runs[1]).txt" -Pattern 'identical|differ' |
        ForEach-Object { $_.Line } | Select-Object -First 14
}
