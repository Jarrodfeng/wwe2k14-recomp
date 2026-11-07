# Session 92 (M6 piece C): the check's precompile - the game exe in its precompile mode (--wwe2k14_precompile) at ONE
# configuration, filling a translation store (545408B2.vk.xtc) from the shader list's V lines. Under the run lock (through
# tools\run_and_triage.ps1), on a frozen user data copy and a copy of the settings file, never the real ones.
# The census / owned_check runs that follow read the store with
#   "--flags=--gpu_m6_store_path=<store> --gpu_m6_store_append=false"
# Run it once per configuration (e.g. 1440p = the census settings, and 720p = owned_check's other scale): a different
# resolution is a different translator configuration, so a different set of keys in the same file.
#
#   & C:\Users\Jarrod\wwe2k14-recomp\tools\shaders\m6_precompile.ps1 -Tag p1440 [-Settings FILE] [-List FILE] [-Store FILE]
#       [-Fresh] [-Exe FILE] [-WaitSeconds 900] [-ExtraArgs "--gpu_native_vertex_input=2"]  (Session 95: the attribute forms are made only at switch 2)
#
#   -Tag       names the run's files under out\m6\ (settings_<tag>.json, userdata_<tag>\, progress_<tag>.json)
#   -Settings  launcher settings JSON for the run (default out\owned_check\settings_native1440.json = the census's;
#              720p: the owned_check 720 settings file). TRUE NATIVE must be on in it (the store switch rides on it).
#   -List      the shader list to import (default out\s92\shader_list_m6.txt = the census list + the V lines; NOT shipped)
#   -Store     the translation store file (default out\m6\store\545408B2.vk.xtc: shared by every tag/configuration)
#   -Fresh     delete the store first (a cold start; the user data is mirrored from userdata_frozen on every run, so the xvar and .xsh are rebuilt by the import)
# Prints the run's [m6] lines and the precompile's end state. 1 game run per call.
param([Parameter(Mandatory)][string]$Tag,
      [string]$Settings = 'C:\Users\Jarrod\wwe2k14-recomp\out\owned_check\settings_native1440.json',
      [string]$List = 'C:\Users\Jarrod\wwe2k14-recomp\out\s92\shader_list_m6.txt',
      [string]$Store = 'C:\Users\Jarrod\wwe2k14-recomp\out\m6\store\545408B2.vk.xtc',
      [switch]$Fresh,
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\wwe2k14.exe',
      [int]$WaitSeconds = 900,
      [string[]]$ExtraArgs = @())
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$m6 = "$R\out\m6"
$work = "$m6\userdata_$Tag"
$set = "$m6\settings_$Tag.json"
$progress = "$m6\progress_$Tag.json"
foreach ($p in @($Settings, $List, $Exe)) { if (-not (Test-Path $p)) { Write-Host "m6_precompile: missing $p"; exit 2 } }
New-Item -ItemType Directory -Force -Path (Split-Path $Store -Parent) | Out-Null
if ($Fresh) {
    Remove-Item -Force -ErrorAction SilentlyContinue $Store
}
# The run's user data is a fresh mirror of the frozen copy every time, exactly as tools\bench\run_list.ps1 does (CLAUDE.md Safety):
# never a reused copy, never Jarrod's real save.
robocopy "$R\out\bench\userdata_frozen" $work /MIR /NFL /NDL /NJH /NJS | Out-Null
Copy-Item -Force $Settings $set
Remove-Item -Force -ErrorAction SilentlyContinue $progress
$args1 = @("--user_data_root=$work", "--cache_root=$work\cache", "--wwe2k14_d3d_owned=true", "--wwe2k14_native_frame=true",
           "--wwe2k14_use_launcher_settings=true", "--wwe2k14_settings_file=$set", "--wwe2k14_dlc=true",
           "--wwe2k14_replay_dir=$R\out\replays", "--wwe2k14_scenario_dir=$R\out\scenarios",
           "--wwe2k14_shader_list=$List", "--wwe2k14_precompile=$progress", "--gpu_m6_store_path=$Store",
           "--window_activate_when_shown=false", "--window_show_behind=true", "--wwe2k14_perf_report=false",
           "--wwe2k14_discord=false")
$args1 += $ExtraArgs
$out = & "$R\tools\run_and_triage.ps1" -WaitSeconds $WaitSeconds -KillAfterWait -Exe $Exe -ExtraArgs $args1
$out | ForEach-Object { Write-Host $_ }
$log = $null
foreach ($l in $out) { if ("$l" -match 'wwe2k14_(\d+)\.log') { $log = Join-Path (Split-Path $Exe -Parent) "logs\wwe2k14_$($Matches[1]).log" } }
if ($log -and (Test-Path $log)) {
    Write-Host "--- $log"
    Select-String -Path $log -Pattern '\[m6\]\[(precompile|store|miss|verify)\]|\[wwe2k14\]\[(shaders|precompile)\]|translator runs' | Select-Object -First 60 | ForEach-Object { $_.Line }
}
if (Test-Path $progress) { Write-Host "progress: $(Get-Content -Raw $progress)" }
if (Test-Path $Store) { Write-Host "store: $Store $((Get-Item $Store).Length) bytes" }
