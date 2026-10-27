# Screenshots of the phone launcher's PC test build (Session 71): every page, portrait and landscape, into
# out\launcher_mobile\shots\<orientation>_<page>.png. Its own files folder (out\launcher_mobile\test_files), never the
# phone's; the game files: -Game (default game_data\iso\wwe2k14_test.iso, read only) or -NoGame.
#   pwsh -File tools\android\launcher_shots.ps1 [-Pages 0,1,5] [-Orient portrait|landscape|both] [-NoGame]
param([string]$Pages = '0,1,2,3,4,5,6,7',  # a comma list (pwsh -File passes arrays as one string)
      [ValidateSet('portrait', 'landscape', 'both')][string]$Orient = 'both',
      [string]$Game = '', [switch]$NoGame, [float]$Scroll = 0, [int]$Select = -1, [string]$Tag = '', [switch]$Drawer)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = "$root\out\launcher_mobile\pc\wwe2k14_launcher_mobile.exe"
if (-not (Test-Path $exe)) { "launcher_shots.ps1: build it first (build_launcher.ps1 -Target pc)"; exit 2 }
$files = "$root\out\launcher_mobile\test_files"
$shots = "$root\out\launcher_mobile\shots"
[void](New-Item -ItemType Directory -Force $files, $shots)
if (-not $Game -and -not $NoGame) { $Game = "$root\game_data\iso\wwe2k14_test.iso" }
$settings = "$files\launcher_settings.json"
$json = if (Test-Path $settings) { Get-Content $settings -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
$json['game_uri'] = if ($NoGame) { '' } else { $Game }
$json['game_name'] = if ($NoGame) { '' } else { Split-Path $Game -Leaf }
$json | ConvertTo-Json | Set-Content $settings -Encoding utf8

$sizes = [ordered]@{}
if ($Orient -ne 'landscape') { $sizes['portrait'] = '720x1584' }
if ($Orient -ne 'portrait') { $sizes['landscape'] = '1584x720' }
foreach ($o in $sizes.Keys) {
    foreach ($p in ($Pages -split ',' | ForEach-Object { [int]$_ })) {
        $out = "$shots\$($o)_$p$Tag.png"
        $a = @('--files', $files, '--window', $sizes[$o], '--scale', '1.75', '--page', "$p", '--shot', $out, '--frames', '6')
        if ($Scroll) { $a += @('--scroll', "$Scroll") }
        if ($Select -ge 0) { $a += @('--select', "$Select") }
        if ($Drawer) { $a += @('--drawer', '1') }
        $proc = Start-Process $exe -ArgumentList $a -PassThru -WindowStyle Hidden
        if (-not $proc.WaitForExit(30000)) { $proc.Kill(); "page $p ($o): timed out" ; continue }
        if (Test-Path $out) { "$out" } else { "page $p ($o): no shot (exit $($proc.ExitCode))" }
    }
}
Get-Content "$files\logs\launcher.log" -Tail 8
