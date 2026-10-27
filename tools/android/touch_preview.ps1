# Session 71: the touch controls on the PC (tools\android\touch_preview\TouchPreview.java): compiles TouchPad.java with
# the preview, draws out\launcher_mobile\touch\touch_idle.png / touch_pressed.png at the phone's size and runs the touch
# checks (exit 1 when one fails). No Android, no run lock (no game, no build of the game).
#   pwsh -File tools\android\touch_preview.ps1 [-Size 100] [-Opacity 70] [-Background shot.png] [-Width 3168 -Height 1440]
#   pwsh -File tools\android\touch_preview.ps1 -SaveZip     the save export / import checks instead (SaveZipTest.java)
param([switch]$SaveZip, [int]$Size = 100, [int]$Opacity = 70, [string]$Background = '', [int]$Width = 3168,
      [int]$Height = 1440)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot\env.ps1" -AllowMissing
$java = "$root\android\java\io\github\jarrodfeng\wwe2k14"
$obj = "$root\out\launcher_mobile\touch_classes"
$out = "$root\out\launcher_mobile\touch"
if (Test-Path $obj) { Remove-Item $obj -Recurse -Force }
[void](New-Item -ItemType Directory -Force $obj, $out)
if ($SaveZip) {
    & "$Jdk\bin\javac.exe" -encoding UTF-8 -d $obj "$java\SaveZip.java" "$PSScriptRoot\touch_preview\SaveZipTest.java"
    if ($LASTEXITCODE -ne 0) { "javac failed"; exit 1 }
    & "$Jdk\bin\java.exe" -cp $obj SaveZipTest "$root\out\launcher_mobile\savezip_work"
    exit $LASTEXITCODE
}
& "$Jdk\bin\javac.exe" -encoding UTF-8 -d $obj "$java\TouchPad.java" "$PSScriptRoot\touch_preview\TouchPreview.java"
if ($LASTEXITCODE -ne 0) { "javac failed"; exit 1 }
$a = @('-cp', $obj, 'TouchPreview', $out, "$Width", "$Height", "$Size", "$Opacity")
if ($Background) { $a += $Background }
& "$Jdk\bin\java.exe" @a
exit $LASTEXITCODE
