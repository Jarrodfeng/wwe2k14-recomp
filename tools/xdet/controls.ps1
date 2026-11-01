# Session 77 (XDET_PLAN.md verification rule 2): sensitivity controls for the cross-device comparer. Each control
# changes something on purpose and must be caught by tools\determinism\det_compare.py --cross against a reference
# play of the same recording (all with the stage A flags; 3 lanes at once):
#   poke     --wwe2k14_det_poke flips the lowest bit of Cena's position x (slot 0, [char+176]) at frame -PokeFrame:
#            PASS = the first SIM difference is at exactly that frame (and the run is marked INVALID from it: a test).
#   noinput  the recording without one input (-NoInput NAME, made by hand: one 'S' line removed): PASS = DIVERGED in
#            a game layer (SIM, or LAYOUT with a value class: whole objects hold values too) within 60 frames after the
#            removed input. The input must come after player control starts (~frame 2640 in stage A): the pad
#            recording without its walk left at 2679 (tools\xdet\refs\xdet_cena_taker_pad_nowalk2679.rec, copy it to
#            out\replays) diverged at 2685 (run 4862); earlier removals changed nothing lasting (run 4849, 4853).
#   timeout  --det_serial_vblank_timeout_ms=1: PASS = INVALID with a det_serial vblank timeout.
#   pwsh -NoProfile -File tools\xdet\controls.ps1 -Rec xdet_cena_taker_pad -Ref 4855
#        [-NoInput xdet_cena_taker_pad_nowalk2679 -NoInputFrame 2679] [-PokeFrame 3000]
param([Parameter(Mandatory)][string]$Rec, [Parameter(Mandatory)][int]$Ref, [int]$PokeFrame = 3000,
      [string]$NoInput = '', [int]$NoInputFrame = 0, [int]$WaitSeconds = 900)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$PSScriptRoot\flags.ps1"  # Merge-Flags: a control's flag replaces the stage A one (a repeat drops flags)
$base = Merge-Flags (Get-StageAFlags) ("--game_data_root=$R\game_data\iso\wwe2k14_test.iso " +
                                       "--det_serial_trace=$R\out\harness\determinism\trace --wwe2k14_det_hash_raw_every=60")
$script = "$R\tools\xdet\cena_taker_a.txt"
$names = @('poke', 'timeout')
$extras = @((Join-Flags (Merge-Flags $base "--wwe2k14_replay_play=$Rec --wwe2k14_det_poke=${PokeFrame}:0x836F38D0>176>0:1")),
            (Join-Flags (Merge-Flags $base "--wwe2k14_replay_play=$Rec --det_serial_vblank_timeout_ms=1")))
if ($NoInput) { $names += 'noinput'; $extras += (Join-Flags (Merge-Flags $base "--wwe2k14_replay_play=$NoInput")) }
$scripts = @(); foreach ($n in $names) { $scripts += $script }
$o = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras -Parallel 3 -WaitSeconds $WaitSeconds 2>&1
$runs = @($o | Where-Object { "$_" -match '-> wwe2k14_(\d+)' } | ForEach-Object { [void]("$_" -match 'wwe2k14_(\d+)'); [int]$Matches[1] })
$fails = 0
for ($i = 0; $i -lt $runs.Count; $i++) {
  $out = py "$R\tools\determinism\det_compare.py" --cross $Ref $runs[$i] 2>&1
  $verdict = "$($out | Select-String -Pattern '^VERDICT' | Select-Object -First 1)"
  $sim = "$($out | Select-String -Pattern '^\s+SIM\s' | Select-Object -First 1)"
  $pass = switch ($names[$i]) {
    'poke' { $sim -match "SIM\s+$PokeFrame \(" -and $verdict -match 'TEST poke' }
    'timeout' { $verdict -match 'INVALID' -and ($out -match 'det_serial vblank timeout') }
    'noinput' { $verdict -match "DIVERGED at frame (\d+): layer (SIM|LAYOUT), \w+: value" -and
                [int]$Matches[1] -ge $NoInputFrame -and [int]$Matches[1] -le $NoInputFrame + 60 }
  }
  if (-not $pass) { $fails++ }
  "{0,-8} run {1}: {2}`n         {3}`n         {4}" -f $names[$i], $runs[$i], $(if ($pass) { 'PASS' } else { 'FAIL' }), $verdict, $sim.Trim()
}
"controls: $($runs.Count - $fails) / $($runs.Count) PASS"
exit $fails
