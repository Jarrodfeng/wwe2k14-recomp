# Build the phone launcher (Session 71, src\launcher_mobile): -Target android -> out\android\launcher-arm64\liblauncher.so
# (build_apk.ps1 packs it), -Target pc -> out\launcher_mobile\pc\wwe2k14_launcher_mobile.exe (the test build in a
# phone-shaped window; tools\android\launcher_shots.ps1 takes its screenshots). Holds the run lock. SDL3 is compiled
# into it from the SDK source tree (the first build takes a minute or two).
#   pwsh -File tools\android\build_launcher.ps1 -Target pc
#   pwsh -File tools\android\build_launcher.ps1 -Target android [-Reconfigure]
param([ValidateSet('pc', 'android')][string]$Target = 'pc', [switch]$Reconfigure, [int]$Jobs = 16)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = "$root\src\launcher_mobile"
. "$root\tools\runlock.ps1"

if ($Target -eq 'android') {
    . "$PSScriptRoot\env.ps1"
    $build = "$AndroidOut\launcher-arm64"
    $cfg = @("-DCMAKE_TOOLCHAIN_FILE=$NdkToolchainFile", '-DANDROID_ABI=arm64-v8a', "-DANDROID_PLATFORM=android-$MinApi",
             '-DANDROID_STL=c++_shared', "-DCMAKE_MAKE_PROGRAM=$Ninja")
    $cmake = $CMake
} else {
    . "$root\tools\env.ps1" | Out-Null
    $build = "$root\out\launcher_mobile\pc"
    $cfg = @('-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
    $cmake = 'cmake'
}
$ErrorActionPreference = 'Continue'
[void][System.IO.Directory]::CreateDirectory("$root\logs\android")
$log = "$root\logs\android\launcher_$Target.log"

$lock = Enter-RunLock -Name "build_launcher.ps1 $Target"
try {
    $t0 = Get-Date
    "== launcher build $Target, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $(git -C $root log --oneline -1)" | Out-File $log
    if ($Reconfigure -or -not (Test-Path "$build\CMakeCache.txt")) {
        & $cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release @cfg *>&1 | Out-File $log -Append
        if ($LASTEXITCODE -ne 0) {
            "configure failed (exit $LASTEXITCODE), log: $log"
            Get-Content $log | Select-String 'CMake Error|error' -Context 0, 3 | Select-Object -First 15 | ForEach-Object { $_.ToString() }
            exit 1
        }
    }
    & $cmake --build $build -j $Jobs *>&1 | Out-File $log -Append
    $code = $LASTEXITCODE
    "launcher build ($Target) exit: $code  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s  log: $log"
    Get-Content $log | Select-String ': error|FAILED|error:|undefined symbol' | Select-Object -First 40 |
        ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
    if ($code -ne 0) { exit $code }
    $out = if ($Target -eq 'android') { "$build\liblauncher.so" } else { "$build\wwe2k14_launcher_mobile.exe" }
    "{0}: {1:N1} MB" -f $out, ((Get-Item $out).Length / 1MB)
} finally { Exit-RunLock $lock }
