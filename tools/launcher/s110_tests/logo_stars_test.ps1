# S110 HELPERSTART (review): the launcher's logo import with USE MY SUPERSTARS on and an UNMIGRATED test mods folder (an old pack6\, no
# frame\). The import must build the pack first (the YOUR SUPERSTARS window), start by itself when the build is done, and the game must
# take the pack in (the mask-0x7C save needs pack 6: without it the route stops at the missing-content notice, stage "start").
# Test copies only: out\bench\userdata_mask7c_s105 -> out\bench\userdata_s110_logoimport (the save), out\s105\mods ->
# out\s110\helperstart\mods_old (the mods), the settings file out\s110\helperstart\settings.json. Nothing of Jarrod's is read or written.
# It OPENS the launcher window and the game: only when the screen is free. Run it through tools\runlock.ps1 run.
# Twin: -Launcher of a build from before HELPERSTART: no build line, and the route result is "failed ... logo_import: start".
param([Parameter(Mandatory)][string]$Launcher, [string]$Tag = 'run')
$Repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$R = "$Repo\out\s110\helperstart"
$ud = "$Repo\out\bench\userdata_s110_logoimport"
$mods = "$R\mods_old"
New-Item -ItemType Directory -Force $R | Out-Null
robocopy "$Repo\out\bench\userdata_mask7c_s105" $ud /MIR /NFL /NDL /NJH /NJS | Out-Null
robocopy "$Repo\out\s105\mods" $mods /MIR /NFL /NDL /NJH /NJS | Out-Null
if (Test-Path "$mods\superstars\frame") { "logo_stars_test: STOP: $mods\superstars\frame exists: the copy is not unmigrated"; exit 2 }
if (-not (Test-Path "$mods\superstars\pack6")) { "logo_stars_test: STOP: $mods\superstars\pack6 missing: not the old-pack state this test needs"; exit 2 }
$settings = [ordered]@{ game_data_dir = "$Repo\game_data\extracted"; user_data_dir = $ud; first_run_done = $true; check_updates = $false;
                        dlc = $true; dlc_dir = "$Repo\game_data\dlc"; superstars = $true; mods_dir = $mods; precompile = $false } | ConvertTo-Json
Set-Content "$R\settings.json" $settings
$png = "$R\logo_test_256.png"
if (-not (Test-Path $png)) {
  Add-Type -AssemblyName System.Drawing
  $b = [System.Drawing.Bitmap]::new(256, 256)
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.Clear([System.Drawing.Color]::FromArgb(255, 200, 30, 30))
  $g.Dispose()
  $b.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
  $b.Dispose()
}
$log = "$R\launcher_$Tag.log"
Remove-Item $log, "$R\local\logo_import_result.txt" -ErrorAction SilentlyContinue
$logs = Join-Path (Split-Path $Launcher) 'logs'
$before = @(Get-ChildItem $logs -Filter 'wwe2k14_*.log' -ErrorAction SilentlyContinue | ForEach-Object Name)
$a = "--settings $R\settings.json --local-data $R\local --log $log --no-wizard --no-save --page mods --mods-tab logos --logo-size 256 " +
     "--logo-add $png --wait 5 --logo-import --wait-stars --wait 20 --wait-game --wait 10 --exit"
$t0 = Get-Date
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
if (-not $l.WaitForExit(900000)) { "launcher still running after 900 s: killed"; Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force }
"== $Tag ($([int]((Get-Date) - $t0).TotalSeconds) s)"
Get-Content $log | Select-String -NotMatch '^step |^launcher start|^menu font'
"-- route result: $(Get-Content "$R\local\logo_import_result.txt" -ErrorAction SilentlyContinue)"
"-- frame built: $(Test-Path "$mods\superstars\frame\info\catalog.dlc")"
$new = Get-ChildItem $logs -Filter 'wwe2k14_*.log' | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($new) {
  "-- game log $($new.Name)"
  Select-String -Path $new.FullName -Pattern '\[dlc\]|\[superstars\]|logo_import|input_result' | Select-Object -First 40 | ForEach-Object { $_.Line }
}
