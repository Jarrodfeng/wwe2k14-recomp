# Session 79 (HD 520 arc, item 3): the first Surface session's bench in one command, run ON the Surface (over SSH or at
# its desktop), from a folder holding a build of the game (a release / preview zip unpacked: wwe2k14.exe + DLLs). It
# never touches the PC's run lock. Plugged in, best-performance power mode (Jarrod). Uses only Jarrod's own game files
# (-GameData) and a copy of the PC's frozen user data (-UserData: the save the replays were recorded on).
#   pwsh -File bench.ps1 -Exe C:\wwe\wwe2k14.exe -GameData D:\wwe2k14 -UserData C:\wwe\userdata_frozen
#   (Session 80: Windows PowerShell 5.1 works too - the Surface has no pwsh: powershell -ExecutionPolicy Bypass -File ...)
#        -Routes C:\wwe\routes -Replays C:\wwe\replays -Out C:\wwe\bench_out
#        [-Only passes,passes_ram,multi,arc,frames,mul,cold,levers,probes,pinned]
# Steps (each one game run; logs land in the game's logs folder, copied to -Out\<step>):
#   passes  per-pass GPU log at 720p on the native frame (s72_gap_log.txt + the s21_lb replay, fast-forwarded):
#           the Intel per-pass split (read on the PC: tools\native\pass_roles.py / pass_log.py)
#   frames  frame times at NORMAL speed, 60 fps, true 720p, native frame on (profile_lesnar_bryan route, launched):
#           against 16.7 ms per scene (tools\bench\bench_report.py)
#   frames_bryan / frames_brock / frames_match (Session 89, ask for them with -Only): one scene each, normal speed,
#           fixed seed 1, quits at the scene's end (tools\input_scripts\scenes\)
#   frames_bryan_heavy (S102 M6, also -AbScene): Bryan's heavy stretch only, tron+760 .. +1330 (scenes\bryan_heavy.txt header: why)
#   ab      (Session 95, heat aid; ask for it with -Only ab) INTERLEAVED A/B on a hot machine: arm A, arm B, A, B ... with a
#           fixed gap and NO cool-down, so both arms see the same heat (the fanless Surface drifts 35% in 8 minutes: Bryan GPU
#           23.3 -> 31.5 ms over six sequential runs, out\s95\step0.md). One scene per run (-AbScene frames_bryan|frames_brock|
#           frames_match, default frames_match). Arm A = the scene's flags; arm B = the same plus -ArmBFlags (cvars) and/or -ArmBExe
#           (another build's wwe2k14.exe: PGO, a branch). -AbRounds N pairs (default 2 = A B B A: the arm order alternates per round so the heat drift cancels), -AbGapSeconds N between runs
#           (default 20), -AbWarmup adds one discarded A first (the machine starts hot, not cold). Steps land in Out\ab_<n>_A|B;
#           Out\ab_order.txt lists the order and the start times. Read each pair on the PC (frames_fps.py) and compare the A and B
#           means of the SAME rounds; a drift down A1 -> A2 is heat, not the arm. The fixed gap is the only pacing: a temperature
#           or clock gate is not built (the Surface's counter names are unconfirmed).
#   mul     the Intel multiply rule: the same frames run with --wwe2k14_intel_exact_mul=0 (exact rule off): its cost
#   cold    first launch with our shader cache emptied (a fresh -UserData copy): shader preparation time. The driver's
#           own cache (%LOCALAPPDATA%Low\Intel\ShaderCache) is moved aside ONLY with -ColdDriver (Jarrod's call), and
#           moved back afterwards - nothing is deleted.
# Then: Out\surface_bench_<stamp>.zip (logs only: no pictures, no game data) for the PC.
# S102 S0-1: -Profile auto|standard_player|release_day picks the settings file the game runs (profiles\<name>.json beside this script; tools\surface\profile_check.py
# proves them). auto (the default) = what a fresh install on integrated graphics writes: the AUTO tier resolving to LOW, 720p, crowd 70 %, LOW's
# levers on (src\gfx_applied.h FreshInstall; the bench measures at 60 fps, a fresh install writes 30: that also leaves out entrance_60fps=false, step_hz 30, vblank spin 250,
# direct texture loads). standard_player = the same install after the player picks STANDARD (true_native on, no LOW levers). release_day = release day's file byte for byte (no settings_version / true_native: no D3D-layer stack, no LOW levers), kept for continuity. Every run's log carries the game's own `[gfx][applied]` line (tier, render scale, internal
# size, crowd, levers on / asked but ignored); -DryRun (PC only, no game, no ssh) writes the file and prints the line the game would log
# for it (out\tools\gfx_applied.exe, built by build_gfx_applied.ps1), with the Surface's panel size (-Panel) as the swapchain.
#   pwsh -File bench.ps1 -Profile auto -DryRun -Out out\s102\bench_dry
# (The six paths below are required unless -DryRun.)
[CmdletBinding()]  # (review A: a misspelt bench_args key must throw, not run silently)
param([string]$Exe = '', [string]$GameData = '', [string]$UserData = '', [string]$Routes = '', [string]$Replays = '', [string]$Out = '',
      [ValidateSet('auto', 'standard_player', 'release_day', 'auto_dh540', 'auto_dh576', 'auto_dh600', 'auto_dh648')][string]$Profile = 'auto', [switch]$DryRun, [string]$Panel = '2736x1824',
      # (Session 80: the --wwe2k14_launch=lesnar_bryan scenario's folder; the game's default is ..\..\scenarios from the
      # exe, which only a build inside the repo has - out\scenarios.)
      [string]$Scenarios = '',
      # (Session 80: flags added to every step's game, without their leading -- (pwsh -File takes a value starting with
      # -- for a parameter name), e.g. -GameFlags wwe2k14_shader_list=X for a dev build folder, which has no
      # shader_list.txt next to the exe.)
      [string[]]$GameFlags = @(),
      # (Session 80, Jarrod: skip most of each entrance for A/Bs) the levers' and probes' route; profile_lesnar_bryan.txt
      # plays both entrances in full.
      [string]$AbRoute = 's80_probe_short.txt',
      # (Session 80) the multi step's in-run route (tools\native\passab_multi.py writes them), e.g. s80_owncrowd_multi.txt.
      [string]$MultiRoute = 's80_scene_multi.txt',
      [string[]]$Only = @('passes', 'frames', 'mul', 'cold'), [switch]$ColdDriver, [int]$TimeoutSeconds = 1500,
      [string[]]$Levers = @(),
      # (Session 80, 2026-11-02 10:10: a game started by the scheduled task is not let to the foreground by Windows, so
      # the taskbar stays over its borderless fullscreen window and the desktop compositor composes the 2736x1824
      # screen every refresh - ~3.3 ms of GPU time inside ~2 frames in 3 (runs 033-036, dwm.exe 7-13% of the 3D
      # engine). -Front puts the game's window on top and in the foreground once it shows, as a player's launch does.)
      [switch]$Front,
      # (Session 95, warm-60 work: Power=true in bench_args.txt) power_log.ps1 beside this script samples the RAPL powers,
      # CPU utility, thermal zones and the 3D engine once a second into <step>\power.csv (read: tools\surface\power_read.py).
      [switch]$Power,
      # (Session 102 lane S0-2: power logging is ON for every step now (-Power is kept, a no-op); NoPower=true in bench_args.txt turns it off.
      # A scene step (thread-sampled) is tagged steady / not steady from its own power.csv by heat_steady.ps1; a run outside the band is
      # moved to <step>_rejected and repeated once.)
      [switch]$NoPower,
      # (Session 102, 08:34, R14: NoThreads=true in bench_args.txt skips thread_busy.ps1, the once-a-second thread sampler,
      # to measure what it costs the 1 % low; the heat verdict still runs.)
      [switch]$NoThreads,
      # (Session 95: the ab step, see above.)
      [string]$AbScene = 'frames_match', [string[]]$ArmAFlags = @(), [string[]]$ArmBFlags = @(), [string]$ArmBExe = '', [int]$AbRounds = 2,
      [int]$AbGapSeconds = 5, [switch]$AbWarmup,
# S102 M5: -ArmFlags 'A:flags|g2:flags|g3:flags' = several B arms in one job (flags split by ',', arms by '|'; arm A is required). -AbPlan prints the run order
# and folder names without starting anything (and refuses an order where an arm always runs last or a B arm has no A neighbour in a round); -AbPlanOrder
# 'A,g2,A,g3;g3,A,g2' checks a hand-made order (rounds split by ';') instead of the generated one.
[string[]]$ArmFlags = @(), [switch]$AbPlan, [string]$AbPlanOrder = '',
# S102 S0-5: cvars added to every per-scene step (frames_bryan / _brock / _match), comma-separated like -ArmBFlags, e.g.
# SceneFlags=wwe2k14_rdr_ko_draw_none=true in bench_args.txt for one knock-out job (each job = its own zip).
[string[]]$SceneFlags = @(),
# S102 H3: FrameSplit=true in bench_args.txt adds --wwe2k14_frame_split=true to every per-scene step (frames_bryan / _brock / _match, and the ab arms): seven more
# columns at the end of frames_NNN.csv, the game's swap thread split per frame (src\frame_split_math.h; read with bench_line.py --split / ab_line.py --split).
[switch]$FrameSplit,
# S102 M7: AbFlip=<spec> in bench_args.txt (spec format: src\ab_flip_math.h; characters A-Z a-z 0-9 _ = + / only) = the in-run A/B (--wwe2k14_ab_flip). With a scene step
# (frames_bryan_heavy ...) the run flips on that schedule. With Only=ab the job is ONE COMPLEMENTARY PAIR per round: arm A runs the spec, arm B the same spec plus
# /first=on (so every guest frame is measured both ways); read with  flip_line.py <ab_1_A> --pair <ab_1_B>. Not with ArmFlags / ArmBFlags / ArmBExe.
[string]$AbFlip = '',
# S102 B1 (R28, the Surface's power budget): the Surface opens every run in a power burst (about 145 % cpu_perf, 20-26 W, from an energy credit of 200-235 J that 50+ s idle
# refills; it ends 10-40 s into a run) and the bar must be measured after it. Warm=SECONDS in bench_args.txt: before the first measured scene step, run the first selected scene's
# own route as a discarded load (folders warm_1, warm_2 ... , same flags as the measured steps, never read as a number) until SECONDS of wall time have gone by: the burst is spent
# and the measured steps start from a drawn-down credit. No new route is needed: a scene run is 40-50 s, Warm=60 is two of them. Repeats=N: every selected scene step runs N times,
# round-robin (bryan, brock, match, bryan, ...), the first in frames_<scene>, the others in frames_<scene>_r2 ... _rN; tools\surface\bar_report.py prints median and range per scene
# (and which repeats were SUSTAINED, burst_rules.py). Both are for the scene steps (frames_bryan / _brock / _match / _bryan_heavy), not ab.
[int]$Warm = 0, [int]$Repeats = 1,
# S105 SRJ: Only=route runs ONE input-script route (Route=<file in Routes>, e.g. s103_moveset_stall.txt) Repeats times (1-9), with the job's GameFlags added to the
# step's own flags (GameFlags=a=b,c=d in bench_args.txt, no leading --, comma separated; the route's '# harness: flags' header is NOT read here: put them in GameFlags).
# Steps route, route_r2 .. route_rN, each with the frozen user data copied fresh, TimeoutSeconds as the hard per-run kill, the game's logs (log / csv / txt, so
# flight_*.txt too) and its new captures\*.png (screenshots) copied into the step folder; Front=true puts the window in front as before.
[string]$Route = '')
# (-Levers a,b: only those levers / probes and their base run, e.g. -Only levers -Levers no_alias_direct,aniso_4x.)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })  # (pwsh -File: one string)
$Levers = @($Levers | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
function Select-Levers($table) {
  if (-not $Levers.Count) { return @($table.Keys) }
  $unknown = @($Levers | Where-Object { -not $table.Contains($_) })
  if ($unknown.Count) { Write-Host "[surface] not a lever / probe here: $($unknown -join ', ')" }
  return @($table.Keys | Where-Object { $_ -eq 'base' -or $Levers -contains $_ })
}
# S102 M5: the multi-arm ab plan. Arms: ordered name -> flag list (arm A required). Each round runs every B arm once and A ceil(k/2) times, laid out
# b1 A b2 | b3 A b4 | ... (an odd last B gets "A b" before it), so every B arm has an A right beside it; the B list is rotated right by one per round, so the
# arm that runs last changes every round (k = 1 keeps the old A B / B A alternation).
function Get-AbArmSpec([string[]]$Raw) {
  $spec = [ordered]@{}
  foreach ($chunk in @($Raw | ForEach-Object { $_ -split '\|' } | Where-Object { $_.Trim() })) {
    $n, $f = $chunk -split ':', 2
    $n = $n.Trim()
    if ($n -notmatch '^[A-Za-z0-9]{1,20}$' -or ($n -match '^A\d+$')) { throw "ab: bad arm name '$n' in -ArmFlags (letters and digits, 1-20; A2, A3 ... are the base's repeat runs)" }
    if ($spec.Contains($n)) { throw "ab: arm '$n' twice in -ArmFlags" }
    $spec[$n] = @(@($f -split ',') | Where-Object { $_ -and $_.Trim() } | ForEach-Object { '--' + $_.Trim().TrimStart('-') })
  }
  if (-not $spec.Contains('A')) { throw "ab: -ArmFlags needs an arm named A (the base)" }
  if ($spec.Count -lt 2) { throw 'ab: -ArmFlags needs at least one arm besides A' }
  return $spec
}
function Get-AbPlan([string[]]$Names, [int]$Rounds, [bool]$Warmup) {
  $b = @($Names | Where-Object { $_ -ne 'A' }); $k = $b.Count
  $seq = @()
  if ($Warmup) { $seq += , @('abwarm', 'A') }
  for ($r = 1; $r -le $Rounds; $r++) {
    $rot = ($r - 1) % $k
    $bb = @(); for ($i = 0; $i -lt $k; $i++) { $bb += $b[(($i - $rot) % $k + $k) % $k] }
    $lay = @()
    for ($i = 0; $i -lt $k; $i += 2) {
      if ($i + 1 -lt $k) { $lay += $bb[$i]; $lay += 'A'; $lay += $bb[$i + 1] } else { $lay += 'A'; $lay += $bb[$i] }
    }
    if ($k -eq 1 -and $r % 2 -eq 0) { [array]::Reverse($lay) }
    $na = 0
    foreach ($a in $lay) { $fa = $a; if ($a -eq 'A') { $na++; if ($na -gt 1) { $fa = "A$na" } }; $seq += , @("ab_${r}_$fa", $a) }   # a round's 2nd A run is ab_<r>_A2 ...
  }
  return $seq
}
# $Rounds: array of arrays of arm names (warm-up excluded). Returns a list of refusal strings (empty = fine).
function Test-AbOrder($Rounds, [string[]]$Names) {
  $bad = @(); $b = @($Names | Where-Object { $_ -ne 'A' }); $n = 0
  foreach ($rd in $Rounds) {
    $n++; $rd = @($rd)
    foreach ($x in $b) {
      $c = @($rd | Where-Object { $_ -eq $x }).Count
      if ($c -ne 1) { $bad += "round ${n}: arm $x runs $c times (want 1)"; continue }
      $ix = [array]::IndexOf($rd, $x)
      $nb = @($(if ($ix -gt 0) { $rd[$ix - 1] } else { '' }), $(if ($ix -lt $rd.Count - 1) { $rd[$ix + 1] } else { '' }))
      if ($nb -notcontains 'A') { $bad += "round ${n}: arm $x has no A beside it" }
    }
    if (@($rd | Where-Object { $_ -eq 'A' }).Count -lt 1) { $bad += "round ${n}: no A" }
  }
  if (@($Rounds).Count -ge 2) {
    $lasts = @($Rounds | ForEach-Object { @($_)[-1] } | Select-Object -Unique)
    if ($lasts.Count -eq 1) { $bad += "arm $($lasts[0]) runs last in every round (the heat drift would land on it)" }
  }
  return $bad
}
if ($AbPlan) {
  $names = if ($AbPlanOrder) { @($AbPlanOrder -split '[;,]' | Where-Object { $_ } | Select-Object -Unique) } elseif ($ArmFlags.Count) { @((Get-AbArmSpec $ArmFlags).Keys) } else { @('A', 'B') }
  if ($AbPlanOrder) { $rounds = @($AbPlanOrder -split ';' | ForEach-Object { , @($_ -split ',' | Where-Object { $_ }) }) }
  else { $rounds = @(); $sq = Get-AbPlan $names $AbRounds $false; for ($r = 1; $r -le $AbRounds; $r++) { $rounds += , @($sq | Where-Object { $_[0] -like "ab_${r}_*" } | ForEach-Object { $_[1] }) } }
  if ($AbWarmup -and -not $AbPlanOrder) { Write-Host 'run abwarm (A, discarded)' }
  $r = 0; foreach ($rd in $rounds) { $r++; $na = 0; Write-Host ("round ${r}: " + (($rd | ForEach-Object { $f = $_; if ($_ -eq 'A') { $na++; if ($na -gt 1) { $f = "A$na" } }; "ab_${r}_$f" }) -join ' ')) }
  $bad = Test-AbOrder $rounds $names
  $nRuns = (@($rounds | ForEach-Object { @($_).Count } | Measure-Object -Sum).Sum) + $(if ($AbWarmup) { 1 } else { 0 })
  Write-Host "plan: $($names.Count) arms, $($rounds.Count) rounds, $nRuns runs"
  if ($bad.Count) { $bad | ForEach-Object { Write-Host "REFUSED: $_" }; exit 1 }
  Write-Host 'plan ok'
  exit 0
}
if (-not $DryRun) {
  foreach ($n in 'Exe', 'GameData', 'UserData', 'Routes', 'Replays', 'Out') { if (-not (Get-Variable $n -ValueOnly)) { throw "bench.ps1: -$n is required (only -DryRun runs without it)" } }
} elseif (-not $Out) { throw 'bench.ps1: -DryRun needs -Out (a folder under out\)' }
$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$exeDir = if ($Exe) { Split-Path -Parent $Exe } else { '' }
$logs = if ($exeDir) { Join-Path $exeDir 'logs' } else { '' }
New-Item -ItemType Directory -Force $Out | Out-Null
# S102 S0-1: the profile's file, copied byte for byte (a test file: the game gets it with --wwe2k14_settings_file, never the player's own).
$profileFile = Join-Path (Join-Path $PSScriptRoot 'profiles') "$Profile.json"
if (-not (Test-Path $profileFile)) { throw "bench.ps1: profile file missing: $profileFile (push_kit.ps1 sends tools\surface\profiles)" }
$settings = Join-Path $Out "settings_$Profile.json"
Copy-Item -Force $profileFile $settings
Write-Host "[surface] profile $Profile : $settings"
if ($DryRun) {
  $cli = Join-Path $PSScriptRoot '..\..\out\tools\gfx_applied.exe'
  if (-not (Test-Path $cli)) { & (Join-Path $PSScriptRoot 'build_gfx_applied.ps1') }
  $cli = (Resolve-Path (Join-Path $PSScriptRoot '..\..\out\tools\gfx_applied.exe')).Path
  & $cli --settings $settings --gpu integrated --gpu-name 'Intel(R) UHD Graphics 620' --panel $Panel
  if ($LASTEXITCODE -ne 0) { throw "gfx_applied failed ($LASTEXITCODE)" }
  return
}

# (Session 80, -Front: see the parameter.) Waits for the game's window, makes it topmost (above the taskbar) and the
# foreground window (an Alt press first: Windows lets a process that just had input move the foreground), and says
# whether it is the foreground window afterwards.
function Set-GameFront($proc, [string]$name) {
  if (-not ('WweBench.User32' -as [type])) {
    Add-Type -Namespace WweBench -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
  }
  $deadline = (Get-Date).AddSeconds(120)
  while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
  }
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { Write-Host "[surface] $name : front: no window"; return }
  Start-Sleep -Seconds 2  # (the window goes fullscreen right after it shows)
  [void][WweBench.User32]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040)  # TOPMOST, NOSIZE NOMOVE SHOWWINDOW
  [WweBench.User32]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)  # Alt down
  [void][WweBench.User32]::SetForegroundWindow($h)
  [WweBench.User32]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)  # Alt up
  Start-Sleep -Milliseconds 500
  $fg = [WweBench.User32]::GetForegroundWindow() -eq $h
  Write-Host "[surface] $name : front: topmost, foreground $fg"
}

