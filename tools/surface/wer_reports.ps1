# S102 TS1: standalone read-only listing of the Surface's GPU-watchdog reports (see wer_lib.ps1). Reads the Surface; writes only the staging folder there.
#   pwsh -NoProfile -File tools\surface\wer_reports.ps1 -Out out\s102\wer_test [-SinceMinutes 60] [-Stage C:\wwe\out\ts1_test\stage]
param([Parameter(Mandatory)][string]$Out, [int]$SinceMinutes = 60, [string]$Stage = 'C:\wwe\out\wer_stage', [string]$HostName = 'surface')
. (Join-Path $PSScriptRoot 'wer_lib.ps1')
$lg = { param($m) Write-Host "$(Get-Date -Format 'HH:mm:ss') $m" }
New-Item -ItemType Directory -Force $Out | Out-Null
Save-JobWer 'wer_test' (Get-Date).AddMinutes(-$SinceMinutes) $Out $lg $Stage $HostName
