# Session 77 (XDET_PLAN.md stage A): the PC side of the Cena vs Undertaker cross-device diagnostic. Every run gets
# tools\xdet\stageA.flags, the test ISO (the file tools\android\run.ps1 -PushGame copies to the phone), the frozen user
# data (run_list mirrors it per run), the det_serial scheduler trace, and raw dumps every 60 frames (local only), then
# the runs are compared with tools\determinism\det_compare.py --cross (strict: frame against frame, all layers).
#   pwsh -NoProfile -File tools\xdet\run_stageA.ps1 -Record NAME [-Scenario pad|cpu] [-Seed 77]   record NAME.rec
#   pwsh -NoProfile -File tools\xdet\run_stageA.ps1 -Play NAME [-Plays 2]       play it N times, compare each pair
#   ... [-More '--flag=x ...'] [-Shots] [-RealTime] [-Compare RUN]
# -Scenario: tools\xdet\scenario_<NAME>.flags, recording only (playback takes the launch from the recording).
# -Shots: a PC-only variant of the route with a screenshot at each match mark (PICTURE layer; phones write none).
# -RealTime: normal speed (the host-speed check: a fast-forwarded and a normal-speed play must match).
# -Compare RUN: also compare every new run with RUN (e.g. the reference play).
# -Route FILE: another route (stage 2: tools\xdet\cena_taker_entr.txt with -Scenario entr); default cena_taker_a.txt.
# -FlagsFile NAME: the flags file in tools\xdet (default stageA.flags; played.flags = the "as played" config).
# -NoDraw: --gpu_det_skip_host_draws=true (GPU packets run, no host draws or copies: no picture; PC 4924 = drawn 4919 in
#   every game layer, ~20% faster; phone 24% faster). Compare no-draw with no-draw (the comparer treats it as a game
#   setting: copies the game reads back are not written).
# -Data NAME: run_list's user data copy (default userdata_frozen; userdata_coldcache = the same save, empty cache).
# Session 77: -More must not hold the flags run_list sets itself (--user_data_root, --cache_root, --wwe2k14_timeline,
#   --wwe2k14_input_script): a flag given twice drops every startup flag after it (run 4952: --cache_root in -More,
#   the game booted without the replay, the route or the profile and sat in the attract demo). Refused here.
param([string]$Record = '', [string]$Play = '', [int]$Plays = 2, [int]$Seed = 77, [string]$More = '',
      [string]$Scenario = 'pad', [switch]$Shots, [switch]$RealTime, [int]$Compare = 0, [int]$WaitSeconds = 900,
      [string]$Route = '', [string]$FlagsFile = 'stageA.flags', [switch]$NoDraw, [string]$Data = 'userdata_frozen')
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
if (-not $Record -and -not $Play) { 'give -Record NAME or -Play NAME'; exit 2 }
foreach ($own in 'user_data_root', 'cache_root', 'wwe2k14_timeline', 'wwe2k14_input_script') {
  if ($More -match "(^|\s)--$own(=|\s|$)") { "ABORT: -More has --$own, which run_list sets itself (use -Data / -Route)"; exit 2 }
}
$iso = "$R\game_data\iso\wwe2k14_test.iso"
if (-not (Test-Path $iso)) { "missing $iso (the phone's test image)"; exit 2 }
. "$PSScriptRoot\flags.ps1"  # Merge-Flags: -More / -RealTime replace a stage A flag (a repeat drops flags)
$trace = "$R\out\harness\determinism\trace"
New-Item -ItemType Directory -Force $trace | Out-Null
$merged = Merge-Flags (Get-StageAFlags $FlagsFile) "--game_data_root=$iso --det_serial_trace=$trace --wwe2k14_det_hash_raw_every=60"
if ($RealTime) { $merged = Merge-Flags $merged '--vsync=true' }
if ($NoDraw) { $merged = Merge-Flags $merged '--gpu_det_skip_host_draws=true' }
if ($More) { $merged = Merge-Flags $merged $More }
$extra = Join-Flags $merged

$script = if ($Route) { (Resolve-Path $Route).Path } else { "$R\tools\xdet\cena_taker_a.txt" }
if ($Shots) {
  # Session 80: one file per route (it was always cena_taker_a_shots.txt: two -Shots runs started together overwrote
  # each other's route - run 5189, stage A's replay on the entrances route), written only when its text changes.
  $shotScript = "$R\out\xdet\$([IO.Path]::GetFileNameWithoutExtension($script)).shots.txt"
  New-Item -ItemType Directory -Force (Split-Path $shotScript) | Out-Null
  $out = foreach ($line in Get-Content $script) {
    $line
    if ($line -match '^log xdet: match (\d+)$') { "screenshot x_match$($Matches[1])" }
  }
  $old = if (Test-Path $shotScript) { Get-Content $shotScript } else { $null }
  if ($null -eq $old -or (Compare-Object $old $out -SyncWindow 0)) { $out | Set-Content -Encoding ascii $shotScript }
  $script = $shotScript
}

function Get-Runs($lines) {
  @($lines | Where-Object { "$_" -match '-> wwe2k14_(\d+)' } | ForEach-Object { [void]("$_" -match 'wwe2k14_(\d+)'); [int]$Matches[1] })
}

$runs = @()
if ($Record) {
  if (Test-Path "$R\out\replays\$Record.rec") { "ABORT: out\replays\$Record.rec exists (never overwritten)"; exit 2 }
  $scen = (Get-Content "$R\tools\xdet\scenario_$Scenario.flags" | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
           ForEach-Object { $_.Trim() }) -join ' '
  $recExtra = Join-Flags (Merge-Flags $merged "$scen --wwe2k14_replay_record=$Record --wwe2k14_fixed_seed=$Seed")
  $o = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra $recExtra -Data $Data -WaitSeconds $WaitSeconds 2>&1
  $o | Where-Object { "$_" -match '->|checks:' } | ForEach-Object { "record: $_" }
  $runs += Get-Runs $o
}
if ($Play) {
  $scripts = @(); for ($i = 0; $i -lt $Plays; $i++) { $scripts += $script }
  $o = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -Extra "$extra --wwe2k14_replay_play=$Play" -Data $Data -WaitSeconds $WaitSeconds 2>&1
  $o | Where-Object { "$_" -match '->|checks:' } | ForEach-Object { "play: $_" }
  $runs += Get-Runs $o
}
"runs: $($runs -join ', ')"
$pairs = @()
for ($i = 0; $i + 1 -lt $runs.Count; $i++) { $pairs += , @($runs[$i], $runs[$i + 1]) }
# (Session 77: not $r - PowerShell names ignore case, and a $r loop overwrote $R, the repo root: every compare after
# it failed, runs 4858 / 4860 / 4871.)
if ($Compare) { foreach ($run in $runs) { $pairs += , @($Compare, $run) } }
foreach ($p in $pairs) {
  $o = @(py "$R\tools\determinism\det_compare.py" --cross $p[0] $p[1] 2>&1)
  $v = $o | Select-String -Pattern '^VERDICT' | Select-Object -First 1
  # (Session 77: show the tail when there is no verdict; it found the $r / $R clash above.)
  if (-not $v) { $v = "NO VERDICT (exit $LASTEXITCODE): " + (($o | Select-Object -Last 3) -join ' | ') }
  "compare $($p[0]) vs $($p[1]): $v"
}
