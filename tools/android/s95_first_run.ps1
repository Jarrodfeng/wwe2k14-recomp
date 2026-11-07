# Session 95 (Lane E): the first phone run, in the order of the Android report's list "One APK at the end, about 6 runs on the 830"
# (out\s95\android_wide.md lines 361-369; no document is labelled "section C"), plus the Chamber on a phone. NOT RUN YET (0 game runs in round 7).
#   pwsh -File tools\android\s95_first_run.ps1 -First 200            (all steps)
#   pwsh -File tools\android\s95_first_run.ps1 -First 200 -Only 4    (one step)
# Every step goes through tools\android\run.ps1 (fresh test user data, test settings, args.txt; never Jarrod's folders). run.ps1 writes args.txt and the
# game uses THAT LIST ONLY (GameActivity.java: the launcher's GameArgs() is not applied), so steps 2-7 pass the phone's flags themselves ($phoneArgs, mirroring
# src\launcher_mobile\mobile_settings.h GameArgs: TRUE NATIVE, transcode, thermal, keep-warm, dynamic res, LOW crowd, by_load) plus the crowd rank diag.
# Step 1 is the RELEASED APK: it gets no phone flags (that APK does not know by_load).
# Steps 1-3 run the match in a background job of this script: the run is started, then the script waits for your Enter ("the match is on screen") and drives
# HOME / lock / recents / the icon itself over adb. No timeouts: nothing goes on until you press Enter. Each run folder gets apk_version.txt and notes.txt.
# Run numbers: -First, -First+1, ... (a taken number is skipped). adb must show exactly one phone (run.ps1 checks).
#   1 Home and return in a match, released APK: black? (A1)       2 same on the new APK + lock screen, recents, 90 s background
#   3 tap the icon after Home: does the running game come back (A13)?   4 forced --gpu_plain_mode=on
#   5 wwe2k14_test_hang=guest with exit on                              6 a 120 Hz request holds
#   7 Chamber (chamber_6man) in a match with the phone's flags    (then, by hand: flip wwe2k14_android_park / wwe2k14_hang_exit only if [life] / HANG lines say so)
param([int]$First = 200, [int]$Only = 0, [int]$ChamberSeconds = 900)
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'
$run = $First
$hold = "$ProjectRoot\tools\input_scripts\android\s95_hold.txt"   # to the match scene, then stays (run.ps1 -Seconds ends it)
$chamber = "$ProjectRoot\tools\input_scripts\android\s95_chamber.txt"
$matchArgs = "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=139,103"
$phoneArgs = "--wwe2k14_d3d_owned=true --vulkan_texture_transcode=true --wwe2k14_android_thermal=true --wwe2k14_phys_alloc_fast_fail=true --wwe2k14_gpu_keep_warm=true " +
             "--wwe2k14_readback_char_window_by_load=true --wwe2k14_dynamic_res=true --gpu_scene_crowd_density=70 --wwe2k14_use_launcher_settings=false --gpu_scene_crowd_rank_diag=true"

function Adb { & $Adb @args }
function Wait-Enter([string]$what) { Write-Host ">>> $what"; [void](Read-Host "    (Enter when done)") }
function Note([string]$r, [string]$what) { $a = Read-Host ">>> $what"; Add-Content -Path "$($AndroidDirs.Runs)\$r\notes.txt" -Value "$what : $a" }
function Next-Run { while (Test-Path "$($AndroidDirs.Runs)\$('{0:D3}' -f $script:run)") { $script:run++ }; $n = '{0:D3}' -f $script:run; $script:run++; return $n }
function Version-Line { ((Adb shell dumpsys package $Pkg) | Select-String 'versionName' | Select-Object -First 1).ToString().Trim() }
function Save-Version([string]$r, [string]$expect) {
    $v = Version-Line
    $dir = "$($AndroidDirs.Runs)\$r"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Set-Content "$dir\apk_version.txt" "$v (expected: $expect)"
    Write-Host "installed: $v (expected: $expect)"
}
function Step([int]$n, [string]$title, [scriptblock]$body) {
    if ($Only -and $Only -ne $n) { return }
    Write-Host "== step ${n}: $title"
    & $body
}
# Starts run.ps1 in a background job (it blocks for -Seconds); the caller drives the phone meanwhile, then calls Finish-Job.
# (Save-Version made the run folder; run.ps1 refuses an existing one, so it is removed again just before the job starts - only apk_version.txt is in it, kept aside.)
function Start-Run([string]$r, [int]$secs, [int]$shots, [string]$route, [string]$scn, [string]$gargs) {
    $dir = "$($AndroidDirs.Runs)\$r"
    $ver = Get-Content "$dir\apk_version.txt" -ErrorAction SilentlyContinue
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    $job = Start-Job -ArgumentList $PSScriptRoot, $r, $secs, $shots, $route, $scn, $gargs -ScriptBlock {
        param($d, $r, $secs, $shots, $route, $scn, $gargs)
        & "$d\run.ps1" -Run $r -Seconds $secs -Shots $shots -Route $route -Scenario $scn -GameArgs $gargs
    }
    for ($i = 0; $i -lt 60 -and -not (Test-Path $dir); $i++) { Start-Sleep -Milliseconds 500 }   # run.ps1 makes the folder first thing
    if ($ver) { Set-Content "$dir\apk_version.txt" $ver }
    return $job
}
function Finish-Job($job) { Wait-Job $job | Out-Null; Receive-Job $job; Remove-Job $job }

