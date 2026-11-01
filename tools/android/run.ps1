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
# Session 77 (XDET_PLAN.md stage A, the phone side of tools\xdet\run_stageA.ps1):
#   pwsh -File tools\android\run.ps1 -Run 120 -Replay xdet_cena_taker_a -StageA -Route tools\xdet\cena_taker_a.txt -Seconds 900 -Shots 0
#     -Replay NAME: out\replays\NAME.rec pushed to <app>/replays and played (--wwe2k14_replay_play), the det_serial
#     scheduler trace and the det hash files in <app>/logs/det (emptied first; pulled with the logs). -StageA: the flags
#     of tools\xdet\stageA.flags (the PC's), raw dumps every 60 frames (Jarrod's phone only: game memory stays local).
#     Then: py tools\determinism\det_compare.py --cross <PC run> out\android\runs\NNN
param([string]$Run, [string]$GameArgs = '', [int]$Seconds = 60, [int]$Shots = 1, [string]$Until = '', [switch]$PushGame,
      [switch]$KeepRunning, [switch]$FreshShaders, [string]$Route = '', [string]$Scenario = '', [string]$Replay = '',
      [switch]$StageA, [switch]$Played, [switch]$NoDraw, [string]$Data = 'userdata_frozen')
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
# Session 77 (P7): -Data NAME = another save copy under out\bench (default userdata_frozen; userdata_empty = an empty
# folder, the tester check's fresh save: made on the phone, nothing pushed).
$dataSrc = "$ProjectRoot\out\bench\$Data"
if (-not (Test-Path $dataSrc)) { "run.ps1: no $dataSrc"; exit 2 }
Adb shell rm -rf "$PhoneFiles/test_userdata" | Out-Null
Adb shell mkdir -p "$PhoneFiles" | Out-Null
if (Get-ChildItem $dataSrc -Force | Select-Object -First 1) {
    Adb push $dataSrc "$PhoneFiles/test_userdata" | Out-Null
} else {
    Adb shell mkdir -p "$PhoneFiles/test_userdata" | Out-Null
}
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
if ($Replay) {  # Session 77: our own controller recordings (CLAUDE.md rule 1 exception, 2026-10-31)
    $rec = "$ProjectRoot\out\replays\$Replay.rec"
    if (-not (Test-Path $rec)) { "run.ps1: no $rec"; exit 2 }
    Adb shell mkdir -p "$PhoneFiles/replays" | Out-Null
    Adb push $rec "$PhoneFiles/replays/$Replay.rec" | Out-Null
    Adb shell rm -rf "$PhoneFiles/logs/det" | Out-Null
    Adb shell mkdir -p "$PhoneFiles/logs/det" | Out-Null
    Adb shell chmod -R a+rwX "$PhoneFiles/replays" "$PhoneFiles/logs/det" | Out-Null
    $argLines += @("--wwe2k14_replay_dir=$PhoneFiles/replays", "--wwe2k14_replay_play=$Replay",
                   "--det_serial_trace=$PhoneFiles/logs/det", "--wwe2k14_det_hash_dir=$PhoneFiles/logs/det")
}
if ($StageA -or $Played) {
    # Session 77: -Played = tools\xdet\played.flags (the "as played" config, Jarrod 2026-10-31 ~11:10), and both
    # fast-forward the phone like the PC's test runs (--vsync=false: host-only; at the default 60 Hz pacing the phone
    # took 33 s to the main menu, 3.9 s without it, runs 128 / 129). -GameArgs '--vsync=true' gives normal speed.
    $file = if ($Played) { 'played.flags' } else { 'stageA.flags' }
    $argLines += '--vsync=false'
    $argLines += @(Get-Content "$ProjectRoot\tools\xdet\$file" | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
                   ForEach-Object { $_.Trim() })
    $argLines += '--wwe2k14_det_hash_raw_every=60'
}
# Session 77: -NoDraw = no host draws or copies (tools\xdet\run_stageA.ps1 -NoDraw; phone 140: match 36 s vs 48 s,
# = PC 4924 in every layer, the scheduler included).
if ($NoDraw) { $argLines += '--gpu_det_skip_host_draws=true' }
if ($GameArgs.Trim()) { $argLines += ($GameArgs.Trim() -split '\s+(?=--)') }
# Session 77: a flag given twice is a CLI11 parse error that drops every startup flag after it (user_data_root too):
# the last of each name wins (tools\xdet\flags.ps1).
. "$ProjectRoot\tools\xdet\flags.ps1"
$argLines = @(Merge-Flags @() $argLines)
$argLines | Set-Content "$out\args.txt" -Encoding utf8NoBOM
Adb push "$out\args.txt" "$PhoneFiles/args.txt" | Out-Null

# Logcat to a file while the run lasts
Adb logcat -c | Out-Null
$logcat = Start-Process -FilePath $Adb -ArgumentList 'logcat', '-v', 'threadtime' -RedirectStandardOutput "$out\logcat.txt" `
    -RedirectStandardError "$out\logcat_err.txt" -NoNewWindow -PassThru
$t0 = Get-Date
"run ${Run} started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'): $($argLines -join ' ')"
# Session 77 (run 145): who owns the phone now. When the game dies, Android relaunches its activity - WITHOUT this
# run's arguments (GameActivity deleted args.txt), so on the default user data. A different game process at the end
# is a relaunch while this marker is still ours; a newer run.ps1 (started once our game ended) rewrites it.
$owner = "$Run $PID $([guid]::NewGuid())"
$ownerFile = "$AndroidOut\phone_run_owner.txt"
Set-Content -Path $ownerFile -Value $owner -Encoding ascii
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
    $p = "$(Adb shell pidof $GameProc)".Trim()
    if (-not ($p -match '\d') -and $el -gt 5) { $ended = $true; break }
    # Session 77: our game died and Android relaunched it already (another pid, no arguments): stop it at once.
    if ($ourPid -and $p -match '\d' -and $p -ne $ourPid) {
        Adb shell am force-stop $Pkg | Out-Null
        "run.ps1: the game (pid $ourPid) ended and Android relaunched it without arguments (pid $p): stopped"
        $ended = $true; break
    }
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
$stillOurs = (Get-Content $ownerFile -ErrorAction SilentlyContinue) -eq $owner
if (-not $KeepRunning -and (-not $nowPid -or $nowPid -eq $ourPid)) { Adb shell am force-stop $Pkg | Out-Null }
elseif (-not $KeepRunning -and $stillOurs) {
    # Session 77 (run 145): no newer run took the phone, so this is Android's relaunch of our dead game (no arguments).
    Adb shell am force-stop $Pkg | Out-Null
    "run.ps1: the game was relaunched by Android after it ended (pid $nowPid, no arguments): stopped"
}
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
