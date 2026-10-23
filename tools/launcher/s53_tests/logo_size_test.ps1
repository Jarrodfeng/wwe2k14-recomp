# Session 53 item 2.5: the launcher's logo import at 128x128 and at 256x256 into a copy of the frozen save (test
# settings file out\s53\set_logo.json -> out\s53\userdata_logo), then the launcher's own result lines.
param([string]$Launcher)
$R = 'C:\Users\Jarrod\wwe2k14-recomp\out\s53'
$frozen = 'C:\Users\Jarrod\wwe2k14-recomp\out\bench\userdata_frozen'
Remove-Item "$R\userdata_logo" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $frozen "$R\userdata_logo" -Recurse
$settings = @{ game_data_dir = 'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted'; user_data_dir = "$R\userdata_logo";
               first_run_done = $true; check_updates = $false } | ConvertTo-Json
Set-Content "$R\set_logo.json" $settings
foreach ($size in 128, 256) {
  $png = "$R\logo_test_$size.png"
  $log = "$R\launcher_logo_$size.log"
  $a = "--settings $R\set_logo.json --local-data $R\localdata --log $log --no-wizard --page mods --mods-tab logos --logo-size $size --logo-add $png --wait 5 --logo-import --wait 20 --wait-game --wait 10 --shot $R\launcher_logo_$size.png --exit"
  $t0 = Get-Date
  $l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
  if (-not $l.WaitForExit(600000)) { "launcher still running after 600 s: killed"; Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force }
  "== $size ($([int]((Get-Date) - $t0).TotalSeconds) s)"
  Get-Content $log | Select-String -NotMatch '^step |^launcher start|^menu font'
}
