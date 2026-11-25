# Build the phone launcher (Session 71, src\launcher_mobile): -Target android -> out\android\launcher-arm64\liblauncher.so
# (+ libviewer.so, the Arena Viewer for ViewerActivity: Session 98 phone port stage C)
# (build_apk.ps1 packs it), -Target pc -> out\launcher_mobile\pc\wwe2k14_launcher_mobile.exe (the test build in a
# phone-shaped window; tools\android\launcher_shots.ps1 takes its screenshots). -Target pc holds a BUILD run-lock slot, -Target android a folder lock
# (S110 BUILDLOCK) and no run lock. SDL3 is compiled
# into it from the SDK source tree (the first build takes a minute or two).
#   pwsh -File tools\android\build_launcher.ps1 -Target pc
#   pwsh -File tools\android\build_launcher.ps1 -Target android [-Reconfigure]
#   -StreamFree (S105 A1b): the launcher of a stream-free game (WWE2K14_KEEP_STREAM=OFF, src\stream_free_target.h): no PREPARE SHADERS precompile,
#   no DLSS / FSR, the advanced renderer flag ignored. build_all.ps1 passes it with the game library's -StreamFree (a -Release implies both). A change of
#   this switch reconfigures by itself (the same build tree).
param([ValidateSet('pc', 'android')][string]$Target = 'pc', [switch]$Reconfigure, [int]$Jobs = 16, [switch]$StreamFree)
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

# S106 G2L: -Target pc writes only out\launcher_mobile\pc (no game run reads it), so it takes the BUILD lock class (beside untimed game runs, 3 at once) instead of
# the exclusive one (S105 11-20 17:27: an exclusive launcher build waited 624.9 s behind game runs and held the lock 0.1 s). Refused while a process runs from the build
# folder (the link would fail on a running exe).
# S110 BUILDLOCK (docs\research\s110\BUILDLOCK.md): -Target android takes NO run-lock hold. It writes only out\android\launcher-arm64 (+ its log), which no PC game
# run loads; what it shares is that folder, with build_apk.ps1 (which packs it) and a second launcher build: the folder's WRITER lock (tools\harness\folderlock.ps1)
# keeps both out. Its compiles go through the compile queue (admit: memory, and an -Alone run's pause). v0.4.3 night: it waited 159.7 s on a timed run for a 38.7 s build.
$lock = $null; $androidLock = $null
if ($Target -eq 'pc') {
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and $_.Path.StartsWith("$build\", [StringComparison]::OrdinalIgnoreCase) } catch { $false } })
    if ($running.Count) { "REFUSED: $($running.Count) process(es) run from $build (pid $($running[0].Id) ...): close it and build again"; exit 6 }
    $lock = Enter-RunLock -Name "build_launcher.ps1 $Target" -Mode Build
} else {
    . "$root\tools\harness\folderlock.ps1"
    . "$root\tools\admit\Admit.ps1"
    try { $androidLock = Enter-FolderLocks -Write @($build) -Name "build_launcher.ps1 android" }
    catch { "REFUSED, nothing built: $($_.Exception.Message)"; exit 6 }
    try { $Jobs = Request-BuildParallel -Want $Jobs -Kind cc.tool }
    catch { Exit-FolderLocks $androidLock; "NOT BUILT: $($_.Exception.Message)"; exit 75 }
}
# S106 G2LF F9: the BUILD class lets two of these run at once, and two pc builds would write one folder and one log: a per-folder lock file (open with no sharing,
# held to the end) refuses the second. Not needed for android: the folder's writer lock (above, S110 BUILDLOCK) keeps a second android build out.
$folderLock = $null
if ($Target -eq 'pc') {
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $build))
    try { $folderLock = [System.IO.File]::Open("$build.build.lock", 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { Exit-RunLock $lock; "REFUSED: another build_launcher.ps1 pc is already building $build (lock file $build.build.lock is held): wait for it"; exit 6 }
}
try {
    $t0 = Get-Date
    "== launcher build $Target, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $(git -C $root log --oneline -1)" | Out-File $log
    $streamVal = if ($StreamFree) { 'OFF' } else { 'ON' }
    $cacheFile = "$build\CMakeCache.txt"
    $cacheWrong = (Test-Path $cacheFile) -and -not (Select-String -LiteralPath $cacheFile -Pattern "^WWE2K14_KEEP_STREAM:BOOL=$streamVal" -Quiet)
    if ($Reconfigure -or $cacheWrong -or -not (Test-Path $cacheFile)) {
        & $cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DWWE2K14_KEEP_STREAM=$streamVal" @cfg *>&1 | Out-File $log -Append
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
    # Session 98 (Arena Viewer phone port, stage C): the android build also makes libviewer.so (ViewerActivity), same CMake project.
    $outs = if ($Target -eq 'android') { @("$build\liblauncher.so", "$build\libviewer.so") } else { @("$build\wwe2k14_launcher_mobile.exe") }
    foreach ($out in $outs) {
        if (-not (Test-Path $out)) { "missing $out"; exit 1 }
        "{0}: {1:N1} MB" -f $out, ((Get-Item $out).Length / 1MB)
    }
} finally { if ($folderLock) { $folderLock.Dispose() }; if ($androidLock) { Release-BuildParallel; Exit-FolderLocks $androidLock }; if ($lock) { Exit-RunLock $lock } }