# (Session 79, 04:30: -Threads samples every thread's CPU time once a second with thread_busy.ps1 - copied next to this
# script - into <step>\threads.csv; -Affinity limits the game to those logical processors, e.g. 0xF = 2 cores / 4
# threads, the i3-6100U's count. Read on the PC: tools\perf\thread_scenes.py <threads.csv> <the step's log>.)
# (Session 102 lane S0-2: the heat rule, stale-file clean-up and retry decision are functions in heat_steady.ps1 next to this script, so they can be tested.)
$heatLib = Join-Path $PSScriptRoot 'heat_steady.ps1'
$haveHeat = Test-Path $heatLib
if ($haveHeat) { . $heatLib }
$script:PrevPower = ''
function Invoke-Step([string]$name, [string]$route, [string[]]$flags, [bool]$freshData, [bool]$Threads = $false,
                     [int64]$Affinity = 0, [bool]$Retry = $false, [string]$FirstTry = '') {
  # (Session 95: the ab step's arm B may come from another build folder.)
  $Exe = if ($script:StepExe) { $script:StepExe } else { $Exe }
  $exeDir = Split-Path -Parent $Exe
  $logs = Join-Path $exeDir 'logs'
  $work = Join-Path $Out "userdata_$name"
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }   # our own scratch copy only
  Copy-Item -Recurse $UserData $work
  if ($freshData) { $cache = Join-Path $work 'cache'; if (Test-Path $cache) { Remove-Item -Recurse -Force $cache } }
  $before = @(Get-ChildItem $logs -Filter 'wwe2k14_*.log' -ErrorAction SilentlyContinue | ForEach-Object Name)
  $gameArgs = @("--game_data_root=$GameData", "--user_data_root=$work", "--cache_root=$(Join-Path $work 'cache')",
            "--wwe2k14_settings_file=$settings", '--wwe2k14_rdr_start_failure_dialog=false', '--wwe2k14_use_launcher_settings=true',
            "--wwe2k14_replay_dir=$Replays", "--wwe2k14_input_script=$(Join-Path $Routes $route)",
            $(if ($Scenarios) { "--wwe2k14_scenario_dir=$Scenarios" }),
            '--wwe2k14_timeline=true', '--wwe2k14_native_frame=true') + $flags +
              @($GameFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })
  $gameArgs = @($gameArgs | Where-Object { $_ })
  # S102 main (2026-11-15 00:3x): one copy of each switch, the LATER one wins (job flags override the step's own). A string switch given twice
  # makes the SDK's CLI11 parse throw, and every switch registered after it is then silently dropped (stderr only): the stacks step hard-codes
  # --wwe2k14_profile_threads and the profile jobs passed it again, so --wwe2k14_launch / scenario_dir / every rdr flag were lost and the route
  # timed out at the menu (prof2 00:04, upd1 20:52). Found by reading cvar.cpp Init + CLI11's MultiOptionPolicy::Throw.
  $seenArg = @{}; $keep = New-Object System.Collections.Generic.List[string]; $dropped = @()
  for ($k = $gameArgs.Count - 1; $k -ge 0; $k--) {
    $key = ($gameArgs[$k] -replace '=.*$', '').ToLowerInvariant()
    if ($seenArg.ContainsKey($key)) { $dropped += $gameArgs[$k]; continue }
    $seenArg[$key] = $true; $keep.Insert(0, $gameArgs[$k])
  }
  if ($dropped.Count) { Write-Host "[surface] $name : duplicate switches removed (the later one kept): $($dropped -join ' ')" }
  $gameArgs = @($keep)
  Write-Host "[surface] $name : $route $($flags -join ' ')$(if ($Affinity) { ' (affinity 0x{0:X})' -f $Affinity })"
  $dest = Join-Path $Out $name
  # S102 main (2026-11-14 20:20): a step folder was reused across jobs and never cleared, so every arm folder carried older runs of the same
  # name (ab_2_B held runs from 2026-11-07 / 11-13 / 11-14 morning). bench_line reads the newest run, so a step that failed to write its csv
  # would have reported an OLD run as this job's number. Earlier jobs' files were already fetched in their own zips: clear the step's files.
  if (Test-Path $dest) {
    $old = @(Get-ChildItem $dest -File -ErrorAction SilentlyContinue)
    if ($old.Count) { $old | Remove-Item -Force -ErrorAction SilentlyContinue; Write-Host "[surface] $name : cleared $($old.Count) files of earlier jobs from the step folder" }
  }
  New-Item -ItemType Directory -Force $dest | Out-Null
  # (Session 102 lane S0-2: a power.csv / counters.txt / verdict from an earlier run of this step is removed BEFORE the run, so a run can
  # never carry another run's power file; the old R5 finding: files from 6 days earlier sat beside the new log and looked current.)
  # (threads.csv too: thread_busy.ps1 appends, and a second header row breaks tools\perf\thread_scenes.py.)
  if ($haveHeat) { foreach ($stale in (Clear-StaleStepFiles $dest)) { Write-Host "[surface] $name : removed stale $stale" } }
  $usePower = -not $NoPower
  $busy = $null
  $busyScript = Join-Path $PSScriptRoot 'thread_busy.ps1'
  if ($Threads -and $NoThreads) {
    Write-Host "[surface] $name : NoThreads: no thread sampler (S102 R14)"
  } elseif ($Threads -and (Test-Path $busyScript)) {
    # (Session 80: the PowerShell running this script, pwsh or Windows PowerShell.)
    $busy = Start-Process (Get-Process -Id $PID).Path -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                               $busyScript, '-Out', (Join-Path $dest 'threads.csv'),
                                               '-WaitForStart', '120') -PassThru -WindowStyle Hidden
  } elseif ($Threads) {
    Write-Host "[surface] $name : thread_busy.ps1 not next to bench.ps1 - no thread times"
  }
  $pwr = $null
  $pwrScript = Join-Path $PSScriptRoot 'power_log.ps1'
  if ($usePower -and (Test-Path $pwrScript)) {
    $pwr = Start-Process (Get-Process -Id $PID).Path -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                               $pwrScript, '-Out', (Join-Path $dest 'power.csv'), '-WaitForStart', '120',
                                               '-MaxSeconds', ($TimeoutSeconds + 120)) -PassThru -WindowStyle Hidden
  } elseif ($usePower) { Write-Host "[surface] $name : power_log.ps1 not next to bench.ps1 - no power log" }
  $t0 = Get-Date
  $p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory $exeDir -PassThru
  if ($Affinity) { try { $p.ProcessorAffinity = [IntPtr]$Affinity } catch { Write-Host "[surface] affinity not set: $_" } }
  if ($Front) { Set-GameFront $p $name }
  $timedOut = $false
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { $p.Kill(); $timedOut = $true; Write-Host "[surface] $name : timed out, killed" }
  if ($busy -and -not $busy.WaitForExit(30000)) { Stop-Process -Id $busy.Id -Force }
  if ($pwr -and -not $pwr.WaitForExit(30000)) { Stop-Process -Id $pwr.Id -Force }
  Get-ChildItem $logs | Where-Object { $before -notcontains $_.Name -and $_.LastWriteTime -ge $t0 } |
    Where-Object { $_.Extension -in '.log', '.csv', '.txt' } | Copy-Item -Destination $dest
  if ($name -like 'route*') {   # S105 SRJ: the route's screenshots (the game writes captures\NNN_label.png next to the exe)
    $capDir = Join-Path $exeDir 'captures'
    if (Test-Path $capDir) {
      $shots = @(Get-ChildItem $capDir -File | Where-Object { $_.LastWriteTime -ge $t0 -and $_.Extension -in '.png', '.jpg' })
      if ($shots.Count) { $sd = Join-Path $dest 'captures'; New-Item -ItemType Directory -Force $sd | Out-Null; $shots | Copy-Item -Destination $sd }
      Write-Host "[surface] $name : $($shots.Count) screenshots copied"
    }
  }
  Write-Host "[surface] $name : exit $($p.ExitCode), $([int]((Get-Date) - $t0).TotalSeconds) s"
  # (Session 102 lane S0-2: the heat verdict of a scene step, one rule written in heat_steady.ps1 / heat_rules.py.)
  # (A/B arms and the abwarm run get a verdict but never a repeat; only a heat reason on a game that ended cleanly repeats: Test-HeatRetry.)
  if ($usePower -and $Threads -and $haveHeat) {
    $pcsv = Join-Path $dest 'power.csv'
    $hv = Get-HeatVerdict $pcsv $script:PrevPower
    $text = if ($Retry -and $FirstTry) { "retry; first try: $FirstTry`r`nthis try: $($hv.Reason)" } else { $hv.Reason }
    Set-Content (Join-Path $dest 'heat_verdict.txt') $text
    Write-Host "[surface] $name : heat: $($hv.Reason)"
    if (Test-HeatRetry $hv $name $Retry $p.ExitCode $timedOut) {
      $rej = "$dest`_rejected"
      if (Test-Path $rej) { Remove-Item -Recurse -Force $rej }
      Move-Item $dest $rej
      Write-Host "[surface] $name : rejected (outside the steady band), repeating once; kept as $rej"
      Invoke-Step $name $route $flags $freshData $Threads $Affinity $true $hv.Reason
    } elseif (Test-Path $pcsv) { $script:PrevPower = $pcsv }
  }
}

