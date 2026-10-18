# Session 8 task 2: same-seed determinism runs with the racing boot allocation placed at fixed points
# (--wwe2k14_det_boot_alloc_place=N, src/fp_probe.h). One run of the determinism_lesnar route per entry of -Places
# (comma list; "off" = no placement setting), all under ONE exclusive run-lock hold (run_list re-enters it), with the
# fixed seed, the per-frame hash (as run_det.ps1), the distance probe, the skeleton probe (raw dump from
# -DumpFrom for 300 frames) and the allocation trace for the first -AllocFrames frames.
# Compare with: py tools\determinism\det_compare.py A B --slack 1 ; py tools\determinism\fp_compare.py A B ;
#               py tools\determinism\skel_compare.py A B ; py tools\determinism\alloc_place.py A B
# Usage: pwsh -NoProfile -File tools\determinism\run_place.ps1 -Places "0,33,33,0"
param([string]$Places = '0,33', [long]$Seed = 1234, [int]$RawEvery = 30, [string]$Spec = 'default,objects',
      [int]$DumpFrom = 8950, [int]$AllocFrames = 60, [string]$MoreArgs = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = '..\..\..\tools\input_scripts\determinism_lesnar.txt'
. "$R\tools\runlock.ps1"
$lock = Enter-RunLock -Name "run_place places=$Places"
try {
    foreach ($p in ($Places -split ',' | ForEach-Object { $_.Trim() })) {
        $extra = "--wwe2k14_fixed_seed=$Seed --wwe2k14_det_hash=$Spec --wwe2k14_det_hash_raw_every=$RawEvery"
        $extra = "$extra --wwe2k14_fp_probe=true --wwe2k14_skel_probe=true --wwe2k14_skel_probe_dump_from=$DumpFrom"
        $extra = "$extra --wwe2k14_alloc_trace_frames=$AllocFrames"
        if ($p -ne 'off') { $extra = "$extra --wwe2k14_det_boot_alloc_place=$p" }
        if ($MoreArgs) { $extra = "$extra $MoreArgs" }
        Write-Output "=== place $p : $extra"
        & "$R\tools\bench\run_list.ps1" -Scripts @($route) -Extra $extra -Exclusive
    }
} finally { Exit-RunLock $lock }
