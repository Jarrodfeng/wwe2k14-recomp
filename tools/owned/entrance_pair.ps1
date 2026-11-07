# The entrance pair on its own (Session 95; the same two runs as integrate.ps1's step 5): the quick entrance replay, owned ON flags vs
# the emulated reference, 720p, in one batch, then the verdict. 2 game runs.
#   & .\tools\owned\entrance_pair.ps1 -OnFlags '--wwe2k14_d3d_owned=true' [-Exe X]
param([string]$OnFlags = '--wwe2k14_d3d_owned=true', [string]$Exe = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $R
$exePath = if ($Exe) { $Exe } else { "$R\out\build\win-amd64-release\wwe2k14.exe" }
$set = "$R\out\owned_check\settings_native720.json"
$ent = 'tools\input_scripts\short\s82_ent_shots.txt'
$common = "--wwe2k14_replay_play=s82_lb_quick_ent --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false --game_data_root=$R\game_data\extracted --wwe2k14_replay_dir=$R\out\replays --wwe2k14_scenario_dir=$R\out\scenarios"
$extras = @("--wwe2k14_settings_file=$set $OnFlags", "--wwe2k14_settings_file=$set --gpu_native_own_steps=0 --gpu_native_owned_frame=false")
$eo = @(& "$R\tools\bench\run_list.ps1" -Scripts @($ent, $ent) -ScriptExtras $extras -Parallel 2 -Extra $common -Exe $exePath 2>&1)
$eo | Where-Object { "$_" -match ' -> wwe2k14_\d+' } | ForEach-Object { "$_" }
$eruns = @($eo | ForEach-Object { if ("$_" -match ' -> wwe2k14_(\d+)') { $Matches[1] } })
if ($eruns.Count -ne 2) { "entrance pair: expected 2 runs, got $($eruns.Count)"; exit 7 }
py -I "$R\tools\native\owned_verdict.py" --dir (Split-Path -Parent $exePath) @eruns
