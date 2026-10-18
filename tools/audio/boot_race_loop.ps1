# Session 27 item 1: boots under the SDL audio boot-crash trigger (Sessions 22-23): two games booting at once, then one
# game started right after they close. Each cycle = that pair + the single boot (short\boot_menu_quit.txt: title ->
# main menu -> quit). Counts runs that died with 0xC0000264 (the SDL_UpdateAudio race).
#   pwsh -NoProfile -File tools\audio\boot_race_loop.ps1 -Cycles 30 [-Extra '--audio_sdl_init_on_ui_thread=false'] [-StopOnCrash] -Tag T
# Writes out\s27\boot_race_<Tag>.tsv (cycle, run, role, exit, crash).
param([int]$Cycles = 30, [string]$Extra = '', [switch]$StopOnCrash, [string]$Tag = 'loop')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$route = Join-Path $repo 'tools\input_scripts\short\boot_menu_quit.txt'
$logs = Join-Path $repo 'out\build\win-amd64-release\logs'
$outDir = Join-Path $repo 'out\s27'
New-Item -ItemType Directory -Force $outDir | Out-Null
$tsv = Join-Path $outDir "boot_race_$Tag.tsv"
"cycle`trun`trole`texit`tcrash" | Set-Content $tsv

function Invoke-Runs([int]$count, [string]$role, [int]$cycle) {
  $scripts = @(); for ($k = 0; $k -lt $count; $k++) { $scripts += $route }
  $rl = @{ Scripts = $scripts; WaitSeconds = 180 }
  if ($count -gt 1) { $rl.Parallel = $count }
  if ($Extra) { $rl.Extra = $Extra }
  $out = & (Join-Path $repo 'tools\bench\run_list.ps1') @rl 2>&1 | Out-String
  $crashes = 0
  foreach ($m in [regex]::Matches($out, '-> wwe2k14_(\d+)\s+(\w+)[^\r\n]*?exit code (-?\d+)')) {
    $run = $m.Groups[1].Value
    $log = Join-Path $logs "wwe2k14_$run.log"
    $crash = (Test-Path $log) -and [bool](Select-String -Path $log -Pattern '0xC0000264' -SimpleMatch -Quiet)
    if ($crash) { $crashes++ }
    "$cycle`t$run`t$role`t$($m.Groups[3].Value)`t$([int]$crash)" | Add-Content $tsv
    Write-Host ("cycle {0} {1} run {2}: exit {3}{4}" -f $cycle, $role, $run, $m.Groups[3].Value, $(if ($crash) { '  CRASH 0xC0000264' } else { '' }))
  }
  return $crashes
}

$total = 0; $ran = 0
for ($c = 1; $c -le $Cycles; $c++) {
  $total += Invoke-Runs 2 'pair' $c
  $total += Invoke-Runs 1 'single' $c
  $ran = $c
  if ($StopOnCrash -and $total -gt 0) { break }
}
Write-Host "done: $ran cycle(s), $total crash(es) with 0xC0000264; table $tsv"