if ($Only -contains 'passes') {
  Invoke-Step 'passes' 's72_gap_log.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                            '--vsync=false') $false
}
# passes_ram (Session 80, 14:20; not in the default list): the same per-pass log with gpu_ram_stats, which times every
# resolve's copy and texture fill on its own (segment kinds 6 / 7): the plugged-in run 14:01 had 11.0 of the
# entrance's 32.0 GPU ms between passes, at the scene's colour resolves and depth fills (gap_log.py).
if ($Only -contains 'passes_ram') {
  Invoke-Step 'passes_ram' 's72_gap_log.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                                '--gpu_ram_stats=true', '--vsync=false') $false
}
# multi (Session 80, not in the default list): the in-run A/B of SCENE_SHADERS_PLAN.md - five arms (the game's
# shaders, our crowd shaders, a flat wrestler body, the crowd skipped, the wrestler body skipped) in rotation every 8
# frames at Brock +300, Bryan +440 and the match +900 of the s21_lb replay (tools\native\passab_multi.py; read
# with its --read). Sequential runs drift with the Surface's heat (14:49: a lever that removes GPU work came out 47% slower
# as the 4th of 4 runs), so every comparison on it runs inside one run.
if ($Only -contains 'multi') {
  # (The route's own switches - e.g. scene_multi's --wwe2k14_scene_shaders=true / _probe=1 - come in -GameFlags:
  # a flag given twice drops every startup flag after it.)
  Invoke-Step 'multi' $MultiRoute @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true', '--vsync=false') $false
}
# arc (Session 80, not in the default list): the same kind of in-run A/B for the HD 520 arc's GPU parts (route
# s80_arc_multi.txt: all on = today, all off = the arc's start for GPU work, and alias loads / copy batching / cube
# faces / the crowd cull + thin off one at a time). PC check 5420 (4090): all off +20% / +34% / +9% GPU (Brock / Bryan /
# match).
if ($Only -contains 'arc') {
  Invoke-Step 'arc' 's80_arc_multi.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                         '--vsync=false') $false
}
if ($Only -contains 'frames') {
  Invoke-Step 'frames' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan') $false -Threads $true
}
# Session 89 (per-scene frames, not in the default list): one scene per run at NORMAL speed, fixed seed 1 (Bryan's camera
# variant pinned), entrances quitting 10 s after their step 15 and the match 10 s after its clock starts (Jarrod)
# (tools\input_scripts\scenes\; pushed flat into Routes with
# nav\boot_to_menu.txt, which they include). frames_bryan = Bryan out FIRST (participants 324,343), so his entrance
# starts ~6 s after the launch instead of after Brock's. The flags here repeat the routes' '# harness: flags' headers
# (the Surface bench does not read them). Read: py tools\native\frames_fps.py CSV name:first:last (prints the seed).
$sceneSteps = [ordered]@{
  'frames_bryan' = @('bryan_entrance.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_participants=324,343', '--wwe2k14_fixed_seed=1'))
  'frames_brock' = @('brock_entrance.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_fixed_seed=1'))
  'frames_bryan_heavy' = @('bryan_heavy.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_participants=324,343', '--wwe2k14_fixed_seed=1'))   # S102 M6: Bryan's heavy stretch only (tron+760 .. +1330), see the route's header
  'frames_match' = @('match.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_skip_entrances=true', '--wwe2k14_fixed_seed=1'))
}
$sceneFlagList = @($SceneFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })
if ($FrameSplit) { $sceneFlagList = @($sceneFlagList) + @('--wwe2k14_frame_split=true') }  # S102 H3
# S102 M7 (review F5): the NON-reference phase's values on the command line of both arms, so a switch that only works when armed at start (reader.h LiveOptions copies,
# the prelight multiview device feature, the card thin probe: src\ab_flip_math.h kArmed) is armed; the game sets the reference values at frame 2 and the phases from the anchor.
function Get-AbFlipCli([string]$Spec) {
  $phases = @($Spec -split '/' | Where-Object { $_ -and $_ -notmatch '^(period|settle|start|order|first|anchor)=' })
  if ($phases.Count -lt 2) { return @() }
  $non = ($phases[1] -split '=', 2)[1]
  return @($non -split '\+' | Where-Object { $_ } | ForEach-Object { '--' + $_ })
}
if ($AbFlip) {   # S102 M7: the in-run A/B; the format is checked here and by s0_5_jobs.ps1, the game checks the switches (PerFrame only) and refuses a bad spec whole
  if ($AbFlip -notmatch '^[A-Za-z0-9_=+/:]{8,600}$') { throw "AbFlip: the spec may hold only letters, digits, _ = + / : (no ; , | space): $AbFlip" }
  if ($ArmFlags.Count -or $ArmAFlags.Count -or $ArmBFlags.Count -or $ArmBExe) { throw 'AbFlip: not with ArmFlags / ArmAFlags / ArmBFlags / ArmBExe (put the base levers in SceneFlags: both arms get them) (the two arms of an AbFlip job are the complementary schedules)' }
  if ($AbFlip -match '(^|/)first=') { throw 'AbFlip: do not write first= (arm B adds /first=on itself)' }
  if ($AbFlip -notmatch '(^|/)anchor=\d') { throw 'AbFlip: the spec needs anchor=<route mark id> (a pair is paired by route-mark time, never by the absolute guest frame)' }
  if ($Only -notcontains 'ab') { $sceneFlagList = @($sceneFlagList) + @(Get-AbFlipCli $AbFlip) + @("--wwe2k14_ab_flip=$AbFlip") }
}
$abSceneFlags = @($SceneFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })   # S102 M7 (F8): SceneFlags (the base levers) reach EVERY ab arm
# S102 B1: Warm / Repeats (see the parameters). The names the zip takes are listed in $stepKeys below.
$selScenes = @($sceneSteps.Keys | Where-Object { $Only -contains $_ })
if (($Warm -gt 0 -or ($Repeats -gt 1 -and $Only -notcontains 'route')) -and -not $selScenes.Count) { Write-Host "[surface] Warm=$Warm Repeats=$Repeats ignored: no scene step in -Only ($($Only -join ','))" }
if ($Warm -lt 0 -or $Warm -gt 600) { throw "Warm=$Warm : seconds, 0 to 600" }
if ($Repeats -lt 1 -or $Repeats -gt 9) { throw "Repeats=$Repeats : 1 to 9" }
if ($Warm -gt 0 -and $selScenes.Count) {
  $wk = $selScenes[0]
  $warmStart = Get-Date; $wi = 0
  while (((Get-Date) - $warmStart).TotalSeconds -lt $Warm -and $wi -lt 6) {
    $wi++
    Write-Host "[surface] warm_$wi : discarded load (the $wk route) to spend the power burst; $([int]((Get-Date) - $warmStart).TotalSeconds) s of $Warm s so far"
    Invoke-Step "warm_$wi" $sceneSteps[$wk][0] (@($sceneSteps[$wk][1]) + $sceneFlagList) $false -Threads $false
  }
  Write-Host "[surface] warm: $wi discarded run(s), $([int]((Get-Date) - $warmStart).TotalSeconds) s; the measured steps start now"
}
for ($rep = 1; $rep -le $Repeats; $rep++) {
  foreach ($k in $selScenes) {
    $nm = if ($rep -eq 1) { $k } else { "${k}_r$rep" }
    Invoke-Step $nm $sceneSteps[$k][0] (@($sceneSteps[$k][1]) + $sceneFlagList) $false -Threads $true
  }
}
# S105 SRJ: route (see the Route parameter): one route, Repeats times, no heat verdict / retry (a diagnosis job, not a bar).
if ($Only -contains 'route') {
  if (-not $Route) { throw 'Only=route needs Route=<route file name in the routes folder>' }
  if ($Route -notmatch '^[A-Za-z0-9_.-]+$') { throw "Route=$Route : a file name only" }
  if (-not (Test-Path (Join-Path $Routes $Route))) { throw "Route=$Route : not in $Routes (push_kit.ps1 sends it)" }
  if ($Repeats -lt 1 -or $Repeats -gt 9) { throw "Repeats=$Repeats : 1 to 9" }
  for ($rep = 1; $rep -le $Repeats; $rep++) {
    $nm = if ($rep -eq 1) { 'route' } else { "route_r$rep" }
    Invoke-Step $nm $Route @() $false -Threads $false
  }
}
# pinned (Session 79, not in the default list): the frames run held to 2 cores / 4 threads (the i3-6100U's count; the
# Surface's i5 has 4 / 8), with every thread's CPU time: the CPU budget at 60 fps on a 4-thread laptop.
if ($Only -contains 'pinned') {
  Invoke-Step 'pinned' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan') $false -Threads $true -Affinity 0xF
}
# gpuprof (Session 95, tool Q): one launched run with the per-pass GPU logs at Brock +300, the pyro, Bryan +440, the match +600.
# Read: py -I tools/native/gpu_profile.py RUN --dir STEP_FOLDER.
if ($Only -contains 'gpuprof') {
  Invoke-Step 'gpuprof' 's95_gpu_profile.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_fixed_seed=1', '--vulkan_gpu_timestamps=true',
    '--gpu_native_gpu_timers=true', '--vsync=false') $false
}
# shaderrank (Session 95, B2): the shader family ranking - one match run, each family of our pixel shaders flat grey in turn
# (live cvar gpu_native_shader_ablate; needs the s95-shade SDK build). Read: py -I tools/native/shader_rank.py RUN --dir STEP_FOLDER.
if ($Only -contains 'shaderrank') {
  Invoke-Step 'shaderrank' 's95_shader_rank.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_skip_entrances=true', '--wwe2k14_fixed_seed=1',
    '--vulkan_gpu_timestamps=true', '--gpu_native_gpu_timers=true', '--vulkan_stored_pipelines_at_boot=true', '--vsync=false') $false
}
# stacks (Session 95): one symbols profile of the frames route (the kit needs push_kit -Symbols).
if ($Only -contains 'stacks') {
  Invoke-Step 'stacks' 'profile_lesnar_bryan.txt' (@('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_fixed_seed=1',
    '--wwe2k14_profile_gpu_commands=true', '--wwe2k14_profile_threads=rdr_render,rdr_record', '--wwe2k14_profile_main=true', '--wwe2k14_profile_stacks=true') + $sceneFlagList) $false   # S102: SceneFlags reach the profile too; the owned renderer has no "GPU Commands" thread: sample the main (VdSwap) thread and rdr_render
}
# ab (Session 95, heat aid): see the header. Same scene, A B B A back to back with a fixed gap.
if ($Only -contains 'ab') {
  if (-not $sceneSteps.Contains($AbScene)) { Write-Host "[surface] ab: -AbScene must be one of $($sceneSteps.Keys -join ', ')"; throw 'bad -AbScene' }
  if (-not $ArmFlags.Count -and -not $ArmBFlags.Count -and -not $ArmBExe) { Write-Host '[surface] ab: arm B equals arm A (no -ArmBFlags, no -ArmBExe): the pair measures the noise floor' }
  if ($ArmBExe -and -not (Test-Path -LiteralPath $ArmBExe)) { throw "ab: -ArmBExe not found: $ArmBExe" }
  # (Session 95 stack 4: -ArmAFlags are added to arm A only, so the arms can differ by a whole flag set.)
  $armAFlagList = @($ArmAFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })
  $armBFlagList = @($ArmBFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })
  $order = New-Object System.Collections.Generic.List[string]
  $abNames = @()
  $seq = @()
  $armSpec = $null
  if ($ArmFlags.Count) {   # S102 M5: several B arms in one job
    if ($ArmAFlags.Count -or $ArmBFlags.Count -or $ArmBExe) { throw 'ab: -ArmFlags replaces -ArmAFlags / -ArmBFlags / -ArmBExe: use one form' }
    $armSpec = Get-AbArmSpec $ArmFlags
    $seq = @(Get-AbPlan @($armSpec.Keys) $AbRounds ([bool]$AbWarmup))
    $planRounds = @(); for ($r = 1; $r -le $AbRounds; $r++) { $planRounds += , @($seq | Where-Object { $_[0] -like "ab_${r}_*" } | ForEach-Object { $_[1] }) }
    $planBad = Test-AbOrder $planRounds @($armSpec.Keys)
    if ($planBad.Count) { throw "ab: the order was refused: $($planBad -join '; ')" }
    Write-Host "[surface] ab: $($armSpec.Count) arms ($($armSpec.Keys -join ', ')), $AbRounds rounds, $($seq.Count) runs"
  }
  elseif ($AbWarmup) { $seq += , @('abwarm', 'A') }
  # A B B A A B ...: heat drifts up one slot at a time, so alternate which arm goes first in a round (Session 95 review).
  if (-not $armSpec) { for ($r = 1; $r -le $AbRounds; $r++) {
    if ($r % 2 -eq 1) { $seq += , @("ab_${r}_A", 'A'); $seq += , @("ab_${r}_B", 'B') }
    else { $seq += , @("ab_${r}_B", 'B'); $seq += , @("ab_${r}_A", 'A') }
  } }
  $first = $true
  foreach ($e in $seq) {
    if (-not $first) { Write-Host "[surface] ab: gap $AbGapSeconds s (no cool-down: both arms stay hot)"; Start-Sleep -Seconds $AbGapSeconds }
    $first = $false
    $script:StepExe = if ($e[1] -eq 'B' -and $ArmBExe) { $ArmBExe } else { '' }
    # S102 M7 (review F8): every ab arm gets the SceneFlags (the @BASE levers) BEFORE its arm flags (later wins, so an arm flag overrides); s0_5_jobs.ps1's @BASE guard credits SceneFlags as arm A's flags, and they never reached the arms before.
    $fl = @($sceneSteps[$AbScene][1]) + $abSceneFlags + $(if ($armSpec) { @($armSpec[$e[1]]) } elseif ($e[1] -eq 'B') { $armBFlagList } else { $armAFlagList }) + $(if ($FrameSplit) { @('--wwe2k14_frame_split=true') } else { @() }) + $(if ($AbFlip) { @(Get-AbFlipCli $AbFlip) + @("--wwe2k14_ab_flip=$AbFlip" + $(if ($e[1] -eq 'B') { '/first=on' } else { '' })) } else { @() })  # S102 H3: both arms; S102 M7: the complementary pair (non-reference values on the command line)
    $order.Add("$($e[0])`t$($e[1])`t$(Get-Date -Format 'HH:mm:ss')`t$AbScene`t$($fl -join ' ')`t$(if ($script:StepExe) { $script:StepExe } else { $Exe })")
    Invoke-Step $e[0] $sceneSteps[$AbScene][0] $fl $false -Threads $true
    if ($e[0] -ne 'abwarm') { $abNames += $e[0] }
  }
  $script:StepExe = ''
  Set-Content (Join-Path $Out 'ab_order.txt') $order
  $script:AbOrderFile = Join-Path $Out 'ab_order.txt'
  $Only += $abNames
}
if ($Only -contains 'mul') {
  Invoke-Step 'mul' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_intel_exact_mul=0') $false
}
# levers: each candidate lever as one fast-forwarded run of the frames route on top of --wwe2k14_native_frame (the GPU
# is the limit on Intel, so fast-forwarded frames per second per scene compare the GPU cost): base, then one change
# each. Read: bench_report.py report <run> per variant, scene by scene against "base".
if ($Only -contains 'levers') {
  $lever_flags = [ordered]@{
    'base'           = @()
    'no_crowd_thin'  = @('--gpu_scene_crowd_thin=0')   # (Session 80: the thinner distant crowd is everyone's default)
    'no_depth_clear' = @('--gpu_native_truth_clear_attachments=false')
    'no_dedupe'      = @('--gpu_native_truth_dedupe_state=false')
    'no_dead_glare'  = @('--gpu_native_frame_dead_glare=0')
    'no_alias_direct' = @('--gpu_native_truth_alias_direct=false')        # (Session 79 01:46: the stores back)
    'no_alias_rgba16' = @('--gpu_native_truth_alias_direct_rgba16=false') # (Session 79 05:18: the RGBA16 owners' stores back)
    'no_alias_lazy'  = @('--gpu_native_truth_alias_direct_lazy_flush=false')  # (Session 79 05:50: a flush before each)
    'no_defer_memory' = @('--gpu_native_truth_defer_memory_resolves=false')
    'no_defer_depth' = @('--gpu_native_truth_defer_depth_resolves=false')  # (Session 79 04:34: the depth copies batched)
    'no_defer_full'  = @('--gpu_native_truth_defer_full_resolves=false')   # (Session 79 04:56: the k_8 / k_4_4_4_4 copies)
    'no_cube_faces'  = @('--gpu_native_truth_texture_only_cube_faces=false', '--gpu_native_truth_fill_cube_faces=false')
    'mul_off'        = @('--wwe2k14_intel_exact_mul=0')
    # (Look levers: texture filtering. Since Session 80 players get the game's own (anisotropic_override -1; the forced
    # 4x before, 3); Gen9 filters anisotropically at a fraction of the bilinear rate. 0 = none.)
    'aniso_4x'       = @('--anisotropic_override=3')
    'aniso_off'      = @('--anisotropic_override=0')
    # (Session 80: the readback rules are everyone's default - this puts back Session 71's one limit, the character
    # windows' CPU / GPU waits.)
    'no_readback_narrow' = @('--wwe2k14_readback_skip_recurring_narrow=0')
    # (Session 80, look lever, off: the pixel shaders' maths at half precision, characters excepted. ps_relaxed = the
    # decoration only - does Intel's driver honour it at all; ps_half = real 16-bit floats. Shots: out\s80\review.)
    'ps_relaxed'     = @('--gpu_native_ps_relaxed_precision=true')
    'ps_half'        = @('--gpu_native_ps_relaxed_precision=true', '--gpu_native_ps_half=true')
    # (Session 80, off: the quiet pipeline workers raised when a skipped draw's pipeline is queued.)
    'workers_raise'  = @('--gpu_native_pipeline_workers_raise=true')
    # (Session 80, look lever, off - Jarrod: "Every other frame looks fine": the wrestlers' shadow maps on alternate
    # frames.)
    'shadow_half'    = @('--gpu_shadow_half_rate=true')
    # (Session 80, SCENE_SHADERS_PLAN.md step 1, off: our own scene material shaders - so far the crowd's pixel
    # shaders.)
    'scene_shaders'  = @('--wwe2k14_scene_shaders=true')
  }
  $chosen = Select-Levers $lever_flags
  foreach ($name in $chosen) {
    Invoke-Step "lever_$name" $AbRoute (@('--wwe2k14_launch=lesnar_bryan', '--vsync=false') + $lever_flags[$name]) $false
  }
  $Only += @($chosen | ForEach-Object { "lever_$_" })
}
# probes (Session 79, 04:05; not in the default list): what the frame's structure costs on Intel, as the levers step
# (one fast-forwarded frames run each against probe_base). Not candidate defaults - each turns a NATIVE part off.
# Dry run on the PC (out\s79\surface_dryrun4, 4 min 49 s): GPU per frame +25-30% with no_batch, the rest within ~5%.
# (A "no copies" ceiling was tried and dropped: the textures then reload from memory, so GPU time rose - not a ceiling.)
if ($Only -contains 'probes') {
  $probe_flags = [ordered]@{
    'base'              = @()
    'no_batch'          = @('--gpu_native_truth_batch_resolves=false', '--gpu_native_truth_batch_fills=false')  # one barrier pair a copy
    'no_merge_barriers' = @('--vulkan_merge_adjacent_barriers=false', '--vulkan_cancel_inverse_transitions=false')
    'no_dont_care'      = @('--gpu_native_truth_pass_dont_care=false', '--gpu_native_truth_color_none=false')  # load / store ops
    'no_crowd_batch'    = @('--gpu_scene_crowd_batch=false')   # a draw per crowd member: the front end's per-draw cost
    # (Session 80, SCENE_SHADERS_PLAN.md step 0: what each target material costs on Intel, its draws not drawn - the
    # most our own shader could save. The debug skip also turns the lean draw path off: compare GPU-bound scenes.)
    'skip_crowd'        = @('--gpu_debug_skip_vertex_shader=183CC64A4C4E96AC,0E2A3DF5BAA2CA18,C1028A64EFCF8229,CB5B16DA1312E5AB')
    'skip_wrestler_body' = @('--gpu_debug_skip_vertex_shader=F709D14F30FCC95C')
    'skip_4f11'         = @('--gpu_debug_skip_vertex_shader=4F1118D4B3F6D388')
    # (Session 80: a flat grey in place of the wrestlers' body material, src\scene_swap.h - the most our own shader for
    # it could save on its pixel side; skip_wrestler_body minus this = its vertices and rasterising.)
    'flat_wrestler_body' = @('--wwe2k14_scene_shaders_probe=1')
  }
  $chosen = Select-Levers $probe_flags
  foreach ($name in $chosen) {
    Invoke-Step "probe_$name" $AbRoute (@('--wwe2k14_launch=lesnar_bryan', '--vsync=false') + $probe_flags[$name]) $false
  }
  $Only += @($chosen | ForEach-Object { "probe_$_" })
}
# (-ColdDriver: Intel's driver cache moved aside for the step and back afterwards; the step's own cache is kept.)
function Invoke-DriverCold([string]$Name, [scriptblock]$Body) {
  $driverCache = Join-Path $env:USERPROFILE 'AppData\LocalLow\Intel\ShaderCache'
  $aside = "$driverCache.aside_${stamp}_$Name"
  if ($ColdDriver -and (Test-Path $driverCache)) { Rename-Item $driverCache $aside; Write-Host "[surface] driver cache moved aside" }
  try { & $Body } finally {
    if ($ColdDriver -and (Test-Path $aside)) {
      if (Test-Path $driverCache) { Rename-Item $driverCache "$driverCache.cold_${stamp}_$Name" }
      Rename-Item $aside $driverCache
      Write-Host "[surface] driver cache moved back (the step's own cache kept as ShaderCache.cold_${stamp}_$Name)"
    }
  }
}
if ($Only -contains 'cold') {
  Invoke-DriverCold 'cold' {
    Invoke-Step 'cold' 's78_anatomy_720.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_preset_low=true',
                                               '--vsync=false') $true
  }
}
# precompile (Session 80, not in the default list): a player's first launch - the launcher's PREPARING SHADERS run
# (--wwe2k14_precompile: every stored NATIVE pipeline on a worker per CPU thread, here only the shipped list's, our cache
# emptied; the game quits when done). Its progress JSON (made, total, seconds) lands in the step's folder. With
# -ColdDriver: the first launch after an install or a driver update (the arc's 30 s target on the HD 520).
if ($Only -contains 'precompile') {
  Invoke-DriverCold 'precompile' {
    Invoke-Step 'precompile' 's80_precompile_idle.txt' @("--wwe2k14_precompile=$(Join-Path $Out 'precompile\progress.json')",
                                                          '--vsync=false') $true
  }
}
$zip = Join-Path $Out "surface_bench_$stamp.zip"
$stepBase = { param($n) ($n -replace '_rejected$', '') -replace '_r\d+$', '' }   # S102 B1: frames_bryan_r2 / frames_bryan_r2_rejected belong to frames_bryan
$dirs = @(Get-ChildItem $Out -Directory | Where-Object { $Only -contains $_.Name -or $Only -contains (& $stepBase $_.Name) -or $_.Name -like 'warm_*' } | ForEach-Object FullName)   # (S0-2: a rejected heat run travels in the zip too; S102 B1: repeats and the warm-up loads too)   # (S0-2: a rejected heat run travels in the zip too)
if ($script:AbOrderFile -and (Test-Path $script:AbOrderFile)) { $dirs += $script:AbOrderFile }
if ($dirs.Count) {
  Compress-Archive -Path $dirs -DestinationPath $zip
  Write-Host "[surface] done: $zip"
} else {
  Write-Host "[surface] nothing ran (-Only $($Only -join ','))"
}
