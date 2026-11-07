# Build the game library for Android arm64 (out\android\game-arm64\libmain.so) against the Android SDK install
# (ANDROID_PLAN.md step 3.2). Holds the run lock. Log: logs\android\game_<Run>.log.
#   pwsh -File tools\android\build_game_android.ps1 -Run g01               incremental build
#   pwsh -File tools\android\build_game_android.ps1 -Run g01 -Reconfigure  re-run CMake first
#   -KeepGoing: ninja -k 0 (every failure in one pass while porting)
#   -Target wwe2k14_recomp: only the generated code (the long part; needs none of our headers)
#   -Jobs N: default 12 (Session 95: the PC has 32 threads shared by up to 3 builds; 16 each oversubscribed it)
#   -Cache: Session 95 (process report item 6) compiles through sccache (NDK clang as the CMake compiler launcher). OFF by
#           default until one measured build proves it (the PCH may block hits). A build folder configured without the cache is
#           reconfigured once when -Cache is given. Stats are printed after the build. -BuildDir: another build folder (a trial).
# The generated code comes from the PC build: refused while generated\default\codegen.build.stamp is older than the
# manifest or any config\*.toml (run the PC build first, which re-runs codegen). The first build compiles all the
# generated files (tens of minutes). The unstripped libmain.so is copied to out\android\symbols (ndk-stack).
param([Parameter(Mandatory)][string]$Run, [switch]$Reconfigure, [int]$Jobs = 12, [switch]$KeepGoing,
      [string]$Target = 'wwe2k14', [switch]$Cache, [string]$BuildDir = '')
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'
. "$ProjectRoot\tools\runlock.ps1"

$build = if ($BuildDir) { $BuildDir } else { $AndroidDirs.Game }
$sccache = ''; $launcherOpts = @()
if ($Cache) {
    $c = Get-Command sccache -ErrorAction SilentlyContinue
    $sccache = if ($c) { $c.Source } elseif (Test-Path "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe") { "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe" } else { '' }
    if ($sccache) {
        $env:SCCACHE_BASEDIRS = $ProjectRoot
        $env:SCCACHE_SERVER_PORT = '4950'   # outside fast_build's 4300-4899 range
        $env:SCCACHE_DIRECT = 'false'       # Session 95: fast_build.ps1 says why (a direct hit took another branch's object)
        & $sccache --stop-server *> $null
        $launcherOpts = @("-DCMAKE_C_COMPILER_LAUNCHER=$sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache")
        if (-not $env:SCCACHE_CACHE_SIZE) { $env:SCCACHE_CACHE_SIZE = '40G' }
        if ((Test-Path "$build\CMakeCache.txt") -and -not (Select-String -LiteralPath "$build\CMakeCache.txt" -Pattern '^CMAKE_CXX_COMPILER_LAUNCHER:.*sccache' -Quiet)) { $Reconfigure = $true }
    } else { "build_game_android.ps1: -Cache given but sccache is not installed; compiling without it" }
} elseif ((Test-Path "$build\CMakeCache.txt") -and (Select-String -LiteralPath "$build\CMakeCache.txt" -Pattern '^CMAKE_CXX_COMPILER_LAUNCHER:.*sccache' -Quiet)) {
    # Session 95 review: a folder configured with -Cache keeps the launcher in CMakeCache; without -Cache clear it.
    $Reconfigure = $true; $launcherOpts = @('-UCMAKE_C_COMPILER_LAUNCHER', '-UCMAKE_CXX_COMPILER_LAUNCHER')
}
# Session 95 review: sccache's key holds the preprocessed text, not the precompiled header contents, so a changed SDK
# header would return stale objects. With -Cache the PCH is off (compiles the same headers, just slower per file).
if ($sccache) { $launcherOpts += '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON' }
[void][System.IO.Directory]::CreateDirectory("$ProjectRoot\logs\android")
$log = "$ProjectRoot\logs\android\game_$Run.log"

