# One test run on the phone (ANDROID_PLAN.md step 5.2). T1-phone: start, watch logcat, screenshots, pull the logs, stop.
#   pwsh -File tools\android\run.ps1 -Run 001 -Seconds 60
#   pwsh -File tools\android\run.ps1 -Run 002 -GameArgs "--wwe2k14_preset_1440p60=false" -Seconds 120 -Shots 3
#   pwsh -File tools\android\run.ps1 -Run 003 -Until "title screen marker" -Seconds 180
#   pwsh -File tools\android\run.ps1 -PushGame      (once: the test ISO into the app's folder, 7.8 GB over USB)
# Every run: a fresh copy of the frozen user data (out\bench\userdata_frozen -> <app>/test_userdata) and a test settings
# file in the app folder (never the folders Jarrod plays with, CLAUDE.md "Safety"). The game's arguments go through
# <app>/args.txt (GameActivity reads and deletes it). Output: out\android\runs\NNN (logcat.txt, the game's log, shots).
# One phone run at a time: refused while the app runs. Phone runs do not take the PC run lock.
#   pwsh -File tools\android\run.ps1 -Run 029 -Route tools\input_scripts\android\bench_cena_taker.txt -Scenario lesnar_bryan
#     (-Route: an input script, pushed and passed as --wwe2k14_input_script; -Scenario NAME: out\scenarios\NAME.* pushed
#     to <app>/scenarios for --wwe2k14_launch; the route's own header gives the rest of its arguments)
param([string]$Run, [string]$GameArgs = '', [int]$Seconds = 60, [int]$Shots = 1, [string]$Until = '', [switch]$PushGame,
      [switch]$KeepRunning, [switch]$FreshShaders, [string]$Route = '', [string]$Scenario = '')
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'

