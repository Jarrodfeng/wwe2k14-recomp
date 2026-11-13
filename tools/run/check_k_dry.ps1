# Session 101 lane 173: dry-run every Check K run through the door (SESSION101_PLAN.md "Check K (planned)", K.3).
# Starts NOTHING (run.py --dry-run): prints the verdict per run. The door's rule 3 refuses an unknown flag name, so a typo shows here.
# Usage:  & .\tools\run\check_k_dry.ps1
$R = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# out\ lives in the MAIN checkout (gitignored): from a worktree take the parent of the git common dir
$M = $R
$common = (& git -C $R rev-parse --git-common-dir 2>$null)
if ($common) { $M = Split-Path ([System.IO.Path]::GetFullPath([string]$common, $R)) -Parent }
$S720 = Join-Path $M 'out\owned_check\settings_native720.json'
$S1440 = Join-Path $M 'out\owned_check\settings_native1440.json'
$S120 = Join-Path $M 'out\s101\settings_native720_hz120.json'
if (-not (Test-Path $S120)) {
  New-Item -ItemType Directory -Force -Path (Split-Path $S120) | Out-Null
  Set-Content -Path $S120 -Value '{"quality_tier": 2, "renderer": 2, "frame_rate": 2, "refresh_hz": 120, "resolution": 0}' -NoNewline
  Write-Host "made $S120"
}
$J = '--wwe2k14_rdr_owner=true --wwe2k14_scene_records=true --wwe2k14_scene_light_states=true --wwe2k14_cost_counters=true --wwe2k14_rdr_crowd_ours=true --wwe2k14_rdr_crowd_floor=true --wwe2k14_rdr_crowd_skip_skin=true --wwe2k14_rdr_crowd_cards=true --wwe2k14_rdr_crowd_props_ours=true --wwe2k14_rdr_arena_ours=true --wwe2k14_rdr_arena_thin=true --wwe2k14_rdr_wrestlers_ours=true --wwe2k14_rdr_wrestlers_thin=true --wwe2k14_rdr_ring_ours=true --wwe2k14_rdr_ring_thin=true --wwe2k14_rdr_flash_ours=true --wwe2k14_rdr_props_ours=true --wwe2k14_rdr_particles_ours=true --wwe2k14_rdr_ui_ours=true --wwe2k14_rdr_ui_text_ours=true --wwe2k14_rdr_shadows_ours=true --wwe2k14_rdr_rim_ours=true --wwe2k14_rdr_screens_ours=true --wwe2k14_rdr_post_ours=true --wwe2k14_rdr_movie_ours=true --wwe2k14_rdr_live_validation=1 --wwe2k14_rdr_ui_quads_ours=true --wwe2k14_rdr_small_passes_ours=true --wwe2k14_rdr_circles_ours=true --wwe2k14_rdr_rim_chain_ours=true'
$SC = '--wwe2k14_rdr_shadow_casters_ours=true'
$NOSC = '--wwe2k14_rdr_shadow_casters_ours=false --wwe2k14_rdr_plane_shadow_ours=true'
$runs = [ordered]@{
  'K1 verify + 6-Pack' = @('scenes\s101_roster_classes.txt', "$SC --wwe2k14_settings_file=$S720 --wwe2k14_rdr_arena_d3=true --wwe2k14_rdr_arena_d3_verify=true --wwe2k14_rdr_crowd_bake_skip=1")
  'K7 wm30 census'     = @('scenes\s100_owner_s90_wm30.txt', "$SC --wwe2k14_settings_file=$S720 --wwe2k14_rdr_crowd_bake_skip=1")
  'K2 D3 + Cena AA'    = @('scenes\s101_checkK_cena_aa.txt', "$SC --wwe2k14_settings_file=$S720 --wwe2k14_rdr_arena_d3=true --wwe2k14_rdr_crowd_bake_skip=1 --wwe2k14_case_window_s=30 --wwe2k14_case_mb=2048")
  'K3 Brock + plane'   = @('short\s100_owner_s89_brock.txt', "$NOSC --wwe2k14_settings_file=$S720")
  'K3b Brock 1440p'    = @('short\s100_owner_s89_brock.txt', "$NOSC --wwe2k14_settings_file=$S1440")
  'K4 finish + logo'   = @('scenes\s101_checkK_finish_logo.txt', "$NOSC --wwe2k14_settings_file=$S120 --wwe2k14_pump_follow_display=false")
  'K5 CAW editor'      = @('scenes\s100_owner_s90_caw_editor.txt', "$SC --wwe2k14_rdr_caw_ours=true --wwe2k14_settings_file=$S120")
}
foreach ($k in $runs.Keys) {
  $route = Join-Path $R ('tools\input_scripts\' + $runs[$k][0])
  $flags = ($J + ' ' + $runs[$k][1]) -split '\s+' | Where-Object { $_ }
  Write-Host "### $k  ($($runs[$k][0]))"
  $out = & py -I (Join-Path $R 'tools\run\run.py') $route --dry-run @flags 2>&1
  $out | Select-String -Pattern 'REFUSED|VERDICT|WARN' | ForEach-Object { Write-Host ('   ' + $_.Line.Trim().Substring(0, [Math]::Min(200, $_.Line.Trim().Length))) }
}
