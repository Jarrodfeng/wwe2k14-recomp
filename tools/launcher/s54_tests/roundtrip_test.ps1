# Session 54 (a ROG Ally tester: "whatever set from launcher doesn't get saved to game setting graphics or the other
# way around"): the launcher starts the game with 540p (NATIVE), the game's GRAPHICS menu moves RESOLUTION one step
# (short\s54_gfx_res_change.txt: 540P -> 480P) and saves, the game quits, the launcher takes the change over and
# saves on its own exit. Passes when the settings file then says draw_height 480. A test settings file and a copy of
# the frozen user data under out\s54 only (never the real save or launcher settings). Run under the run lock:
#   pwsh -File tools\runlock.ps1 run -Name s54-roundtrip -- pwsh -File tools\launcher\s54_tests\roundtrip_test.ps1
param([string]$Launcher = "$PSScriptRoot\..\..\..\out\build\win-amd64-release\wwe2k14_launcher.exe")
$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$R = Join-Path $Root 'out\s54'
$data = "$R\userdata_rt"
robocopy "$Root\out\bench\userdata_frozen" $data /MIR /NFL /NDL /NJH /NJS | Out-Null
New-Item -ItemType Directory -Force "$R\localdata" | Out-Null
$set = "$R\roundtrip.json"
$route = Join-Path $Root 'tools\input_scripts\short\s54_gfx_res_change.txt'
@{ first_run_done = $true; settings_version = 3; renderer = 2; resolution = 0; draw_height = 540; upscale = 0;
   frame_rate = 1; user_data_dir = $data; extra = "--wwe2k14_input_script=$route" } | ConvertTo-Json |
    Set-Content -Encoding utf8NoBOM $set
$log = "$R\launcher_roundtrip.log"
Remove-Item $log -ErrorAction SilentlyContinue
$a = "--settings $set --local-data $R\localdata --log $log --no-wizard --launch --wait-game --exit"
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
if (-not $l.WaitForExit(300000)) { $l.Kill(); "launcher did not quit (killed)" } else { "launcher exit $($l.ExitCode)" }
$after = Get-Content $set -Raw
foreach ($k in 'resolution', 'draw_height') { if ($after -match "`"$k`":\s*(\d+)") { "$k = $($Matches[1])" } }
if ($after -match '"draw_height":\s*480') { 'ROUNDTRIP PASS' } else { 'ROUNDTRIP FAIL' }
