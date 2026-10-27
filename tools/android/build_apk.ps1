# Package the Android APK (ANDROID_PLAN.md step 4.3): our Java + SDL's Java (javac -> d8), the manifest (aapt2), the
# native libraries (stripped of debug info), zipalign, apksigner with the debug key. No Gradle. Holds the run lock.
#   pwsh -File tools\android\build_apk.ps1              -> out\android\apk\wwe2k14-android-dev.apk
#   pwsh -File tools\android\build_apk.ps1 -Install     ... then adb install -r on the connected phone
# Inputs: out\android\sdk-install-arm64\lib (build_sdk_android.ps1), out\android\game-arm64\libmain.so
# (build_game_android.ps1), out\android\launcher-arm64\liblauncher.so (build_launcher.ps1 -Target android, Session 71).
# Never game files: only our code, the recompiled code and shader_list.txt (CLAUDE.md rule 1), plus Jarrod's own logo
# trace (assets\launcher\wwe2k14_logo.png, the launcher's) and the app icon made from game_logo.png (the fan-made
# SteamGridDB icon the PC exe uses; F13, Jarrod 2026-10-28).
# The debug key lives outside the repo (%USERPROFILE%\.android\wwe2k14-debug.jks), made once with keytool; its password
# is Android's usual debug-key "android" (a development key: release signing is ANDROID_PLAN 9.3).
param([switch]$Install, [switch]$NoDebuggable)
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Stop'
. "$ProjectRoot\tools\runlock.ps1"

$sdl = "$ProjectRoot\thirdparty\rexglue-sdk-src\thirdparty\sdl3\android-project\app\src\main\java"
$ours = "$ProjectRoot\android\java"
$manifest = "$ProjectRoot\android\AndroidManifest.xml"
$obj = "$AndroidOut\apk-obj"
$apk = "$($AndroidDirs.Apk)\wwe2k14-android-dev.apk"
$keystore = "$env:USERPROFILE\.android\wwe2k14-debug.jks"
$libs = [ordered]@{
    'libc++_shared.so'   = "$Llvm\..\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
    'librexruntime.so'   = "$($AndroidDirs.SdkInstall)\lib\librexruntime.so"
    'librexgpu-xenos.so' = "$($AndroidDirs.SdkInstall)\lib\librexgpu-xenos.so"
    'libmain.so'         = "$($AndroidDirs.Game)\libmain.so"
    'liblauncher.so'     = "$AndroidOut\launcher-arm64\liblauncher.so"
}
foreach ($k in $libs.Keys) { if (-not (Test-Path $libs[$k])) { "build_apk.ps1: missing $k ($($libs[$k]))"; exit 2 } }

function Run([string]$What, [scriptblock]$Cmd) {
    $out = & $Cmd 2>&1
    if ($LASTEXITCODE -ne 0) { "build_apk.ps1: $What failed (exit $LASTEXITCODE)"; $out | Select-Object -Last 30; exit 1 }
}

