# Build + install the ReXGlue SDK for Android arm64 (thirdparty\rexglue-sdk-src, branch android) into
# out\android\sdk-install-arm64 (ANDROID_PLAN.md step 2.1). Holds the run lock. Log: logs\android\sdk_<Run>.log.
#   pwsh -File tools\android\build_sdk_android.ps1 -Run a01               incremental build + install
#   pwsh -File tools\android\build_sdk_android.ps1 -Run a01 -Reconfigure  re-run CMake first (first build, CMake edits)
# The PC SDK build (tools\build_sdk.ps1, out/build/win-amd64) is untouched; the install is never registered in the
# CMake user package registry, so the PC game build can never find it.
#   -KeepGoing: ninja -k 0 (build every file it can; lists all failures in one pass while porting)
param([Parameter(Mandatory)][string]$Run, [switch]$Reconfigure, [int]$Jobs = 16, [switch]$KeepGoing, [string]$AllowBranch = '')
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'
. "$ProjectRoot\tools\runlock.ps1"

$sdk   = "$ProjectRoot\thirdparty\rexglue-sdk-src"
$build = $AndroidDirs.SdkBuild
$inst  = $AndroidDirs.SdkInstall
[void][System.IO.Directory]::CreateDirectory("$ProjectRoot\logs\android")
$log = "$ProjectRoot\logs\android\sdk_$Run.log"

$branch = git -C $sdk branch --show-current
if ($branch -ne 'android' -and $branch -ne $AllowBranch) { "build_sdk_android.ps1: the SDK is on '$branch', not 'android'"; exit 2 }

$lock = Enter-RunLock -Name "build_sdk_android.ps1 $Run"
try {
$t0 = Get-Date
"== SDK android build $Run, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), SDK $(git -C $sdk log --oneline -1)" | Out-File $log
if ($Reconfigure -or -not (Test-Path "$build\CMakeCache.txt")) {
    $flags = "-march=armv8.2-a"
    & $CMake -S $sdk -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$NdkToolchainFile" -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$MinApi" `
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$inst" `
        "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" `
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_BUILD_TESTS=OFF `
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF *>&1 | Out-File $log -Append
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        "configure failed (exit $code), log: $log"
        Get-Content $log | Select-String 'CMake Error|error' -Context 0, 3 | Select-Object -First 15 | ForEach-Object { $_.ToString() }
        exit $code
    }
}
$ninjaArgs = if ($KeepGoing) { @('--', '-k', '0') } else { @() }
& $CMake --build $build --target install -j $Jobs @ninjaArgs *>&1 | Out-File $log -Append
$code = $LASTEXITCODE
"sdk android build exit: $code  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s  log: $log"
Get-Content $log | Select-String ': error|FAILED|error:|undefined symbol' | Select-Object -First $(if ($KeepGoing) { 80 } else { 30 }) |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
if ($code -ne 0) { exit $code }

foreach ($so in 'librexruntime.so', 'librexgpu-xenos.so') {
    $f = Get-ChildItem $inst -Recurse -Filter $so -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { "{0}: {1:N1} MB  {2}" -f $so, ($f.Length / 1MB), $f.FullName } else { "${so}: not in the install folder" }
}
} finally { Exit-RunLock $lock }
