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
#   -OffFlags '...' flags for the OFF (reference) arms only (Session 90: Bryan's reference is the game's raw crowd, '--wwe2k14_scene_crowd=0' - the default crowd interception hides 5 spectators the game draws, SESSION90_SUMMARY.md, K3)
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
      [switch]$Validation, [string]$Verdict = '', [string]$Exe = '', [string]$OnFlags = '', [string]$OffFlags = '', [switch]$Exact,
      [switch]$NoCensus, [ValidateRange(1, 4)][int]$Parallel = 3,  # Session 92: -Parallel 2 keeps a builder within its game cap beside other checks
      [string]$OnRoute = '',  # Session 95: another ON-arm route file (same file name, so the verdict treats it as the route), e.g. one that runs on past a 600-frame summary
      [switch]$Only720,  # Session 94 (gpu_sweep.ps1): the 720p pair only (ON + OFF arm), no 1440p arms
      [switch]$Poison,   # Session 95 (TILER): the ON arms also run --gpu_tiler_poison=true (DONT_CARE loads cleared to magenta); magenta pixels in the shots are counted (magenta_scan.py)
      [switch]$Sync,     # Session 95 (TILER): synchronization validation on every arm (implies -Validation; VK_LAYER_ENABLES + no duplicate limit); hazards are counted as validation errors
      [switch]$Vendor,   # Session 95: with -Sync, also the layer's best-practices checks with the Arm and IMG vendor checks (their messages are 'BestPractices-...' warnings, not counted as errors: grep the ON log)
      [switch]$SyncControl,  # Session 95: with -Sync, the ON arms also run --gpu_tiler_sync_control=true (a deliberate hazard: the positive control, it MUST report hazards)
      [string]$ReuseOff = '')  # Session 92: 'RUN720 RUN1440' = run only the two ON arms and take the OFF (reference) arms from these earlier runs
                               # (same exe, same route, OFF flags unchanged - e.g. a fix that only touches the ON arm's switch); the verdict
                               # and the feature proofs then run on the new ON pair + those OFF runs
