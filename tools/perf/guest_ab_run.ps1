# Session 95 guest codegen A/B: the capped match route (tools\input_scripts\short\s95_gcpu_match.txt) once per exe, with the
# per-thread CPU sampler (thread_busy.ps1) beside it. Arms: folder names under -Root (<arm>\exe\wwe2k14.exe each), run in the
# order given: give A B B A (the second window of a pair reads high: S95 natives, run 006). An arm written name+flagset takes
# -ArmExtra's flags for that flagset on top (e.g. flat+regsave runs flat's exe with the regsave flags). Prints each run's log
# number; read with tools\perf\guest_ab_report.py.
#   & .\tools\perf\guest_ab_run.ps1 -Root out\wt\s95-mem\v -Arms base,flat,flat,base -Extra '--x=1' -Exclusive -ArmExtra @{ regsave = '--wwe2k14_nf_reg_save=true' }
param([string]$Arms, [string]$Extra = '', [switch]$Exclusive, [string]$Route = '', [string]$Root = 'out\s95\cg', [hashtable]$ArmExtra = @{},
      [string]$OutDir = 'out\s95\gcpu')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = if ($Route) { Join-Path $R $Route } else { "$PSScriptRoot\..\input_scripts\short\s95_gcpu_match.txt" }
$route = [IO.Path]::GetFullPath($route)
$null = New-Item -ItemType Directory -Force -Path (Join-Path $R $OutDir)
foreach ($spec in ($Arms -split ',')) {
    $arm, $set = $spec -split '\+', 2
    $dir = "$R\$Root\$arm\exe"
    $csv = "$R\$OutDir\ab_threads_$($spec -replace '\+','_')_$(Get-Date -Format HHmmss).csv"
    $job = Start-Job { param($t, $o, $e) & pwsh -NoProfile -File $t -Out $o -WaitForStart 1800 -ExePath $e } -ArgumentList "$PSScriptRoot\thread_busy.ps1", $csv, "$dir\wwe2k14.exe"
    $flags = "$Extra $(if ($set) { $ArmExtra[$set] })".Trim()
    $args2 = @{ Scripts = @($route); Exe = "$dir\wwe2k14.exe"; RealTime = $true; NoTriage = $true; WaitSeconds = 420 }
    if ($Exclusive) { $args2.Exclusive = $true }
    if ($flags) { $args2.Extra = $flags }
    $o = & "$R\tools\bench\run_list.ps1" @args2 2>&1 | Where-Object { "$_" -match '->' }
    $null = Wait-Job $job -Timeout 30; Remove-Job $job -Force
    if (-not (Test-Path $csv)) { Write-Warning "sampler wrote no CSV: $csv" }
    "ARM $spec | $csv | $o"
}
