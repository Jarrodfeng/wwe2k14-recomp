# Session 80 (Jarrod: "1440p and is it on seems like the way to go we'll start using those"): the owned-renderer
# check. The same route at 720p and 1440p, owned steps on (the defaults, or -Steps) and off, four runs in parallel
# lanes (run_list -Parallel 3), fast-forwarded, then tools\native\owned_verdict.py on them: per resolution the shots on
# vs off (byte-identical), and whether the owned steps were actually on (census lines with draws into our images;
# pauses after which a frame may be wrong FAIL, replayed ones WARN; refused samplers).
#   & .\tools\native\owned_check.ps1                  quick: replay s80_lb_quick + s82_match_shots_fast.txt (Session 82:
#                                                     three shots 100 frames apart, ~10 s a run with NATIVE settings;
#                                                     was s80_match_shots.txt, shots 600 apart, ~30 s) (the match
#                                                     only, 3 shots; ~1 min) - for iteration
#   & .\tools\native\owned_check.ps1 -Route full      replay s21_lb + s72_swap_shots.txt (both entrances + the match,
#                                                     4 shots; ~5 min) - once per fix, at merge
#   -Steps 'LIST'   the on arm's gpu_native_own_steps (default: the arc defaults, src\native_frame.h)
#   -More '...'     flags for every run;  -Validation  the on arms with Vulkan validation (the verdict counts errors)
#   -OnFlags '...'  flags for the ON arms only (instead of -Steps), e.g. '--gpu_native_owned_frame=true' (Session 82)
#   -Exe PATH       run another build's exe (e.g. out\harness\bisect\exe\sdk_<commit9>\wwe2k14.exe): passed to run_list -Exe,
#                   --game_data_root=<repo>\game_data\extracted added to the flags, and the verdict reads that exe folder's
#                   logs\ and captures\ (owned_verdict.py --dir). The repo's out\replays and out\scenarios via --wwe2k14_replay_dir / --wwe2k14_scenario_dir (no copies).
#   -Exact          shots must be byte-identical (the Session 80 rule); the default is the tolerance (owned_verdict.py header)
#   -Verdict 'RUNS' only the verdict, on runs made before (e.g. -Verdict '5695 5696 5631 5664'; add -Exe for another exe's
#                   runs). The verdict writes difference heat maps and one picture sheet per run pair into
#                   out\owned_check\heat\ and out\owned_check\sheets\ (a JPEG: on | off | heat map for every shot).
#   (Session 82, harness builder) THE CENSUS: when -OnFlags names gpu_native_owned_frame (an s82 build) the quick route is
#   s82_match_shots_census.txt - the same three shots on the same frames plus `cvar gpu_native_owned_frame_log 2` before the
#   first - so the ON arms also log two frames' per-pass lines, and the verdict prints, under each ON run, the scoreboard
#   (tools\native\scoreboard.py) and the truth work per frame (the last 600-frame summary) from the same four runs: no
#   separate census run. The ON arms run s82_match_shots_census_on.txt, which also flips the owned frame off / on at +300 so the
#   truth-work summary (it resets at each flip) covers match frames only. -NoCensus uses the plain route. (An exe without that setting would end the script at the cvar
#   line, so older exes / -Steps checks keep the plain route.)
# In-process only (pwsh -File mangles the strings). Settings files: out\owned_check\settings_native720 / 1440.json
# (made here if missing; NATIVE, 60 fps). The quick replay is recorded by tools\input_scripts\short\
# s80_record_lb_quick.txt (see its header) into out\replays\s80_lb_quick.rec.
#   & .\tools\native\owned_check.ps1 -Route bryan     Session 89: replay s89_bryan_first + s89_bryan_census.txt (Bryan
#                                                     out FIRST; shots at his step 15 + 100 / + 240 with the per-pass
#                                                     census at the first; the ON arms run s89_bryan_census_on.txt, whose
#                                                     owned-frame flip at step 15 makes the summary cover entrance frames
#                                                     only). Record the replay once: short\s89_record_bryan_first.txt
#   & .\tools\native\owned_check.ps1 -Route brock     the same for Brock's entrance: replay s89_brock (short\
#                                                     s89_record_brock.txt) + s89_brock_census[_on].txt
param([ValidateSet('quick', 'full', 'bryan', 'brock')][string]$Route = 'quick', [string]$Steps = '', [string]$More = '',
      [switch]$Validation, [string]$Verdict = '', [string]$Exe = '', [string]$OnFlags = '', [switch]$Exact,
      [switch]$NoCensus)
