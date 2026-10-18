# Session 16 item 1: replay check. Records one run of tools\replay\replay_check.txt with --wwe2k14_replay_record=NAME
# (controller 1 plays Lesnar in lesnar_bryan, entrances skipped), then plays it back -Plays times with
# --wwe2k14_replay_play=NAME (no launch flags: the recording carries them), all fast-forwarded (run_list default),
# one game at a time, and runs tools\determinism\det_compare.py on record vs play 1 and play 1 vs play 2.
#   pwsh -NoProfile -File tools\replay\run_replay_check.ps1 [-Name replay_check] [-Plays 2] [-More '--x=1'] [-PlayOnly]
# -PlayOnly: skip the recording (play NAME.rec again). Recordings: out\replays\NAME.rec (never overwritten: NAME_2...).
param([string]$Name = 'replay_check', [int]$Plays = 2, [string]$More = '', [switch]$PlayOnly,
      [string]$Script = '', [int]$WaitSeconds = 600)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$script = if ($Script) { (Resolve-Path $Script).Path } else { "$R\tools\replay\replay_check.txt" }
$det = '--wwe2k14_det_hash=default,objects --wwe2k14_det_hash_raw_every=30 --resolution_scale=1'
$runs = @()
if (-not $PlayOnly) {
    $rec = "$R\out\replays\$Name.rec"
    if (Test-Path -LiteralPath $rec) { "ABORT: $rec exists (recordings are never overwritten; pick another -Name or use -PlayOnly)"; exit 2 }
    $extra = "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_skip_entrances=true --wwe2k14_launch_participants=343:0,324 --wwe2k14_replay_record=$Name $det"
    if ($More) { $extra += " $More" }
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra $extra -WaitSeconds $WaitSeconds 2>&1
    $out | Where-Object { "$_" -match '->' } | ForEach-Object { "record: $_" }
    $runs += @($out | Where-Object { "$_" -match '-> wwe2k14_(\d+)' } | ForEach-Object { [void]("$_" -match 'wwe2k14_(\d+)'); [int]$Matches[1] })
}
$scripts = @(); for ($i = 0; $i -lt $Plays; $i++) { $scripts += $script }
$extra = "--wwe2k14_replay_play=$Name $det"
if ($More) { $extra += " $More" }
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -Extra $extra -WaitSeconds $WaitSeconds 2>&1
$out | Where-Object { "$_" -match '->' } | ForEach-Object { "play: $_" }
$runs += @($out | Where-Object { "$_" -match '-> wwe2k14_(\d+)' } | ForEach-Object { [void]("$_" -match 'wwe2k14_(\d+)'); [int]$Matches[1] })
"runs: $($runs -join ', ')"
for ($i = 0; $i + 1 -lt $runs.Count; $i++) {
    py "$R\tools\determinism\det_compare.py" $runs[$i] $runs[$i + 1] 2>&1 | Out-Null
    "compare: out\harness\determinism\compare_$($runs[$i])_$($runs[$i + 1]).txt"
}
