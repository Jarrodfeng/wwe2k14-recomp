<#
.SYNOPSIS
    Android build environment for wwe2k14-recomp (ANDROID_PLAN.md step 1.3).

.DESCRIPTION
    Dot-sourced by every tools\android\*.ps1 script. Sets:
      $AndroidSdk  the Android SDK (Android Studio's default place, so Studio can use it later)
      $Ndk         the newest NDK under $AndroidSdk\ndk
      $Llvm        the NDK's clang / llvm-* folder (toolchains\llvm\prebuilt\windows-x86_64\bin)
      $BuildTools  the newest build-tools (aapt2, d8, zipalign, apksigner)
      $AndroidJar  platforms\android-36\android.jar
      $Jdk         JDK used by sdkmanager / d8 / apksigner (also sets JAVA_HOME for this shell)
      $Adb         platform-tools\adb.exe
      $Pkg         the app's package name (Q1, Jarrod 2026-10-25 17:03; never change it)
      $PhoneFiles  the app's own folder on the phone
      $AndroidOut  out\android, with the folders below
    Throws a clear message for anything missing, unless -AllowMissing is passed (doctor.ps1 uses that to report).
    Only affects the current shell.

.EXAMPLE
    . "$PSScriptRoot\env.ps1"
    . "$PSScriptRoot\env.ps1" -AllowMissing
#>
param([switch]$AllowMissing)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AndroidSdk  = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "$env:LOCALAPPDATA\Android\Sdk" }
$Pkg         = "io.github.jarrodfeng.wwe2k14"
$PhoneFiles  = "/sdcard/Android/data/$Pkg/files"
$ApiLevel    = 36
$MinApi      = 28

$AndroidOut = Join-Path $ProjectRoot "out\android"
$AndroidDirs = [ordered]@{
    SdkBuild   = "$AndroidOut\sdk-build-arm64"
    SdkInstall = "$AndroidOut\sdk-install-arm64"
    Game       = "$AndroidOut\game-arm64"
    Apk        = "$AndroidOut\apk"
    Symbols    = "$AndroidOut\symbols"
    Runs       = "$AndroidOut\runs"
}

$AndroidMissing = [System.Collections.Generic.List[string]]::new()
function Get-NewestChild([string]$Dir) {
    if (-not (Test-Path $Dir)) { return $null }
    # Version folders (29.0.14206865, 36.0.0): sort numerically, not as text.
    Get-ChildItem $Dir -Directory | Where-Object { $_.Name -match '^\d+(\.\d+)*$' } |
        Sort-Object { [version]($_.Name + ('.0' * [Math]::Max(0, 2 - ($_.Name.Split('.').Count - 1)))) } |
        Select-Object -Last 1 -ExpandProperty FullName
}

# -- JDK: sdkmanager and d8 need 17+; PATH may hold an old Java 8 first ---------------------------------------------
$Jdk = $null
foreach ($c in @($env:JAVA_HOME, "C:\Program Files\Java\jdk-24", "C:\Program Files\Microsoft\jdk-17*") | Where-Object { $_ }) {
    $hit = Get-Item $c -ErrorAction SilentlyContinue | Where-Object { Test-Path "$($_.FullName)\bin\javac.exe" } | Select-Object -First 1
    if ($hit) { $Jdk = $hit.FullName; break }
}
if ($Jdk) { $env:JAVA_HOME = $Jdk } else { $AndroidMissing.Add("JDK 17+ (javac.exe)") }

# -- SDK pieces ------------------------------------------------------------------------------------------------------
$SdkManager = "$AndroidSdk\cmdline-tools\latest\bin\sdkmanager.bat"
$Adb        = "$AndroidSdk\platform-tools\adb.exe"
$Ndk        = Get-NewestChild "$AndroidSdk\ndk"
$BuildTools = Get-NewestChild "$AndroidSdk\build-tools"
$AndroidJar = "$AndroidSdk\platforms\android-$ApiLevel\android.jar"
$Llvm       = if ($Ndk) { "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\bin" } else { $null }
$NdkToolchainFile = if ($Ndk) { "$Ndk\build\cmake\android.toolchain.cmake" } else { $null }

# CMake + Ninja: VS 2022's own copies (none on PATH). Not the VS developer shell: its MSVC INCLUDE / LIB must not
# reach the NDK's clang.
$CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $CMake -or -not $Ninja) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -version "[17.0,18.0)" -products * -property installationPath | Select-Object -First 1 }
    $vsCMake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
    if (-not $CMake -and (Test-Path "$vsCMake\CMake\bin\cmake.exe")) { $CMake = "$vsCMake\CMake\bin\cmake.exe" }
    if (-not $Ninja -and (Test-Path "$vsCMake\Ninja\ninja.exe"))      { $Ninja = "$vsCMake\Ninja\ninja.exe" }
}
if (-not $CMake) { $AndroidMissing.Add("cmake (VS 2022's CMake component)") }
if (-not $Ninja) { $AndroidMissing.Add("ninja (VS 2022's CMake component)") }

# scrcpy (optional; winget's package folder, since its alias needs a new shell to be on PATH)
$Scrcpy = (Get-Command scrcpy -ErrorAction SilentlyContinue).Source
if (-not $Scrcpy) {
    $Scrcpy = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Genymobile.scrcpy_*\scrcpy-win64-*\scrcpy.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
}

if (-not (Test-Path $SdkManager)) { $AndroidMissing.Add("command-line tools ($SdkManager)") }
if (-not (Test-Path $Adb))        { $AndroidMissing.Add("platform-tools (adb)") }
if (-not $Ndk)                    { $AndroidMissing.Add("NDK (sdkmanager ndk;29.0.14206865)") }
if (-not $BuildTools)             { $AndroidMissing.Add("build-tools (sdkmanager build-tools;36.0.0)") }
if (-not (Test-Path $AndroidJar)) { $AndroidMissing.Add("platform android-$ApiLevel (sdkmanager platforms;android-$ApiLevel)") }

if ($AndroidMissing.Count -and -not $AllowMissing) {
    throw "Android tools missing: $($AndroidMissing -join '; '). See ANDROID_PLAN.md phase 1."
}
foreach ($d in $AndroidDirs.Values) { [void][System.IO.Directory]::CreateDirectory($d) }
