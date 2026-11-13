# Session 101 lane 198: dry-run every row of the coverage sweep (SESSION101_PLAN.md "Coverage sweep (stretch, after Check K)") through the door.
# Starts NOTHING (run.py --dry-run): prints the VERDICT per row. The door's rule 3 refuses an unknown flag name, so a typo shows here.
# Usage:  & .\tools\run\sweep_dry.ps1            (also prints the exact flag line per row with -Show)
param([switch]$Show)
$R = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$M = $R
$common = (& git -C $R rev-parse --git-common-dir 2>$null)
if ($common) { $M = Split-Path ([System.IO.Path]::GetFullPath([string]$common, $R)) -Parent }
$S720 = Join-Path $M 'out\owned_check\settings_native720.json'
$S1440 = Join-Path $M 'out\owned_check\settings_native1440.json'
$S120 = Join-Path $M 'out\s101\settings_native720_hz120.json'
$PH = Join-Path $M 'out\s101\sweep_photos'
# J+ = Check J's J1 flag line + the Check K additions (SESSION101_PLAN.md K.B.4); validation level 1
$J = '--wwe2k14_rdr_owner=true --wwe2k14_scene_records=true --wwe2k14_scene_light_states=true --wwe2k14_cost_counters=true --wwe2k14_rdr_crowd_ours=true --wwe2k14_rdr_crowd_floor=true --wwe2k14_rdr_crowd_skip_skin=true --wwe2k14_rdr_crowd_cards=true --wwe2k14_rdr_crowd_props_ours=true --wwe2k14_rdr_arena_ours=true --wwe2k14_rdr_arena_thin=true --wwe2k14_rdr_wrestlers_ours=true --wwe2k14_rdr_wrestlers_thin=true --wwe2k14_rdr_ring_ours=true --wwe2k14_rdr_ring_thin=true --wwe2k14_rdr_flash_ours=true --wwe2k14_rdr_props_ours=true --wwe2k14_rdr_particles_ours=true --wwe2k14_rdr_ui_ours=true --wwe2k14_rdr_ui_text_ours=true --wwe2k14_rdr_shadows_ours=true --wwe2k14_rdr_rim_ours=true --wwe2k14_rdr_screens_ours=true --wwe2k14_rdr_post_ours=true --wwe2k14_rdr_movie_ours=true --wwe2k14_rdr_live_validation=1 --wwe2k14_rdr_ui_quads_ours=true --wwe2k14_rdr_small_passes_ours=true --wwe2k14_rdr_circles_ours=true --wwe2k14_rdr_rim_chain_ours=true'
$V0 = '--wwe2k14_rdr_live_validation=0'       # the same line with the validation layer off (a few long rows)
$SC = '--wwe2k14_rdr_shadow_casters_ours=true'
$NOSC = '--wwe2k14_rdr_shadow_casters_ours=false --wwe2k14_rdr_plane_shadow_ours=true'
$CASE = '--wwe2k14_case_window_s=30 --wwe2k14_case_mb=2048'
$P2 = '--wwe2k14_launch_participants=139,343'
$P4 = '--wwe2k14_launch_participants=139,343,161,324'
$P3 = '--wwe2k14_launch_participants=139,343,161'
# row name = @(route, extra flags, validation (1 on / 0 off), expected board item)
$rows = [ordered]@{
  'SW01 universe'            = @('scenes\s101_sweep_universe.txt',      "$SC --wwe2k14_settings_file=$S720", 1, 'Universe')
  'SW02 caw pages'           = @('scenes\s101_sweep_caw_pages.txt',     "$SC --wwe2k14_rdr_caw_ours=true --wwe2k14_settings_file=$S720", 1, 'Create modes')
  'SW03 match long PLANE'    = @('scenes\s101_owner_match_long.txt',    "$NOSC --wwe2k14_settings_file=$S720", 1, 'Exhibition matches')
  'SW04 match pause (owner)' = @('scenes\s101_sweep_match_pause.txt',   "$SC --wwe2k14_settings_file=$S720 --wwe2k14_photo_dir=$PH", 1, 'Exhibition matches')
  'SW05 type 60 steel cage'  = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=60 $P2", 0, 'Exhibition matches')
  'SW06 type 49 HIAC'        = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=49 $P2", 1, 'Exhibition matches')
  'SW07 type 55 ladder A44'  = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=55 --wwe2k14_launch_arena=44 $P2", 0, 'Exhibition matches')
  'SW08 type 68 table'       = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=68 $P2", 0, 'Exhibition matches')
  'SW09 type 3 tag (4 men)'  = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=3 $P4", 0, 'Exhibition matches')
  'SW10 type 81 inferno'     = @('scenes\s101_sweep_match_type.txt',    "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=81 $P2", 0, 'Exhibition matches')
  'SW11 fatal4 + 4 entrances' = @('scenes\s101_roster_fatal4.txt',      "$SC --wwe2k14_settings_file=$S720", 1, 'Exhibition matches')
  'SW12 victory PLANE'       = @('scenes\s101_sweep_victory.txt',       "$NOSC --wwe2k14_settings_file=$S720 $CASE", 1, 'Victory scenes')
  'SW13 victory'             = @('scenes\s101_sweep_victory.txt',       "$SC --wwe2k14_settings_file=$S720 $CASE", 1, 'Victory scenes')
  'SW14 entrance Taker'      = @('scenes\s101_sweep_entrance_taker.txt', "$SC --wwe2k14_settings_file=$S720", 1, 'Entrances')
  'SW14b entrance Taker PLANE' = @('scenes\s101_sweep_entrance_taker.txt', "$NOSC --wwe2k14_settings_file=$S720", 1, 'Entrances')   # lane 251: the plane pass DOES run in entrances (plane images made 9-20 in the 04:11 zips) and no plane-arm entrance run existed: the board cell read stream
  'SW15 wm30 long PLANE'    = @('scenes\s101_sweep_wm30_long.txt',     "$NOSC --wwe2k14_settings_file=$S720", 1, '30 Years')
  'SW16 wm30 long'           = @('scenes\s101_sweep_wm30_long.txt',     "$SC --wwe2k14_settings_file=$S720", 0, '30 Years')
  'C1 menus backup (cond.)'  = @('short\s100_owner_boot_menu_quit.txt', "$SC --wwe2k14_settings_file=$S720", 1, 'Menus')
  'C2 Taker 21:9 (cond.)'    = @('scenes\s101_sweep_entrance_taker.txt', "$SC --wwe2k14_aspect=21:9 --wwe2k14_settings_file=$S1440", 1, 'Entrances')
  'C3 type 34 battle royal (cond.)' = @('scenes\s101_sweep_match_type.txt', "$SC --wwe2k14_settings_file=$S720 --wwe2k14_quick_type=34 --wwe2k14_launch_participants=139,343,161,324,131,100", 0, 'Exhibition matches')
  'C4 match long 3rd window (cond.)' = @('scenes\s101_owner_match_long.txt', "$SC --wwe2k14_settings_file=$S720", 1, 'Exhibition matches')
}
foreach ($k in $rows.Keys) {
  $row = $rows[$k]
  $route = Join-Path $R ('tools\input_scripts\' + $row[0])
  $jf = if ($row[2] -eq 1) { $J } else { $J.Replace('--wwe2k14_rdr_live_validation=1', $V0) }
  $flags = ($jf + ' ' + $row[1]) -split '\s+' | Where-Object { $_ }
  Write-Host "### $k  ($($row[0]); board item: $($row[3]); validation $($row[2]))"
  if ($Show) { Write-Host ('    flags: ' + ($flags -join ' ')) }
  $out = & py -I (Join-Path $R 'tools\run\run.py') $route --dry-run @flags 2>&1
  $out | Select-String -Pattern 'REFUSED|VERDICT|WARN|ERROR' | ForEach-Object { Write-Host ('   ' + $_.Line.Trim().Substring(0, [Math]::Min(220, $_.Line.Trim().Length))) }
}
