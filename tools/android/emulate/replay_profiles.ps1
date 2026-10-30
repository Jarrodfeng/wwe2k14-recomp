# Session 73: phone GPUs on the PC, same-frame version: the s21_lb replay (two entrances + the match) with
# short\s72_swap_shots.txt's screenshots, at the phone's settings (NATIVE 720p settings file, host render targets),
# once per profile line ("NAME<TAB>FLAGS"), 3 at a time, normal speed (the route says realtime). ~3.5 min.
#   & .\tools\android\emulate\replay_profiles.ps1 -Profiles tools\android\emulate\profiles_X.tsv
param([Parameter(Mandatory)][string]$Profiles)
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot))
$route = Join-Path $root 'tools\input_scripts\short\s72_swap_shots.txt'
$base = '--wwe2k14_replay_play=s21_lb --wwe2k14_use_launcher_settings=true ' +
        "--wwe2k14_settings_file=$root\out\s72\settings_native720.json --render_target_path_vulkan=fbo"
$scripts = @(); $extras = @(); $names = @()
foreach ($line in Get-Content (Join-Path $root $Profiles)) {
  if ($line -match '^\s*(#|$)') { continue }
  $name, $flags = $line -split "`t", 2
  $names += $name; $scripts += $route; $extras += ("$base $flags").Trim()
}
Write-Host "[profiles] $($names -join ', ')"
& (Join-Path $root 'tools\bench\run_list.ps1') -Scripts $scripts -ScriptExtras $extras -Parallel 3
