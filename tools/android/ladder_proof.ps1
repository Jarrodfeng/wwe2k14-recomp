# Session 95 (LADDER): the phone launcher's PC build with PLANTED run files. Each scenario has its own files folder under
# out\launcher_mobile\ladder\<name>; the launcher is started once per planted run (--shot ends it after 6 frames), and its log's "ladder:" lines
# are the answer. The last start of a scenario also saves a picture. Needs build_launcher.ps1 -Target pc first. 0 game runs.
#   pwsh -File tools\android\ladder_proof.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = "$root\out\launcher_mobile\pc\wwe2k14_launcher_mobile.exe"
if (-not (Test-Path $exe)) { "ladder_proof.ps1: build it first (build_launcher.ps1 -Target pc)"; exit 2 }
$base = "$root\out\launcher_mobile\ladder"
if (Test-Path $base) { Remove-Item $base -Recurse -Force }

$crash = "   0.1  step: logging up`n   1.5  step: first frame: the game is running`n  83.1  CRASH: SIGABRT (abort), code -1, at libc.so+0x1.`n"
$quit = "   0.1  step: logging up`n   1.5  step: first frame: the game is running`n 600.0  java: the player quit to the launcher`n"
$early = "   0.1  step: logging up`n   1.5  step: first frame: the game is running`n  40.0  java: the player quit to the launcher`n"
$unknown = "   0.1  step: logging up`n   1.5  step: first frame: the game is running`n"

function Start-Launcher([string]$Name, [string]$Shot) {
    $files = "$base\$Name"
    $out = if ($Shot) { $Shot } else { "$files\_discard.png" }
    $p = Start-Process $exe -ArgumentList @('--files', $files, '--window', '1584x720', '--scale', '1.75', '--page', '0', '--shot', $out, '--frames', '6') -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(30000)) { $p.Kill(); "  (timed out)" }
}
function Plant([string]$Name, [string]$Startup, [string]$Exit = '') {
    $logs = "$base\$Name\logs"
    [void](New-Item -ItemType Directory -Force $logs)
    Set-Content "$logs\startup.txt" $Startup -NoNewline
    if ($Exit) { Set-Content "$logs\last_game_exit.txt" $Exit -NoNewline } else { Remove-Item "$logs\last_game_exit.txt" -ErrorAction SilentlyContinue }
    (Get-Item "$logs\startup.txt").LastWriteTime = (Get-Date).AddMinutes(-10 * (1 + $script:tick++))   # a different run each time
    if ($Exit) { (Get-Item "$logs\last_game_exit.txt").LastWriteTime = (Get-Date) }
}
function Ladder([string]$Name) { Get-Content "$base\$Name\logs\launcher.log" | Select-String 'ladder:' | Select-Object -Last 3 | ForEach-Object { "    " + ($_.Line -replace '^\S+ ', '') } }
$script:tick = 0
$sig11 = "reason: SIGNALED`nstatus: 11`nimportance: 100`n"
$unkx = "reason: UNKNOWN`nstatus: 0`nimportance: 100`n"

"== A: two crashes in a row -> SAFE MODE step 1"
Plant A $crash $sig11; Start-Launcher A
Plant A $crash $sig11; Start-Launcher A "$base\A_safe_card.png"
Ladder A
"== B: crash, a normal play, crash -> no card"
Plant B $crash $sig11; Start-Launcher B
Plant B $quit; Start-Launcher B
Plant B $crash $sig11; Start-Launcher B "$base\B_none.png"
Ladder B
"== C: unknown ends (swipe-away) twice -> no card, nothing recorded"
Plant C $unknown 'reason: UNKNOWN\nstatus: 0\nimportance: 100\n'; Start-Launcher C
Plant C $unknown 'reason: UNKNOWN\nstatus: 0\nimportance: 100\n'; Start-Launcher C "$base\C_none.png"
Ladder C
"== D: an early quit -> 'did the picture look right?'"
Plant D $early; Start-Launcher D "$base\D_early.png"
Ladder D
"== E: a rung file for another version is ignored (starts clean)"
[void](New-Item -ItemType Directory -Force "$base\E\logs")
Set-Content "$base\E\rung.txt" "rung 2`nold-version`n`nmediump_changed 0`n"
Plant E $quit; Start-Launcher E
Ladder E
"== F: a rung file for this version is used"
[void](New-Item -ItemType Directory -Force "$base\F\logs")
Set-Content "$base\F\rung.txt" "rung 2`nandroid-v0.2.5`n`nmediump_changed 0`n"
Plant F $quit; Start-Launcher F
Ladder F
"history files:"; Get-ChildItem "$base\*\logs\run_history.txt" | ForEach-Object { "  $($_.Directory.Parent.Name): $((Get-Content $_).Count) row(s)" }
