# S111 SHADERBAR: the phone's shader precompile screen (android\java\...\PrecompileBar.java: title, bar, counts, clock, estimate)
# checked on the PC with tools\android\precompile_bar\PrecompileBarTest.java, then (when the Android SDK and SDL's Java are on this
# PC) every Java file of the APK compiled against android.jar, as build_apk.ps1 step 1 does (classes only: no dex, no APK).
# No Android device, no game, no build of the game.
#   pwsh -NoProfile -File tools\android\precompile_bar.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot\env.ps1" -AllowMissing
$javac = if ($Jdk) { "$Jdk\bin\javac.exe" } else { 'javac' }
$javaexe = if ($Jdk) { "$Jdk\bin\java.exe" } else { 'java' }
$java = "$root\android\java\io\github\jarrodfeng\wwe2k14"
$obj = "$root\out\launcher_mobile\precompile_bar_classes"
if (Test-Path $obj) { Remove-Item $obj -Recurse -Force }
[void](New-Item -ItemType Directory -Force $obj)
& $javac -encoding UTF-8 -d $obj "$java\PrecompileBar.java" "$PSScriptRoot\precompile_bar\PrecompileBarTest.java"
if ($LASTEXITCODE -ne 0) { "javac failed"; exit 1 }
& $javaexe -cp $obj io.github.jarrodfeng.wwe2k14.PrecompileBarTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# The activity itself (GameActivity.java uses PrecompileBar): the APK's javac line. A worktree (out\wt\<name>) takes SDL's Java
# from the main checkout, as build_apk.ps1 does.
$deps = $root
if (-not (Test-Path "$root\thirdparty\rexglue-sdk-src") -and (Test-Path "$root\..\..\..\thirdparty\rexglue-sdk-src")) {
    $deps = (Resolve-Path "$root\..\..\..").Path
}
$sdl = "$deps\thirdparty\rexglue-sdk-src\thirdparty\sdl3\android-project\app\src\main\java"
if (-not $AndroidJar -or -not (Test-Path $AndroidJar) -or -not (Test-Path $sdl)) {
    "precompile_bar: the activity compile skipped (no android.jar or no SDL Java on this PC)"
    exit 0
}
$apk = "$root\out\launcher_mobile\precompile_bar_apk_classes"
if (Test-Path $apk) { Remove-Item $apk -Recurse -Force }
[void](New-Item -ItemType Directory -Force $apk)
$list = "$apk\sources.txt"
@(Get-ChildItem $sdl, "$root\android\java" -Recurse -Filter *.java | ForEach-Object { '"' + $_.FullName.Replace('\', '/') + '"' }) |
    Set-Content $list -Encoding ascii
& $javac --release 11 -encoding UTF-8 -nowarn -classpath $AndroidJar -d $apk "@$list"
if ($LASTEXITCODE -ne 0) { "precompile_bar: the APK's Java does not compile"; exit 1 }
"precompile_bar: the APK's Java compiles"
exit 0
