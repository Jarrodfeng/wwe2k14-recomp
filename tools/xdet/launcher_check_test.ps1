# Session 77 (XDET_PLAN.md "P7 testers", step 3): the PC launcher's Determinism check end to end, as a tester would
# run it (ABOUT -> Determinism check), on a test settings file and a test local-data folder (never Jarrod's own
# launcher settings, save or local data). The check files are copied next to the exe as a release ships them (xdet\).
# Then the check's zip is compared with a PC reference play of the same recording (run_stageA.ps1 -Play xdet_check
# -FlagsFile check.flags -Data userdata_empty -Route tools\xdet\check_route.txt).
#   pwsh -NoProfile -File tools\runlock.ps1 run -Name launcher_check -- pwsh -NoProfile -File tools\xdet\launcher_check_test.ps1 -Reference 4963
param([int]$Reference = 0, [string]$Tag = 'run')
$Repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$Build = "$Repo\out\build\win-amd64-release"
$Launcher = "$Build\wwe2k14_launcher.exe"
$R = "$Repo\out\s77_check"
New-Item -ItemType Directory -Force "$R", "$Build\xdet" | Out-Null
Copy-Item "$Repo\tools\xdet\refs\xdet_check.rec" "$Build\xdet\xdet_check.rec" -Force
Copy-Item "$Repo\tools\xdet\check_route.txt" "$Build\xdet\check_route.txt" -Force
$settings = [ordered]@{ game_data_dir = "$Repo\game_data\iso\wwe2k14_test.iso"; user_data_dir = "$R\player_save";
                        first_run_done = $true; check_updates = $false } | ConvertTo-Json
Set-Content "$R\settings.json" $settings
$log = "$R\launcher_$Tag.log"
$before = @(Get-ChildItem "$Build\reports" -Filter 'determinism_check_*.zip' -ErrorAction SilentlyContinue | ForEach-Object FullName)
$a = "--settings $R\settings.json --local-data $R\localdata --log $log --no-wizard --no-open --page about --wait 5 " +
     "--xdet-check --wait 10 --shot $R\launcher_done_$Tag.bmp --exit"
$t0 = Get-Date
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
if (-not $l.WaitForExit(900000)) { "launcher still running after 900 s: killed"; Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force }
"== $Tag ($([int]((Get-Date) - $t0).TotalSeconds) s)"
Get-Content $log -ErrorAction SilentlyContinue | Select-String -NotMatch '^step |^launcher start|^menu font'
$zip = Get-ChildItem "$Build\reports" -Filter 'determinism_check_*.zip' -ErrorAction SilentlyContinue |
       Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $zip) { 'no new determinism_check zip'; exit 1 }
"-- zip $($zip.FullName) ($([int]($zip.Length / 1KB)) KB)"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::OpenRead($zip.FullName).Entries | ForEach-Object { "   $($_.FullName) $([int]($_.Length / 1KB)) KB" }
"-- left in localdata\xdet_check: $((Get-ChildItem "$R\localdata\xdet_check" -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')"
if ($Reference) {
  py "$Repo\tools\determinism\det_compare.py" --cross $Reference $zip.FullName 2>&1 |
    Where-Object { "$_" -match '^(VERDICT|frames:|[AB] identity|  sched trace|  (SCHED|EVENTS|SIM|LAYOUT|AUDIO) |events:|INVALID reasons|  frame|host-only)' } |
    ForEach-Object { "$_".Substring(0, [Math]::Min(300, "$_".Length)) }
}
