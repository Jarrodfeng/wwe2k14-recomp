# S110 BUILDLOCK: the planted test of tools\harness\folderlock.ps1 and of the Android steps' lock sets (docs\research\s110\BUILDLOCK.md).
#   pwsh -NoProfile -File tools\harness\folderlock_selftest.ps1      -> exit 0 only when every row is PASS (~20 s; no build, no game, no run lock)
# Its own lock folder (WWE_FOLDERLOCK_DIR = a scratch folder); real child processes hold the locks. The folders are names only (never created).
# Rows 1-2 and 7 plant takers that MUST overlap, rows 3-6 takers that must NOT; 8-9 a killed holder and a failed all-or-nothing ask leave nothing held;
# 10-11 read the four Android scripts: each parses, and each takes the folder locks this test plants (the launcher's run lock only for -Target pc).
param([string]$Scratch = (Join-Path ([System.IO.Path]::GetTempPath()) ('folderlock_selftest_' + $PID)))
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here)
$lib = Join-Path $here 'folderlock.ps1'
if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
[void](New-Item -ItemType Directory -Force $Scratch)
$env:WWE_FOLDERLOCK_DIR = Join-Path $Scratch 'locks'
. $lib
$script:fails = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') { if ($ok) { "PASS  $what" } else { $script:fails++; "FAIL  $what  $detail" } }

# A child pwsh that takes -Read / -Write, writes <tag>.ready once it holds them, holds for $Seconds, releases.
function Start-Holder([string]$tag, [string[]]$read, [string[]]$write, [double]$seconds = 30) {
    $r = (@($read) | ForEach-Object { "'$_'" }) -join ','
    $w = (@($write) | ForEach-Object { "'$_'" }) -join ','
    $body = ". '$lib'; `$h = Enter-FolderLocks -Read @($r) -Write @($w) -Name '$tag' -TimeoutMinutes 0.5; Set-Content '$Scratch\$tag.ready' 'x'; Start-Sleep -Milliseconds $([int]($seconds * 1000)); Exit-FolderLocks `$h"
    $p = Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', $body) -PassThru -WindowStyle Hidden
    $t = (Get-Date).AddSeconds(30)
    while (-not (Test-Path "$Scratch\$tag.ready") -and (Get-Date) -lt $t -and -not $p.HasExited) { Start-Sleep -Milliseconds 100 }
    return $p
}
function Stop-Holder($p) { try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { }; [void]$p.WaitForExit(10000) }
function Ready([string]$tag) { return (Test-Path "$Scratch\$tag.ready") }
# $true = the probe got every lock within ~1 s (then it gives them back at once)
function Try-Take([string[]]$read, [string[]]$write) {
    try { $h = Enter-FolderLocks -Read $read -Write $write -Name 'probe' -TimeoutMinutes 0.02 -PollMilliseconds 100 6>$null; Exit-FolderLocks $h; return $true } catch { return $false }
}

$A = "$Scratch\out\android\folder-a"; $B = "$Scratch\out\android\folder-b"
$sdk = "$Scratch\out\android\sdk-install-arm64"; $game = "$Scratch\out\android\game-arm64"; $gameOff = "$Scratch\out\android\game-arm64-off"
$launcher = "$Scratch\out\android\launcher-arm64"; $obj = "$Scratch\out\android\apk-obj"

