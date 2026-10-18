# Session 16 item 7: wrestler / referee masks for a route's screenshots. Runs the route twice in replay mode (same seed,
# so the frames are the same), both with --wwe2k14_matte=depth; the second pass also skips the character vertex
# shaders (tools\matte\character_vs.txt, classified in runs 1292 / 1294) from the start. Then for every screenshot
# label: tools\matte\depth.py (depth + scene colour PNGs) and tools\matte\mask.py (NNN_LABEL_mask.png, _mask_overlay.png).
#   pwsh -NoProfile -File tools\matte\mask_pass.ps1 -Script tools\input_scripts\short\matte_check.txt -Extra '--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_skip_entrances=true --wwe2k14_cinecam=true' [-Seed 1234] [-Replay NAME]
# -Replay NAME plays out\replays\NAME.rec instead of a fixed seed (both passes).
param([Parameter(Mandatory)][string]$Script, [string]$Extra = '', [int]$Seed = 1234, [string]$Replay = '',
      [int]$WaitSeconds = 900)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$skip = ([IO.File]::ReadAllText("$R\tools\matte\character_vs.txt")).Trim()
$base = if ($Replay) { "--wwe2k14_replay_play=$Replay" } else { "--wwe2k14_replay_mode=true --wwe2k14_fixed_seed=$Seed" }
$common = "$base --wwe2k14_matte=depth --resolution_scale=1 $Extra".Trim()
$scriptPath = (Resolve-Path $Script).Path
$runs = @()
foreach ($pass in @('', "--gpu_debug_skip_vertex_shader=$skip")) {
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($scriptPath) -Extra "$common $pass".Trim() -WaitSeconds $WaitSeconds 2>&1
    $line = @($out | Where-Object { "$_" -match '->' }) | Select-Object -Last 1
    "$(if ($pass) { 'B (characters skipped)' } else { 'A' }): $line"
    if ("$line" -match 'wwe2k14_(\d+)') { $runs += [int]$Matches[1] }
}
if ($runs.Count -ne 2) { 'ABORT: need two runs'; exit 1 }
$cap = "$R\out\build\win-amd64-release\captures"
$labels = Get-ChildItem -LiteralPath $cap -Filter ("{0:D3}_*_matte.json" -f $runs[0]) | ForEach-Object { $_.Name -replace '^\d+_', '' -replace '_matte\.json$', '' }
foreach ($l in $labels) {
    $a = Join-Path $cap ("{0:D3}_{1}" -f $runs[0], $l)
    $b = Join-Path $cap ("{0:D3}_{1}" -f $runs[1], $l)
    if (-not (Test-Path -LiteralPath "${b}_matte.json")) { "  ${l}: no B dump"; continue }
    py "$R\tools\matte\depth.py" "${a}_depth_0.bin" --far-view 8000 | Out-Null
    py "$R\tools\matte\mask.py" $a $b
}