$stamp = Get-Item "$ProjectRoot\generated\default\codegen.build.stamp" -ErrorAction SilentlyContinue
if (-not $stamp) { "build_game_android.ps1: no generated code (run the PC build first: tools\build.ps1)"; exit 2 }
$inputs = @(Get-Item "$ProjectRoot\wwe2k14_manifest.toml") + @(Get-ChildItem "$ProjectRoot\config\*.toml")
# Session 77 (review): the codegen tool too - a changed instruction builder must regenerate before this build (the
# fctid / fctidz fix was first compiled with the old generated statements).
$inputs += @(Get-Item "$ProjectRoot\thirdparty\rexglue-sdk-vulkan\bin\rexglue.exe", "$ProjectRoot\thirdparty\rexglue-sdk-vulkan\bin\rexruntime.dll" -ErrorAction SilentlyContinue)
# 5 s slack: a release build writes the manifest and the stamp in the same second (16:01:04, v0.2.5: 10 ms apart).
$newer = @($inputs | Where-Object { $_.LastWriteTime -gt $stamp.LastWriteTime.AddSeconds(5) })
if ($newer) { "build_game_android.ps1: codegen is stale ($($newer.Name -join ', ') newer than the stamp): run the PC build first"; exit 2 }
if (-not (Test-Path "$($AndroidDirs.SdkInstall)\lib\cmake\rexglue\rexglueConfig.cmake")) {
    "build_game_android.ps1: no Android SDK install (run tools\android\build_sdk_android.ps1 first)"; exit 2
}
# The PC SDK's codegen tool (the same one tools\env.ps1 picks), so a codegen step here runs on the PC.
$pcRexglue = @("$ProjectRoot\thirdparty\rexglue-sdk-vulkan\bin\rexglue.exe", "$ProjectRoot\thirdparty\rexglue-sdk\bin\rexglue.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1

# Session 92: BUILD class (like fast_build): this writes only under out\android (+ codegen, which PC builds - exclusive
# holders - never overlap), so it shares the PC with untimed runs instead of waiting for every game to close.
$lock = Enter-RunLock -Name "build_game_android.ps1 $Run" -Mode Build
try {
$t0 = Get-Date
"== game android build $Run, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $(git -C $ProjectRoot log --oneline -1)" | Out-File $log
if ($Reconfigure -or -not (Test-Path "$build\CMakeCache.txt")) {
    $flags = "-march=armv8.2-a"
    & $CMake -S $ProjectRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$NdkToolchainFile" -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$MinApi" `
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" `
        "-DCMAKE_PREFIX_PATH=$($AndroidDirs.SdkInstall)" "-DCMAKE_FIND_ROOT_PATH=$($AndroidDirs.SdkInstall)" `
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF "-DWWE2K14_PC_REXGLUE=$pcRexglue" @launcherOpts *>&1 | Out-File $log -Append
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
if ($sccache) { $st = @(& $sccache --show-stats 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match '^(Compile requests executed|Cache hits\s|Cache hits rate|Cache misses\s|Non-cacheable|Max cache size)' }); "sccache: " + (($st | ForEach-Object { ($_ -replace '\s{2,}', ' = ').Trim() }) -join '; ') }
"game android build exit: $code  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s  log: $log"
Get-Content $log | Select-String ': error|FAILED|error:|undefined symbol' | Select-Object -First $(if ($KeepGoing) { 80 } else { 30 }) |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
if ($code -ne 0) { exit $code }

$so = Get-ChildItem $build -Recurse -Filter 'libmain.so' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($so) {
    if ($BuildDir) { "libmain.so: {0:N1} MB  {1} (trial folder: out\android\symbols left alone)" -f ($so.Length / 1MB), $so.FullName }
    else {
        Copy-Item $so.FullName "$($AndroidDirs.Symbols)\libmain.so" -Force
        "libmain.so: {0:N1} MB  {1} (copy in out\android\symbols)" -f ($so.Length / 1MB), $so.FullName
    }
} else { "libmain.so: not found under $build" }
} finally { Exit-RunLock $lock }
