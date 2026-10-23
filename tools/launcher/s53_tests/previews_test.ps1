# Session 53 item 2.2: the launcher's Build previews in a release build outside the repo (test settings + local data).
param([string]$Launcher, [int]$MaxSeconds = 400)
$R = 'C:\Users\Jarrod\wwe2k14-recomp\out\s53'
Remove-Item "$R\localdata\trons" -Recurse -Force -ErrorAction SilentlyContinue
$a = "--settings $R\set_data.json --local-data $R\localdata --log $R\launcher_f.log --no-wizard --build-previews --wait 5 --exit"
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru -WorkingDirectory (Split-Path $Launcher)
$g = $null
for ($i = 0; $i -lt 60 -and -not $g; $i++) { Start-Sleep -Milliseconds 500; $g = Get-Process wwe2k14 -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $g) { "no game started"; exit 1 }
"game started: $((Get-CimInstance Win32_Process -Filter "ProcessId=$($g.Id)").CommandLine)"
$t0 = Get-Date
while (-not $g.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $MaxSeconds) { Start-Sleep -Seconds 2 }
if (-not $g.HasExited) { "game still running after $MaxSeconds s: killed"; Stop-Process -Id $g.Id -Force }
"game closed after $([int]((Get-Date) - $t0).TotalSeconds) s"
$mp4 = @(Get-ChildItem "$R\localdata\trons\*.mp4" -ErrorAction SilentlyContinue)
"previews: $($mp4.Count) mp4 files, $([int](($mp4 | Measure-Object Length -Sum).Sum / 1MB)) MB"
Get-Content "$R\launcher_f.log"
