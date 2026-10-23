# Session 54 part A: the RESOLUTION row in the launcher (Settings -> Display, the first-run wizard) with NATIVE (all
# seven sizes) and LIGHT (720p / 1440p / 4K and a note). Test settings files and local data under out\s54 only; every
# launcher run quits by itself (--exit) after a screenshot.
#   pwsh -File tools\launcher\s54_tests\res_test.ps1 [-Launcher out\build\win-amd64-release\wwe2k14_launcher.exe]
param([string]$Launcher = "$PSScriptRoot\..\..\..\out\build\win-amd64-release\wwe2k14_launcher.exe")
$R = Join-Path (Resolve-Path "$PSScriptRoot\..\..\..").Path 'out\s54'
New-Item -ItemType Directory -Force "$R\localdata" | Out-Null
function Settings($name, $renderer, $resolution, $height) {
    $p = "$R\$name.json"
    "{`n  `"first_run_done`": true,`n  `"settings_version`": 2,`n  `"renderer`": $renderer,`n  `"resolution`": $resolution,`n  `"draw_height`": $height,`n  `"upscale`": 0`n}" |
        Set-Content -Encoding utf8NoBOM $p
    return $p
}
$cases = @(
    @{ name = 'res_native_480'; r = 2; res = 0; h = 480; extra = '--page settings --settings-section 1' },
    @{ name = 'res_light_480'; r = 1; res = 0; h = 480; extra = '--page settings --settings-section 1' },
    @{ name = 'res_native_1080'; r = 2; res = 1; h = 1080; extra = '--page settings --settings-section 1' },
    @{ name = 'res_wizard'; r = 2; res = 0; h = 360; extra = '--wizard --wizard-step 2' }
)
foreach ($c in $cases) {
    $set = Settings $c.name $c.r $c.res $c.h
    $log = "$R\launcher_$($c.name).log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $a = "--settings $set --local-data $R\localdata --log $log $($c.extra) --wait 8 --shot $R\launcher_$($c.name).png --exit"
    $l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
    if (-not $l.WaitForExit(40000)) { $l.Kill(); "$($c.name): did not quit (killed)" } else { "$($c.name): exit $($l.ExitCode), shot $R\launcher_$($c.name).png" }
    $after = Get-Content $set -Raw
    if ($after -match '"draw_height":\s*(\d+)') { "  settings file after: draw_height $($Matches[1])" }
}