$root = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $root
$vArgs = @()
if ($Exact) { $vArgs += '--exact' }
if ($Exe) { $vArgs += @('--dir', (Split-Path -Parent $Exe)) }
if ($Verdict) {
  py -I "$PSScriptRoot\owned_verdict.py" @($vArgs) @($Verdict -split '[ ,]+' | Where-Object { $_ })
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
# (Session 90: TRUE NATIVE, --wwe2k14_d3d_owned=true, turns the owned frame on by itself: it gets the census too.)
if ($Route -eq 'quick' -and -not $NoCensus -and $OnFlags -match 'gpu_native_owned_frame|wwe2k14_d3d_owned') {
  $script = 'tools\input_scripts\short\s82_match_shots_census.txt'
  $scriptOn = 'tools\input_scripts\short\s82_match_shots_census_on.txt'   # the ON arms: + a flip, so the truth-work summary is match-only
}
if (-not $scriptOn) { $scriptOn = $script }
if ($OnRoute) { $scriptOn = $OnRoute }
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
$prevLayerEnables = $env:VK_LAYER_ENABLES; $prevLayerDup = $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT  # (restored at every exit below: the script runs in-process)
$restoreLayerEnv = { $env:VK_LAYER_ENABLES = $prevLayerEnables; $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT = $prevLayerDup }
if ($Sync) { $Validation = [switch]::Present; $env:VK_LAYER_ENABLES = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'; $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT = '0' }
if ($Sync -and $Vendor) { $env:VK_LAYER_ENABLES += ',VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_ARM,VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_IMG' }
if ($Poison) { $on = ($on + ' --gpu_tiler_poison=true').Trim() }
if ($SyncControl) { $on = ($on + ' --gpu_tiler_sync_control=true').Trim() }
$valid = if ($Validation) { ' --vulkan_validation_enabled=true' } else { '' }
$common = "--wwe2k14_replay_play=$replay --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false $More".Trim()
if ($Exe) {
  $common += " --game_data_root=$root\game_data\extracted --wwe2k14_replay_dir=$root\out\replays --wwe2k14_scenario_dir=$root\out\scenarios"
}
$exeArg = if ($Exe) { @{ Exe = $Exe } } else { @{} }
# (On arms first: they start in the first lanes; the off arms are the references.)
# (Session 84: the owned frame is on by default since v0.2.17 - the off arms turn it off too, or they were owned as well and
# the verdict found no reference: "no owned-off run of the same scale / route / replay given".)
$off = ('--gpu_native_own_steps=0 --gpu_native_owned_frame=false ' + $OffFlags).Trim()  # (Session 90: -OffFlags, flags for the OFF (reference) arms only)
$extras = @("--wwe2k14_settings_file=$($settings['720']) $on$valid".Trim(),
            "--wwe2k14_settings_file=$($settings['1440']) $on$valid".Trim(),
            "--wwe2k14_settings_file=$($settings['720']) $off",
            "--wwe2k14_settings_file=$($settings['1440']) $off")
$reuse = @($ReuseOff -split '[ ,]+' | Where-Object { $_ })
if ($ReuseOff -and $reuse.Count -ne $(if ($Only720) { 1 } else { 2 })) { "owned_check: -ReuseOff takes two runs (the 720p and the 1440p OFF arm; with -Only720 one: the 720p OFF arm), got '$ReuseOff'"; & $restoreLayerEnv; exit 2 }
$scripts = if ($reuse) { @($scriptOn, $scriptOn) } else { @($scriptOn, $scriptOn, $script, $script) }
$runExtras = if ($reuse) { @($extras[0], $extras[1]) } else { $extras }
if ($Only720) {   # Session 94: arms 0 (ON 720) and 2 (OFF 720); with -ReuseOff RUN only the ON arm runs
  if ($reuse) { $scripts = @($scriptOn); $runExtras = @($extras[0]) }   # -ReuseOff RUN = an earlier 720p OFF arm: only the ON arm runs
  else { $scripts = @($scriptOn, $script); $runExtras = @($extras[0], $extras[2]) }
}
$out = & "$root\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $runExtras `
  -Parallel $Parallel -Extra $common @exeArg 2>&1
$out | Where-Object { "$_" -match ' -> wwe2k14_\d+' } | ForEach-Object { "$_" }
$runs = @($out | ForEach-Object { if ("$_" -match ' -> wwe2k14_(\d+)') { $Matches[1] } })
if ($runs.Count -ne $scripts.Count) {
  "owned_check: expected $($scripts.Count) runs, got $($runs.Count) - see the run_list lines above"
  & $restoreLayerEnv; exit 1
}
if ($reuse) {
  "owned_check: OFF arms reused from earlier runs $($reuse -join ', ') (-ReuseOff)"
  $runs += $reuse
}
# (Session 92: before the verdict is believed, PROVE from the ON arms' logs that every switch they were given engaged
# (tools\harness\feature_proof.py + feature_proofs.toml): an on arm that lost --wwe2k14_d3d_owned=true ran without the host
# queue and 4 runs proved nothing (M5c). The ON arms are the first two runs (run_list prints in input order).)
$logDir = if ($Exe) { Join-Path (Split-Path -Parent $Exe) 'logs' } else { "$root\out\build\win-amd64-release\logs" }
$expect = @()
foreach ($m in [regex]::Matches("$on $More", '--(\w+)=(\S+)')) { $expect += '--expect'; $expect += "$($m.Groups[1].Value)=$($m.Groups[2].Value)" }
$notEngaged = @()
foreach ($i in $(if ($Only720) { 0 } else { 0, 1 })) {
  $log = Join-Path $logDir ('wwe2k14_{0}.log' -f $runs[$i])
  $fp = & py -I "$PSScriptRoot\..\harness\feature_proof.py" $log @expect 2>&1  # (Session 95: this tree's tool and table, not the main tree's)
  $fpCode = $LASTEXITCODE
  "feature proofs, ON run $($runs[$i]):"
  $fp | ForEach-Object { "  $_" }
  if ($fpCode -ne 0) {
    $bad = @($fp | Where-Object { "$_" -match '^NOT ENGAGED\s+(\S+)' } | ForEach-Object { ("$_" -split '\s+')[2] })
    if (-not $bad) { $bad = @('(the proof check itself failed, exit ' + $fpCode + ')') }
    foreach ($b in $bad) { $notEngaged += "NO VERDICT: feature $b not engaged in run $($runs[$i])" }
  }
}
$scanBad = $false
if ($Poison) {
  $scanArgs = @(); if ($Exe) { $scanArgs += @('--dir', (Split-Path -Parent $Exe)) }  # (not --exact: the scan takes only --dir)
  foreach ($i in $(if ($Only720) { 0 } else { 0, 1 })) {
    "magenta scan, ON run $($runs[$i]):"
    $scanOut = @(& py -I "$PSScriptRoot/magenta_scan.py" @($scanArgs) $runs[$i] 2>&1)
    if ($LASTEXITCODE -ne 0) { $scanBad = $true }
    $scanOut | ForEach-Object { "  $_" }
  }
}
$vOut = @(& py -I "$PSScriptRoot\owned_verdict.py" @($vArgs) @runs 2>&1)
$vCode = $LASTEXITCODE
if ($notEngaged) {
  $vOut | Where-Object { "$_" -notmatch '^OWNED VERDICT ' } | ForEach-Object { "$_" }
  $notEngaged
  'NO VERDICT: the pictures above are NOT a result - fix the flags and re-run the check (feature_proof.py)'
  & $restoreLayerEnv; exit 3
}
$vOut | ForEach-Object { "$_" }
if ($scanBad) { 'FAIL: poison (magenta) pixels or no shots found by magenta_scan.py - the pictures above are NOT a pass'; if ($vCode -eq 0) { $vCode = 1 } }
& $restoreLayerEnv; exit $vCode