$lock = Enter-RunLock -Name "build_apk.ps1"
try {
$t0 = Get-Date
if (Test-Path $obj) { Remove-Item $obj -Recurse -Force }
foreach ($d in 'classes', 'dex', 'zip\lib\arm64-v8a', 'assets', 'res') { [void](New-Item -ItemType Directory -Force "$obj\$d") }

# 1. Java -> classes -> dex
$sources = @(Get-ChildItem $sdl, $ours -Recurse -Filter *.java | ForEach-Object FullName)
$sources | Set-Content "$obj\sources.txt" -Encoding ascii
Run 'javac' { & "$Jdk\bin\javac.exe" --release 11 -encoding UTF-8 -nowarn -classpath $AndroidJar -d "$obj\classes" "@$obj\sources.txt" }
Run 'jar' { & "$Jdk\bin\jar.exe" cf "$obj\classes.jar" -C "$obj\classes" . }
Run 'd8' { & "$BuildTools\d8.bat" --release --min-api $MinApi --lib $AndroidJar --output "$obj\dex" "$obj\classes.jar" }

# 2. Manifest (+ assets, + the icon) -> base APK
if (Test-Path "$ProjectRoot\db\shaders\shader_list.txt") { Copy-Item "$ProjectRoot\db\shaders\shader_list.txt" "$obj\assets\" }
Copy-Item "$ProjectRoot\assets\launcher\wwe2k14_logo.png" "$obj\assets\"  # the launcher's logo (SDL reads assets)
# The launcher icon in Android's five densities, from game_logo.png (512 x 512).
Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Image]::FromFile("$ProjectRoot\game_logo.png")
try {
    foreach ($d in @(@('mdpi', 48), @('hdpi', 72), @('xhdpi', 96), @('xxhdpi', 144), @('xxxhdpi', 192))) {
        [void](New-Item -ItemType Directory -Force "$obj\res\mipmap-$($d[0])")
        $bmp = [System.Drawing.Bitmap]::new($d[1], $d[1])
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($icon, 0, 0, $d[1], $d[1])
        $g.Dispose()
        $bmp.Save("$obj\res\mipmap-$($d[0])\ic_launcher.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
    }
} finally { $icon.Dispose() }
Run 'aapt2 compile' { & "$BuildTools\aapt2.exe" compile --dir "$obj\res" -o "$obj\res.zip" }
$aaptArgs = @('link', '--manifest', $manifest, '-I', $AndroidJar, '--min-sdk-version', "$MinApi",
              '--target-sdk-version', "$ApiLevel", '-A', "$obj\assets", '-R', "$obj\res.zip", '--auto-add-overlay',
              '-o', "$obj\base.apk")
if (-not $NoDebuggable) { $aaptArgs += '--debug-mode' }
Run 'aapt2 link' { & "$BuildTools\aapt2.exe" @aaptArgs }

# 3. classes.dex + the native libraries (debug info stripped; the unstripped copies stay in out\android\symbols)
Copy-Item "$obj\dex\classes.dex" "$obj\zip\classes.dex"
foreach ($k in $libs.Keys) {
    Run "llvm-strip $k" { & "$Llvm\llvm-strip.exe" --strip-debug -o "$obj\zip\lib\arm64-v8a\$k" $libs[$k] }
}
Copy-Item "$($AndroidDirs.SdkInstall)\lib\librexruntime.so", "$($AndroidDirs.SdkInstall)\lib\librexgpu-xenos.so" $AndroidDirs.Symbols -Force
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open("$obj\base.apk", [System.IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($f in Get-ChildItem "$obj\zip" -Recurse -File) {
        $name = $f.FullName.Substring("$obj\zip\".Length).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $name,
            [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }

# 4. Align (16 KB for .so files) and sign
Run 'zipalign' { & "$BuildTools\zipalign.exe" -f -P 16 4 "$obj\base.apk" "$obj\aligned.apk" }
if (-not (Test-Path $keystore)) {
    [void](New-Item -ItemType Directory -Force (Split-Path $keystore))
    Run 'keytool' { & "$Jdk\bin\keytool.exe" -genkeypair -keystore $keystore -storepass android -keypass android `
        -alias wwe2k14-debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=wwe2k14 debug, O=wwe2k14-recomp" }
    "created the debug key $keystore"
}
Run 'apksigner' { & "$BuildTools\apksigner.bat" sign --ks $keystore --ks-pass pass:android --ks-key-alias wwe2k14-debug `
    --out $apk "$obj\aligned.apk" }
"apk: {0:N1} MB  {1}  ({2} s)" -f ((Get-Item $apk).Length / 1MB), $apk, [int]((Get-Date) - $t0).TotalSeconds
} finally { Exit-RunLock $lock }

if ($Install) {
    "installing on the phone (confirm on the phone if ColorOS asks)..."
    & $Adb install -r $apk
    exit $LASTEXITCODE
}
