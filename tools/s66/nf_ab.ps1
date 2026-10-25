# Session 66: run an in-run CPU A/B route (tools\s66\nf_ab.py route ...) with the per-thread CPU sampler alongside, then
# print the report. In-process: & .\tools\s66\nf_ab.ps1 -Route tools\input_scripts\short\s66_nfab_NAME.txt
# The run needs the PC to itself (no other game, no CPU-heavy agent): the sampler picks the first wwe2k14.exe.
param([Parameter(Mandatory)] [string]$Route, [string]$Extra = '', [int]$WaitSeconds = 900)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$t0 = Get-Date
while ((Get-Process wwe2k14 -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalMinutes -lt 15) { Start-Sleep -Seconds 5 }
$job = Start-Job { & pwsh -NoProfile -File 'C:\Users\Jarrod\wwe2k14-recomp\tools\perf\thread_busy.ps1' -WaitForStart 900 }
$common = "--wwe2k14_launch=lesnar_bryan --wwe2k14_preset_low=true --vsync=false --wwe2k14_file_trace=true --wwe2k14_settings_file=$R\out\s66\settings_test.json $Extra".Trim()
$out = & "$R\tools\bench\run_list.ps1" -Scripts @((Resolve-Path $Route).Path) -Extra $common -WaitSeconds $WaitSeconds 2>&1
Wait-Job $job -Timeout 60 | Out-Null
Remove-Job $job -Force
$n = $null
foreach ($l in $out) { if ("$l" -match '\S+\.txt -> wwe2k14_(\d+)') { $n = [int]$Matches[1] } }
if (-not $n) { $out | Select-Object -Last 10; exit 3 }
"run $n"
py "$R\tools\s66\nf_ab.py" report $n