Step 1 'Home and return in a match (RELEASED APK): is it black? (A1)' {
    $r = Next-Run
    Wait-Enter "The RELEASED APK must be installed now. Enter starts the run."
    Save-Version $r 'the released version'
    $job = Start-Run $r 420 4 $hold 'lesnar_bryan' $matchArgs
    Wait-Enter "Wait until the match is on screen (entrances count), then Enter: the script presses HOME."
    Adb shell input keyevent KEYCODE_HOME; Start-Sleep 10
    Write-Host "HOME pressed. Return to the game now (recents)."
    Note $r "Back in the game: is the picture black? (what you see)"
    Finish-Job $job
}
Step 2 'new APK: Home, lock screen, recents, 90 s in the background' {
    $r = Next-Run
    Wait-Enter "The NEW APK must be installed now. Enter starts the run."
    Save-Version $r 'the new version'
    $job = Start-Run $r 600 6 $hold 'lesnar_bryan' "$matchArgs $phoneArgs"
    Wait-Enter "Wait until the match is on screen, then Enter: HOME is pressed."
    Adb shell input keyevent KEYCODE_HOME; Start-Sleep 10
    Note $r "Return to the game. Back: picture and sound ok?"
    Adb shell input keyevent KEYCODE_SLEEP; Start-Sleep 8
    Write-Host "Screen locked. Unlock the phone yourself."
    Note $r "Unlocked: does the game come back (picture, sound)?"
    Adb shell input keyevent KEYCODE_APP_SWITCH; Start-Sleep 3
    Write-Host "Recents open. Tap the game's card."
    Note $r "Back from recents: picture and sound ok?"
    Adb shell input keyevent KEYCODE_HOME
    Write-Host "HOME pressed: 90 s in the background ..."; Start-Sleep 90
    Write-Host "Return to the game."
    Note $r "After 90 s in the background: picture ok? sound ok? (the sound check of the report's list)"
    Finish-Job $job
}
Step 3 'tap the icon after Home: does the running game come back? (A13)' {
    $r = Next-Run
    Wait-Enter "The NEW APK must be installed. Enter starts the run."
    Save-Version $r 'the new version'
    $job = Start-Run $r 420 4 $hold 'lesnar_bryan' "$matchArgs $phoneArgs"
    Wait-Enter "Wait until the match is on screen, then Enter: HOME, then the app icon is launched."
    Adb shell input keyevent KEYCODE_HOME; Start-Sleep 5
    Adb shell monkey -p $Pkg -c android.intent.category.LAUNCHER 1 | Out-Null; Start-Sleep 10
    Note $r "After the icon tap: the running game, a new start, or the launcher menu?"
    Finish-Job $job
}
Step 4 'forced plain mode' {
    $r = Next-Run
    & "$PSScriptRoot\run.ps1" -Run $r -Seconds 240 -Shots 3 -Route $hold -Scenario lesnar_bryan -GameArgs "$matchArgs $phoneArgs --gpu_plain_mode=on"
}
Step 5 'wwe2k14_test_hang=guest with exit on' {
    $r = Next-Run
    # (hang_guard.h: the guest is stopped at frame 600, before the match; the test is the hang guard, not the match)
    & "$PSScriptRoot\run.ps1" -Run $r -Seconds 150 -Shots 1 -Route $hold -Scenario lesnar_bryan -GameArgs "$matchArgs $phoneArgs --wwe2k14_test_hang=guest --wwe2k14_hang_exit=true"
}
Step 6 'a 120 Hz request holds' {
    $r = Next-Run
    & "$PSScriptRoot\run.ps1" -Run $r -Seconds 240 -Shots 1 -Route $hold -Scenario lesnar_bryan -GameArgs "$matchArgs $phoneArgs --wwe2k14_step_hz=120 --wwe2k14_step_hz_entrance=120 --wwe2k14_step_vblank=true --vulkan_allow_present_mode_immediate=false"
}
Step 7 'Chamber (6 men) on a phone' {
    $r = Next-Run
    & "$PSScriptRoot\run.ps1" -Run $r -Seconds $ChamberSeconds -Shots 3 -Route $chamber -Scenario chamber_6man -GameArgs "$phoneArgs --wwe2k14_launch=chamber_6man"
}
Write-Host "done: runs up to $('{0:D3}' -f ($run - 1)) under $($AndroidDirs.Runs); read each run's game log for [life], HANG and [s95][readback] lines (and the share report's READ-BACK AND CROWD section)."
