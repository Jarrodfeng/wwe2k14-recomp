# Session 73: phone GPUs on the PC. Runs a short match route (wide_check: launched match, entrances skipped, two
# screenshots) at the phone's settings (LOW preset, host render targets) once per profile, 3 at a time.
# Each profile = a line "NAME<TAB>FLAGS" in the profile file (SDK vulkan_emulate_limits / _disable / _formats).
#   & .\tools\android\emulate\run_profiles.ps1 -Profiles tools\android\emulate\profiles_s73a.tsv [-Route X.txt] [-Extra '...']
param([Parameter(Mandatory)][string]$Profiles, [string]$Route = 'tools\input_scripts\short\wide_check.txt', [string]$Extra = '')
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot))
$base = '--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_skip_entrances=true --wwe2k14_preset_low=true ' +
        '--render_target_path_vulkan=rtv --fullscreen=false --window_width=1280 --window_height=720'
$scripts = @(); $extras = @(); $names = @()
foreach ($line in Get-Content (Join-Path $root $Profiles)) {
  if ($line -match '^\s*(#|$)') { continue }
  $name, $flags = $line -split "`t", 2
  $names += $name; $scripts += (Join-Path $root $Route); $extras += ("$base $flags $Extra").Trim()
}
Write-Host "[profiles] $($names -join ', ')"
& (Join-Path $root 'tools\bench\run_list.ps1') -Scripts $scripts -ScriptExtras $extras -Parallel 3