function Adb { & $Adb @args }
$ready = @(Adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' })
if ($ready.Count -ne 1) { "run.ps1: need exactly one connected phone (adb devices: $($ready.Count) ready)"; exit 2 }

if ($PushGame) {
    $iso = "$ProjectRoot\game_data\iso\wwe2k14_test.iso"
    if (-not (Test-Path $iso)) { "run.ps1: no test ISO at $iso"; exit 2 }
    Adb shell mkdir -p "$PhoneFiles/game" | Out-Null
    "pushing $iso ({0:N1} GB) to $PhoneFiles/game/wwe2k14.iso ..." -f ((Get-Item $iso).Length / 1GB)
    Adb push $iso "$PhoneFiles/game/wwe2k14.iso"
    $code = $LASTEXITCODE
    Adb shell chmod -R a+rwX "$PhoneFiles/game" | Out-Null  # see the user data push below
    exit $code
}
if (-not $Run) { "run.ps1: -Run NNN is required"; exit 2 }

if ((Adb shell pidof $Pkg $GameProc) -match '\d') { "run.ps1: $Pkg is already running (one phone run at a time)"; exit 3 }
$out = "$($AndroidDirs.Runs)\$Run"
if (Test-Path $out) { "run.ps1: $out exists (pick a new run number)"; exit 2 }
[void](New-Item -ItemType Directory -Force $out)
Adb shell am force-stop $Pkg | Out-Null

# Fresh test user data + arguments
Adb shell rm -rf "$PhoneFiles/test_userdata" | Out-Null
Adb shell mkdir -p "$PhoneFiles" | Out-Null
Adb push "$ProjectRoot\out\bench\userdata_frozen" "$PhoneFiles/test_userdata" | Out-Null
# adb writes as the shell user (2000); the app (its own uid) may not read or write that (run 002: Permission denied on the
# ISO). Open what adb made to everyone: the folder is the app's own (no other app can reach Android/data/<package>).
Adb shell chmod -R a+rwX "$PhoneFiles/test_userdata" | Out-Null
# The shader storage outside the fresh test user data, so it survives between runs (run 010: every run rebuilt every
# shader: very low fps in the first match seconds). -FreshShaders starts it empty. The shipped shader list goes next to
# the "exe" (the app folder, src/platform/compat.h) like on the PC.
if ($FreshShaders) { Adb shell rm -rf "$PhoneFiles/cache" | Out-Null }
$list = "$ProjectRoot\db\shaders\shader_list.txt"  # the committed list (CLAUDE.md rule 1 exception)
if (Test-Path $list) { Adb push $list "$PhoneFiles/shader_list.txt" | Out-Null }
$argLines = @("--user_data_root=$PhoneFiles/test_userdata", "--wwe2k14_settings_file=$PhoneFiles/test_settings.json",
              "--game_data_root=$PhoneFiles/game/wwe2k14.iso", "--cache_root=$PhoneFiles/cache")
if ($Route) {
    Adb push (Resolve-Path $Route).Path "$PhoneFiles/route.txt" | Out-Null
    $argLines += "--wwe2k14_input_script=$PhoneFiles/route.txt"
}
if ($Scenario) {  # local-only copies of game memory (scenario.h): they go to Jarrod's own phone, nowhere else
    Adb shell mkdir -p "$PhoneFiles/scenarios" | Out-Null
    Get-ChildItem "$ProjectRoot\out\scenarios\$Scenario.*" | ForEach-Object { Adb push $_.FullName "$PhoneFiles/scenarios/" | Out-Null }
    Adb shell chmod -R a+rwX "$PhoneFiles/scenarios" | Out-Null
    $argLines += "--wwe2k14_scenario_dir=$PhoneFiles/scenarios"
}
if ($GameArgs.Trim()) { $argLines += ($GameArgs.Trim() -split '\s+(?=--)') }
$argLines | Set-Content "$out\args.txt" -Encoding utf8NoBOM
Adb push "$out\args.txt" "$PhoneFiles/args.txt" | Out-Null

# Logcat to a file while the run lasts
Adb logcat -c | Out-Null
$logcat = Start-Process -FilePath $Adb -ArgumentList 'logcat', '-v', 'threadtime' -RedirectStandardOutput "$out\logcat.txt" `
    -RedirectStandardError "$out\logcat_err.txt" -NoNewWindow -PassThru
$t0 = Get-Date
"run ${Run} started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'): $($argLines -join ' ')"
Adb shell am start -n "$Pkg/.GameActivity" | Out-Null

# Wait: the seconds, an -Until marker in logcat, or the app ending
$shotTimes = if ($Shots -gt 0) { 1..$Shots | ForEach-Object { [int]($Seconds * $_ / $Shots) } } else { @() }
$shot = 0; $ended = $false; $marker = $false
$ourPid = ''  # Session 74: the game process this run started (the end only stops that one)
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 2
    $el = ((Get-Date) - $t0).TotalSeconds
    if (-not $ourPid) { $ourPid = "$(Adb shell pidof $GameProc)".Trim() }
    if ($shot -lt $shotTimes.Count -and $el -ge $shotTimes[$shot] - 2) {
        $shot++
        Adb shell screencap -p /sdcard/wwe2k14_shot.png | Out-Null
        Adb pull /sdcard/wwe2k14_shot.png "$out\shot_$shot.png" | Out-Null
    }
    if ($Until -and (Select-String -Path "$out\logcat.txt" -SimpleMatch $Until -Quiet)) { $marker = $true; break }
    if (-not ((Adb shell pidof $GameProc) -match '\d') -and $el -gt 5) { $ended = $true; break }
}
if ($marker) {
    Adb shell screencap -p /sdcard/wwe2k14_shot.png | Out-Null
    Adb pull /sdcard/wwe2k14_shot.png "$out\shot_marker.png" | Out-Null
}
$elapsed = [int]((Get-Date) - $t0).TotalSeconds

# Collect and stop
Adb pull "$PhoneFiles/logs" "$out" 2>&1 | Out-Null
# Session 74: only while this run's own game is still the one running - a run started right after this one's game
# quit (while its logs were still being pulled) was killed here twice (phone runs 082, 096).
$nowPid = "$(Adb shell pidof $GameProc)".Trim()
if (-not $KeepRunning -and (-not $nowPid -or $nowPid -eq $ourPid)) { Adb shell am force-stop $Pkg | Out-Null }
elseif (-not $KeepRunning) { "run.ps1: another run's game is running (pid $nowPid) - not stopped" }
Start-Sleep -Milliseconds 500
Stop-Process -Id $logcat.Id -ErrorAction SilentlyContinue
Adb shell rm -f /sdcard/wwe2k14_shot.png | Out-Null

$why = if ($marker) { "marker '$Until' seen" } elseif ($ended) { 'the app ended by itself' } else { 'time up' }
"run ${Run}: $why after $elapsed s; output $out"
$lc = "$out\logcat.txt"
# A crash: the abort message and the top of the backtrace (frames up to SDL_main), shortened to the library + function.
Select-String -Path $lc -Pattern 'FATAL EXCEPTION|UnsatisfiedLinkError|dlopen failed|Fatal signal|Abort message|^.* F DEBUG   :\s+#(0\d|1[0-2]) pc' |
    Select-Object -First 20 | ForEach-Object {
        $s = $_.Line -replace '^.*F DEBUG   :\s+', '  ' -replace '/data/app/[^ ]*/lib/arm64/', '' -replace '\(BuildId: [0-9a-f]+\)', ''
        $s.Substring(0, [Math]::Min(260, $s.Length))
    }
"-- last wwe2k14 lines:"
Select-String -Path $lc -Pattern ' wwe2k14|SDL' | Select-Object -Last 15 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(250, $_.Line.Length)) }
