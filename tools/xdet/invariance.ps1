# Session 77 (XDET_PLAN.md verification rule 3): the graphics invariance gate. Plays a stage A recording once per
# renderer variant ("NAME<TAB>FLAGS" lines, the format of tools\android\emulate\profiles_*.tsv) with the stage A flags,
# 3 at a time (fast-forwarded; under replay mode the game's result must not depend on speed or lane load either), and
# compares each with a reference play using det_compare.py --cross --allow <render settings>: the game's layers (SIM,
# LAYOUT, SCHED, EVENTS, AUDIO) must stay identical while only renderer settings differ.
#   pwsh -NoProfile -File tools\xdet\invariance.ps1 -Rec xdet_cena_taker_a -Ref 4842 [-Variants tools\xdet\gate_variants.tsv]
param([Parameter(Mandatory)][string]$Rec, [Parameter(Mandatory)][int]$Ref,
      [string]$Variants = 'tools\xdet\gate_variants.tsv', [int]$WaitSeconds = 900)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$PSScriptRoot\flags.ps1"  # Merge-Flags: a variant's flag replaces the stage A one (a repeat drops flags)
$trace = "$R\out\harness\determinism\trace"
$base = Merge-Flags (Get-StageAFlags) ("--game_data_root=$R\game_data\iso\wwe2k14_test.iso --det_serial_trace=$trace " +
                                       "--wwe2k14_replay_play=$Rec")
$script = "$R\tools\xdet\cena_taker_a.txt"
$names = @(); $scripts = @(); $extras = @()
foreach ($line in Get-Content (Join-Path $R $Variants)) {
  if ($line -match '^\s*(#|$)') { continue }
  $name, $vflags = $line -split "`t", 2
  $names += $name; $scripts += $script; $extras += (Join-Flags (Merge-Flags $base $vflags))
}
"[gate] variants: $($names -join ', ')"
$o = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras -Parallel 3 -WaitSeconds $WaitSeconds 2>&1
# run_list's result lines come in input order (one per script, an ABORT line too): index i = variant i.
$results = @($o | Where-Object { "$_" -match '^\S+ -> ' })
$o | Where-Object { "$_" -match '->' } | ForEach-Object { "  $_" }
$runs = @($results | ForEach-Object { if ("$_" -match '-> wwe2k14_(\d+)') { [int]$Matches[1] } else { 0 } })
# Session 77: each variant's own flags must show in its run's settings line ('# xdet modified:' in the det CSV): a
# variant whose flags never reached the game is the reference again and proves nothing (a flag-merge bug did that).
function Get-VariantMissing([int]$run, [string]$vflags) {
  $csv = "$R\out\harness\determinism\det_$run.csv"
  if (-not (Test-Path $csv)) { return 'no det CSV' }
  $line = Get-Content $csv -TotalCount 12 | Where-Object { $_ -like '# xdet modified:*' } | Select-Object -First 1
  if (-not $line) { return 'no "# xdet modified" line' }
  $have = @{}
  foreach ($kv in ($line.Substring('# xdet modified:'.Length).Trim() -split '\s+')) { $k, $v = $kv -split '=', 2; $have[$k] = $v }
  # (the CSV writer turns commas and spaces in a value into '_')
  $miss = @(Split-Flags $vflags | Where-Object { $_ -match '^--([^=]+)=(.*)$' -and
                                                  $have[$Matches[1]] -ne ($Matches[2] -replace '[,\s]', '_') })
  return ($miss -join ' ')
}
# Render-only settings the variants change (anything else differing still makes the pair INVALID).
$allow = '^(render_|readback_|wwe2k14_use_launcher_settings|wwe2k14_settings_file|wwe2k14_aspect|wwe2k14_preset_|' +
         'resolution_|gpu_|vulkan_|wwe2k14_light_renderer|wwe2k14_force_no_msaa)'
# (Session 77: wwe2k14_scene_crowd is NOT allowed: it skips the game's own draw-recorder calls for crowd members, so the
# guest's render lists differ. Stage A pins it off on every device, stageA.flags; it belongs to stage B.)
$vflagsOf = @{}
foreach ($line in Get-Content (Join-Path $R $Variants)) {
  if ($line -match '^\s*(#|$)') { continue }
  $n, $f = $line -split "`t", 2
  $vflagsOf[$n] = $f
}
for ($i = 0; $i -lt $names.Count; $i++) {
  if ($i -ge $runs.Count -or -not $runs[$i]) { "{0,-16} NOT RUN: {1}" -f $names[$i], $(if ($i -lt $results.Count) { $results[$i] }); continue }
  $missing = Get-VariantMissing $runs[$i] $vflagsOf[$names[$i]]
  $v = py "$R\tools\determinism\det_compare.py" --cross $Ref $runs[$i] --allow $allow 2>&1 |
       Select-String -Pattern '^VERDICT' | Select-Object -First 1
  if ($missing) { $v = "VARIANT NOT APPLIED (missing: $missing); compare said: $v" }
  "{0,-16} run {1}: {2}" -f $names[$i], $runs[$i], $v
}
