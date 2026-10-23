# Session 55 part 2: every Quick Match type played to its finish, all CPU (short\s55_qm_finish.txt), fast-forwarded,
# NATIVE at 360p (the LOW preset's renderer at 720p, drawn at 360p), entrances skipped. Jarrod (2026-10-23): "attempt
# all matches to completion"; game logic, not pictures. Rumble types 20-23 are left out: a built Quick Match of them
# does not load (Session 27, apply draws the entry order from the menu's roster).
#   & .\tools\s55\qm_sweep.ps1 [-Types 53,0,...] [-Parallel 1] [-Step 60] [-Extra '...'] [-Exe COPY\wwe2k14.exe]
# -Exe: a copy of the build in a folder of its own (out\s55\sweep_bin), so the main exe can be rebuilt while the sweep
# runs (it gets --game_data_root; its logs are in that folder's logs\).
# Prints the run numbers, then one report line per run (tools\s55\qm_report.py). In-process only (call with &).
param([int[]]$Types = @(), [int]$Parallel = 1, [int]$Step = 60, [string]$Extra = '', [int]$WaitSeconds = 2400,
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\wwe2k14.exe')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\s55_qm_finish.txt"
# Wrestler counts: the "sides" digits in db\ids\match_types.json, else the menu path (1v1 2, tag / fatal 4-way 4,
# triple threat 3, the chamber 6). Gauntlet 82 with 4 as in Session 31.
$count = @{  # (a plain hashtable: an [ordered] one indexes by position with an int)
    0 = 2; 3 = 4; 4 = 4; 7 = 4; 8 = 6; 9 = 4; 10 = 4; 11 = 6; 12 = 4; 13 = 3; 14 = 4; 15 = 3; 16 = 3; 17 = 4; 18 = 4
    19 = 5; 26 = 2; 27 = 3; 28 = 4; 29 = 4; 30 = 3; 31 = 4; 33 = 2; 34 = 4; 35 = 6; 39 = 6; 40 = 6; 49 = 2; 50 = 4
    51 = 3; 52 = 4; 53 = 6; 54 = 2; 55 = 2; 56 = 4; 57 = 3; 58 = 4; 59 = 2; 60 = 2; 61 = 4; 62 = 3; 63 = 4; 67 = 2
    68 = 2; 69 = 4; 70 = 3; 71 = 4; 72 = 2; 73 = 4; 74 = 3; 75 = 4; 76 = 6; 77 = 2; 78 = 4; 79 = 3; 80 = 4; 81 = 2
    82 = 4
}
# Brock Lesnar, Daniel Bryan, John Cena, Undertaker, Randy Orton, CM Punk (db\ids\characters.json, high).
$pool = @(343, 324, 139, 103, 161, 131)
$mixed = '139,218,298,229'  # type 7: Cena + AJ Lee vs The Miz + Kaitlyn (teams = even / odd slots)
$common = "--wwe2k14_launch=@quick --wwe2k14_launch_skip_entrances=true --wwe2k14_preset_low=true " +
          "--wwe2k14_step_hz=$Step --gpu_native_truth_draw_height=360"
if ($Exe -notlike "$R\out\build\*") { $common += " --game_data_root=$R\game_data\extracted" }
if (-not $Types.Count) { $Types = @($count.Keys | Sort-Object) }
$scripts = @(); $extras = @()
foreach ($t in $Types) {
    if (-not $count.Contains($t)) { Write-Warning "type $t not in the list; skipped"; continue }
    $ids = if ($t -eq 7) { $mixed } else { ($pool[0..($count[$t] - 1)]) -join ',' }
    $scripts += $route
    $extras += "$common --wwe2k14_quick_type=$t --wwe2k14_launch_participants=$ids $Extra".Trim()
}
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras -Parallel $Parallel -WaitSeconds $WaitSeconds -Exe $Exe 2>&1
$runs = @($out | Select-String ' -> wwe2k14_(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$out | Select-String 'checks:|CRASH|crash|refus' | ForEach-Object { $_.Line }
if ($runs.Count) { & py "$R\tools\s55\qm_report.py" --logs (Join-Path (Split-Path -Parent $Exe) logs) @runs }