# 1. two readers of one folder overlap
$p = Start-Holder 'r1' @($A) @(); $ok = (Ready 'r1') -and (Try-Take @($A) @()); Stop-Holder $p
Check '1 MUST OVERLAP: two readers of one folder' $ok
# 2. writers of two different folders overlap
$p = Start-Holder 'w2' @() @($A); $ok = (Ready 'w2') -and (Try-Take @() @($B)); Stop-Holder $p
Check '2 MUST OVERLAP: writers of two different folders' $ok
# 3. two writers of one folder do not
$p = Start-Holder 'w3' @() @($A); $ok = (Ready 'w3') -and -not (Try-Take @() @($A)); Stop-Holder $p
Check '3 MUST NOT: two writers of one folder' $ok
# 4. a writer waits for a reader, and 5. a reader waits for a writer
$p = Start-Holder 'r4' @($A) @(); $ok = (Ready 'r4') -and -not (Try-Take @() @($A)); Stop-Holder $p
Check '4 MUST NOT: a writer beside a reader of its folder' $ok
$p = Start-Holder 'w5' @() @($A); $ok = (Ready 'w5') -and -not (Try-Take @($A) @()); Stop-Holder $p
Check '5 MUST NOT: a reader beside the writer of its folder' $ok
# 6. the real pair: build_launcher.ps1 android (writes launcher-arm64) and build_apk.ps1 (reads it) never overlap; after the launcher ends the APK goes
$p = Start-Holder 'launcher6' @() @($launcher); $ok = (Ready 'launcher6') -and -not (Try-Take @($sdk, $game, $launcher) @($obj)); Stop-Holder $p
Check '6a MUST NOT: build_apk beside build_launcher android' $ok
Check '6b the APK goes once the launcher build has ended' (Try-Take @($sdk, $game, $launcher) @($obj))
# 7. the real pair that overlaps: the stream and the stream-free game library builds (different folders, both read the SDK install)
$p = Start-Holder 'game7' @($sdk) @($gameOff); $ok = (Ready 'game7') -and (Try-Take @($sdk) @($game)); Stop-Holder $p
Check '7 MUST OVERLAP: build_game_android into game-arm64 beside one into game-arm64-off' $ok
# 8. a killed holder frees its lock (the OS closes the handle)
$p = Start-Holder 'w8' @() @($A); $was = Ready 'w8'; Stop-Process -Id $p.Id -Force; [void]$p.WaitForExit(10000)
Check '8 a killed writer leaves the folder free' ($was -and (Try-Take @() @($A)))
# 9. all or nothing: an ask that fails on B keeps nothing of A
$p = Start-Holder 'w9' @() @($B); $failed = (Ready 'w9') -and -not (Try-Take @($A) @($B)); $ok = $failed -and (Try-Take @() @($A)); Stop-Holder $p
Check '9 a failed ask (A read + B written, B busy) leaves A free' $ok

# 10. the scripts parse; 11. each takes the locks planted above (and the APK / android launcher take no run lock)
$scripts = @('tools\android\build_apk.ps1', 'tools\android\build_launcher.ps1', 'tools\android\build_game_android.ps1', 'tools\android\build_sdk_android.ps1', 'tools\harness\folderlock.ps1')
foreach ($s in $scripts) {
    $errs = $null; $tok = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo $s), [ref]$tok, [ref]$errs)
    Check "10 parses: $s" (@($errs).Count -eq 0) ((@($errs) | Select-Object -First 1) -join '')
}
$apk = Get-Content -Raw (Join-Path $repo 'tools\android\build_apk.ps1')
$lau = Get-Content -Raw (Join-Path $repo 'tools\android\build_launcher.ps1')
$gam = Get-Content -Raw (Join-Path $repo 'tools\android\build_game_android.ps1')
$sdkS = Get-Content -Raw (Join-Path $repo 'tools\android\build_sdk_android.ps1')
Check '11a build_apk.ps1: no run lock; reads sdk / game / launcher, writes apk-obj' (($apk -notmatch 'Enter-RunLock') -and $apk.Contains('Enter-FolderLocks -Read $readDirs -Write @($obj)'))
Check '11b build_launcher.ps1: the run lock only inside the -Target pc branch; android writes its folder' ($lau.Contains('    $lock = Enter-RunLock -Name "build_launcher.ps1 $Target" -Mode Build') -and ($lau -notmatch '(?m)^\$lock = Enter-RunLock') -and $lau.Contains('Enter-FolderLocks -Write @($build)'))
Check '11c build_game_android.ps1: BUILD run lock + reads the SDK install, writes its folder' ($gam.Contains('-Mode Build') -and $gam.Contains('Enter-FolderLocks -Read @($AndroidDirs.SdkInstall) -Write @($build)'))
Check '11d build_sdk_android.ps1: BUILD run lock + writes the install' ($sdkS.Contains('-Mode Build') -and $sdkS.Contains('Enter-FolderLocks -Write @($inst)'))
# 11e (BUILDLOCK review): build_apk.ps1 checks the SDK stamp, the game / launcher CMakeCache.txt and the libraries only while it holds the read locks. All-or-none
# means it holds NOTHING while it waits on any one folder, so a check made before the lock can be stale by the time it packs (a -Release APK with a dev libmain.so).
$at = $apk.IndexOf('Enter-FolderLocks -Read $readDirs')
Check '11e build_apk.ps1: the stamp, cache and library checks run under its folder locks' (($at -gt 0) -and ($at -lt $apk.IndexOf('$stampFile = ')) -and ($at -lt $apk.IndexOf('Select-String -LiteralPath $gameCache')) -and ($at -lt $apk.IndexOf('if (-not (Test-Path $libs[$k]))')) -and ($at -lt $apk.IndexOf('Select-String -LiteralPath $launcherCache')))

Remove-Item Env:WWE_FOLDERLOCK_DIR -ErrorAction SilentlyContinue
try { Remove-Item -Recurse -Force $Scratch } catch { }
if ($script:fails) { "folderlock_selftest: $($script:fails) FAIL"; exit 1 }
'folderlock_selftest: all PASS'
exit 0
