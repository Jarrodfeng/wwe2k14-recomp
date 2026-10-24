# Session 57 bug 2: the launcher's logo import with a save made while the DLC packs were on (a copy of
# out\bench\userdata_unlocked, Session 32) and DLC on in the launcher's settings (test settings file
# out\s57\set_logo_dlc.json -> out\s57\userdata_logo_dlc). Before the fix the import's game run got no DLC flags and
# stopped at the game's ATTENTION "missing or damaged downloadable content" notice. Prints the launcher's result lines,
# the route's result file and the game log's DLC / logo_import lines. Run it through tools\runlock.ps1 run.
param([Parameter(Mandatory)][string]$Launcher, [string]$Tag = 'run', [switch]$NoDlc)
$Repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$R = "$Repo\out\s57"
$ud = "$R\userdata_logo_dlc"
New-Item -ItemType Directory -Force $R | Out-Null
Remove-Item $ud -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$Repo\out\bench\userdata_unlocked" $ud -Recurse
$settings = [ordered]@{ game_data_dir = "$Repo\game_data\extracted"; user_data_dir = $ud; first_run_done = $true;
                        check_updates = $false; dlc = (-not $NoDlc); dlc_dir = "$Repo\game_data\dlc" } | ConvertTo-Json
Set-Content "$R\set_logo_dlc.json" $settings
$png = "$R\logo_test_256.png"
if (-not (Test-Path $png)) { Copy-Item "$Repo\out\s53\logo_test_256.png" $png }
$log = "$R\launcher_logo_dlc_$Tag.log"
$logs = Join-Path (Split-Path $Launcher) 'logs'
$before = @(Get-ChildItem $logs -Filter 'wwe2k14_*.log' -ErrorAction SilentlyContinue | ForEach-Object Name)
$a = "--settings $R\set_logo_dlc.json --local-data $R\localdata --log $log --no-wizard --page mods --mods-tab logos --logo-size 256 --logo-add $png --wait 5 --logo-import --wait 20 --wait-game --wait 10 --shot $R\launcher_logo_dlc_$Tag.png --exit"
$t0 = Get-Date
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
if (-not $l.WaitForExit(600000)) { "launcher still running after 600 s: killed"; Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force }
"== $Tag ($([int]((Get-Date) - $t0).TotalSeconds) s)"
Get-Content $log | Select-String -NotMatch '^step |^launcher start|^menu font'
"-- route result: $(Get-Content "$R\localdata\logo_import_result.txt" -ErrorAction SilentlyContinue)"
$new = Get-ChildItem $logs -Filter 'wwe2k14_*.log' | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($new) {
  "-- game log $($new.Name)"
  Select-String -Path $new.FullName -Pattern '\[dlc\]|logo_import|input_result|DownloadableContent|XamContent' | Select-Object -First 40 | ForEach-Object { $_.Line }
}
