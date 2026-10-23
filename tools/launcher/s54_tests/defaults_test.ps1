# Session 54 (Jarrod: "540 as the default for igpus, as well as native as default for unsupported interlock GPUs"):
# the one-time moves of an older settings file (version 2) and the first-run suggestions, on faked devices. Test
# settings files under out\s54 only; every launcher run quits by itself (--exit).
#   pwsh -File tools\launcher\s54_tests\defaults_test.ps1
param([string]$Launcher = "$PSScriptRoot\..\..\..\out\build\win-amd64-release\wwe2k14_launcher.exe")
$R = Join-Path (Resolve-Path "$PSScriptRoot\..\..\..").Path 'out\s54'
New-Item -ItemType Directory -Force "$R\localdata" | Out-Null
function Run($name, $json, $extra) {
    $set = "$R\def_$name.json"
    $json | Set-Content -Encoding utf8NoBOM $set
    $log = "$R\launcher_def_$name.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $a = "--settings $set --local-data $R\localdata --log $log $extra --wait 8 --shot $R\launcher_def_$name.png --exit"
    $l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
    if (-not $l.WaitForExit(40000)) { $l.Kill(); "${name}: did not quit (killed)" } else { "${name}: exit $($l.ExitCode)" }
    Get-Content $log -ErrorAction SilentlyContinue | Select-String 'settings:' | ForEach-Object { "  log: $($_.Line)" }
    $after = Get-Content $set -Raw
    foreach ($k in 'renderer', 'resolution', 'draw_height', 'settings_version') {
        if ($after -match "`"$k`":\s*(\d+)") { "  $k = $($Matches[1])" }
    }
}
$old = '{ "first_run_done": true, "settings_version": 2, "renderer": RR, "resolution": 0, "upscale": 0, "frame_rate": 3 }'
Run 'amd_light' ($old -replace 'RR', '1') '--fake-no-interlock --page settings --settings-section 2'
Run 'igpu_720' ($old -replace 'RR', '2') '--fake-igpu --page settings --settings-section 1'
Run 'amd_light_v3' (($old -replace 'RR', '1') -replace '"settings_version": 2', '"settings_version": 3') '--fake-no-interlock --page settings --settings-section 2'
Run 'wizard_igpu' '{ "first_run_done": false }' '--fake-igpu --wizard --wizard-step 2'
Run 'wizard_amd' '{ "first_run_done": false }' '--fake-no-interlock --wizard --wizard-step 2'