$root = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $root
$vArgs = @()
if ($Exact) { $vArgs += '--exact' }
if ($Exe) { $vArgs += @('--dir', (Split-Path -Parent $Exe)) }
if ($Verdict) {
  py -I "$root\tools\native\owned_verdict.py" @($vArgs) @($Verdict -split '[ ,]+' | Where-Object { $_ })
  exit $LASTEXITCODE
}
$replay = switch ($Route) { 'quick' { 's80_lb_quick' } 'bryan' { 's89_bryan_first' } 'brock' { 's89_brock' } default { 's21_lb' } }
$script = switch ($Route) {
  'quick' { 'tools\input_scripts\short\s82_match_shots_fast.txt' }
  'bryan' { 'tools\input_scripts\short\s89_bryan_census.txt' }
  'brock' { 'tools\input_scripts\short\s89_brock_census.txt' }
  default { 'tools\input_scripts\short\s72_swap_shots.txt' }
}
# (Session 89: the entrance routes always carry the census - every exe since Session 82 has gpu_native_owned_frame_log.)
if ($Route -in 'bryan', 'brock') { $scriptOn = $script -replace '\.txt$', '_on.txt' }
if ($Route -eq 'quick' -and -not $NoCensus -and $OnFlags -match 'gpu_native_owned_frame') {
  $script = 'tools\input_scripts\short\s82_match_shots_census.txt'
  $scriptOn = 'tools\input_scripts\short\s82_match_shots_census_on.txt'   # the ON arms: + a flip, so the truth-work summary is match-only
}
if (-not $scriptOn) { $scriptOn = $script }
# (Session 82: another exe would read <exe folder>\replays and \scenarios - it is pointed at the repo's own folders
# instead with --wwe2k14_replay_dir / --wwe2k14_scenario_dir, src\replay.h; no copies.)
$repDir = "$root\out\replays"
if (-not (Test-Path "$repDir\$replay.rec")) {
  "owned_check: $repDir\$replay.rec missing - record it first (the route's header says how)"
  exit 2
}
$dir = "$root\out\owned_check"
New-Item -ItemType Directory -Force $dir | Out-Null
$settings = @{}
foreach ($r in @(@('720', 0), @('1440', 1))) {
  $file = "$dir\settings_native$($r[0]).json"
  if (-not (Test-Path $file)) { "{`"renderer`": 2, `"frame_rate`": 1, `"resolution`": $($r[1])}" | Set-Content -NoNewline $file }
  $settings[$r[0]] = $file
}
$on = if ($OnFlags) { $OnFlags } elseif ($Steps) { "--gpu_native_own_steps=$Steps" } else { '' }
$valid = if ($Validation) { ' --vulkan_validation_enabled=true' } else { '' }
$common = "--wwe2k14_replay_play=$replay --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false $More".Trim()
if ($Exe) {
  $common += " --game_data_root=$root\game_data\extracted --wwe2k14_replay_dir=$root\out\replays --wwe2k14_scenario_dir=$root\out\scenarios"
}
$exeArg = if ($Exe) { @{ Exe = $Exe } } else { @{} }
# (On arms first: they start in the first lanes; the off arms are the references.)
# (Session 84: the owned frame is on by default since v0.2.17 - the off arms turn it off too, or they were owned as well and
# the verdict found no reference: "no owned-off run of the same scale / route / replay given".)
$off = '--gpu_native_own_steps=0 --gpu_native_owned_frame=false'
$extras = @("--wwe2k14_settings_file=$($settings['720']) $on$valid".Trim(),
            "--wwe2k14_settings_file=$($settings['1440']) $on$valid".Trim(),
            "--wwe2k14_settings_file=$($settings['720']) $off",
            "--wwe2k14_settings_file=$($settings['1440']) $off")
$out = & "$root\tools\bench\run_list.ps1" -Scripts @($scriptOn, $scriptOn, $script, $script) -ScriptExtras $extras `
  -Parallel 3 -Extra $common @exeArg 2>&1
$out | Where-Object { "$_" -match ' -> wwe2k14_\d+' } | ForEach-Object { "$_" }
$runs = @($out | ForEach-Object { if ("$_" -match ' -> wwe2k14_(\d+)') { $Matches[1] } })
if ($runs.Count -ne 4) {
  "owned_check: expected 4 runs, got $($runs.Count) - see the run_list lines above"
  exit 1
}
py -I "$root\tools\native\owned_verdict.py" @($vArgs) @runs
exit $LASTEXITCODE
