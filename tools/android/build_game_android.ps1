# Build the game library for Android arm64 (out\android\game-arm64\libmain.so) against the Android SDK install
# (ANDROID_PLAN.md step 3.2). Holds the run lock. Log: logs\android\game_<Run>.log.
#   pwsh -File tools\android\build_game_android.ps1 -Run g01               incremental build
#   pwsh -File tools\android\build_game_android.ps1 -Run g01 -Reconfigure  re-run CMake first
#   -KeepGoing: ninja -k 0 (every failure in one pass while porting)
#   -Target wwe2k14_recomp: only the generated code (the long part; needs none of our headers)
# The generated code comes from the PC build: refused while generated\default\codegen.build.stamp is older than the
# manifest or any config\*.toml (run the PC build first, which re-runs codegen). The first build compiles all the
# generated files (tens of minutes). The unstripped libmain.so is copied to out\android\symbols (ndk-stack).
param([Parameter(Mandatory)][string]$Run, [switch]$Reconfigure, [int]$Jobs = 16, [switch]$KeepGoing,
      [string]$Target = 'wwe2k14')
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'
. "$ProjectRoot\tools\runlock.ps1"

$build = $AndroidDirs.Game
[void][System.IO.Directory]::CreateDirectory("$ProjectRoot\logs\android")
$log = "$ProjectRoot\logs\android\game_$Run.log"

$stamp = Get-Item "$ProjectRoot\generated\default\codegen.build.stamp" -ErrorAction SilentlyContinue
if (-not $stamp) { "build_game_android.ps1: no generated code (run the PC build first: tools\build.ps1)"; exit 2 }
$inputs = @(Get-Item "$ProjectRoot\wwe2k14_manifest.toml") + @(Get-ChildItem "$ProjectRoot\config\*.toml")
# 5 s slack: a release build writes the manifest and the stamp in the same second (16:01:04, v0.2.5: 10 ms apart).
$newer = @($inputs | Where-Object { $_.LastWriteTime -gt $stamp.LastWriteTime.AddSeconds(5) })
if ($newer) { "build_game_android.ps1: codegen is stale ($($newer.Name -join ', ') newer than the stamp): run the PC build first"; exit 2 }
if (-not (Test-Path "$($AndroidDirs.SdkInstall)\lib\cmake\rexglue\rexglueConfig.cmake")) {
    "build_game_android.ps1: no Android SDK install (run tools\android\build_sdk_android.ps1 first)"; exit 2
}
# The PC SDK's codegen tool (the same one tools\env.ps1 picks), so a codegen step here runs on the PC.
$pcRexglue = @("$ProjectRoot\thirdparty\rexglue-sdk-vulkan\bin\rexglue.exe", "$ProjectRoot\thirdparty\rexglue-sdk\bin\rexglue.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1

$lock = Enter-RunLock -Name "build_game_android.ps1 $Run"
try {
$t0 = Get-Date
"== game android build $Run, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $(git -C $ProjectRoot log --oneline -1)" | Out-File $log
if ($Reconfigure -or -not (Test-Path "$build\CMakeCache.txt")) {
    $flags = "-march=armv8.2-a"
    & $CMake -S $ProjectRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$NdkToolchainFile" -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$MinApi" `
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" `
        "-DCMAKE_PREFIX_PATH=$($AndroidDirs.SdkInstall)" "-DCMAKE_FIND_ROOT_PATH=$($AndroidDirs.SdkInstall)" `
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF "-DWWE2K14_PC_REXGLUE=$pcRexglue" *>&1 | Out-File $log -Append
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        "configure failed (exit $code), log: $log"
        Get-Content $log | Select-String 'CMake Error|error' -Context 0, 3 | Select-Object -First 15 | ForEach-Object { $_.ToString() }
        exit $code
    }
}
$ninjaArgs = if ($KeepGoing) { @('--', '-k', '0') } else { @() }
& $CMake --build $build --target $Target -j $Jobs @ninjaArgs *>&1 | Out-File $log -Append
$code = $LASTEXITCODE
"game android build exit: $code  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s  log: $log"
Get-Content $log | Select-String ': error|FAILED|error:|undefined symbol' | Select-Object -First $(if ($KeepGoing) { 80 } else { 30 }) |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
if ($code -ne 0) { exit $code }

$so = Get-ChildItem $build -Recurse -Filter 'libmain.so' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($so) {
    Copy-Item $so.FullName "$($AndroidDirs.Symbols)\libmain.so" -Force
    "libmain.so: {0:N1} MB  {1} (copy in out\android\symbols)" -f ($so.Length / 1MB), $so.FullName
} else { "libmain.so: not found under $build" }
} finally { Exit-RunLock $lock }
