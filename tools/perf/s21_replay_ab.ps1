# Session 21: confirmation A/B. Plays one replay recording (tools\input_scripts\short\s21_replay_time.txt) -Runs times
# per arm, alternating off / on, exclusive (timed), with GPU timestamps; fast-forwarded (S109 FF: normal speed only when the route says `# intent: performance <what>`). Then
# bench_report per run (per-CP-frame CSVs) and tools\perf\replay_ab.py (GPU ms per scene on matching guest frames).
#   & .\tools\perf\s21_replay_ab.ps1 -Tag T -Off '--x=false' -On '--x=true' [-Name s21_lb] [-Runs 3]
# In-process only (pwsh -File mangles the strings, Session 20).
param([Parameter(Mandatory)][string]$Tag, [string]$Off = '', [string]$On = '', [string]$Name = 's21_lb',
      [int]$Runs = 3)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\s21_replay_time.txt"
$log = "$R\out\harness\step21\replay_ab_$Tag.txt"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
$sw = @{ Exclusive = $true }
$offRuns = @(); $onRuns = @()
for ($i = 1; $i -le $Runs; $i++) {
    foreach ($arm in 'off', 'on') {
        $flags = if ($arm -eq 'off') { $Off } else { $On }
        $extra = "--wwe2k14_replay_play=$Name --vulkan_gpu_timestamps=true $flags".Trim()
        $out = & "$R\tools\bench\run_list.ps1" -Scripts @($route) @sw -Extra $extra -WaitSeconds 1200 2>&1
        $line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
        "$(Get-Date -Format 'HH:mm:ss') $Tag round $i $arm -> $line" | Tee-Object -FilePath $log -Append
        if ("$line" -match 'wwe2k14_(\d+)') {
            if ($arm -eq 'off') { $offRuns += $Matches[1] } else { $onRuns += $Matches[1] }
        }
    }
}
foreach ($run in $offRuns + $onRuns) { py "$R\tools\bench\bench_report.py" report $run *> $null }
py "$R\tools\perf\replay_ab.py" --off ($offRuns -join ',') --on ($onRuns -join ',') | Tee-Object -FilePath $log -Append
