# Determinism runs (Session 8, task 8b): the determinism_lesnar route with the per-frame hash on.
# Holds the run lock for the whole seed list (run_list re-enters it), so no other agent's build or run lands between
# the runs of one set. -Seeds lists the --wwe2k14_fixed_seed value per group (-1 = off) as one comma-separated string;
# -Repeat runs the route that many times per seed; -Spec is the --wwe2k14_det_hash value (src/frame_hash.h);
# -RawEvery N dumps the region bytes every N frames (1 = every frame, about 46 KB per frame with the default spec).
# -MoreArgs adds settings to every run (one string, e.g. '--wwe2k14_fp_probe=true'; task A).
# Usage: pwsh -NoProfile -File tools\determinism\run_det.ps1 -Seeds "1234,98765,-1" -Repeat 1
param([string]$Seeds = '1234', [int]$Repeat = 2, [int]$RawEvery = 30, [string]$Spec = 'default,objects',
      [string]$MoreArgs = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = '..\..\..\tools\input_scripts\determinism_lesnar.txt'
. "$R\tools\runlock.ps1"
$lock = Enter-RunLock -Name "run_det seeds=$Seeds x$Repeat"
try {
    foreach ($seed in ($Seeds -split ',' | ForEach-Object { [long]$_.Trim() })) {
        $scripts = @()
        for ($i = 0; $i -lt $Repeat; $i++) { $scripts += $route }
        $extra = "--wwe2k14_det_hash=$Spec --wwe2k14_det_hash_raw_every=$RawEvery"
        if ($seed -ge 0) { $extra = "--wwe2k14_fixed_seed=$seed $extra" }
        if ($MoreArgs) { $extra = "$extra $MoreArgs" }
        Write-Output "=== seed $seed x$Repeat : $extra"
        & "$R\tools\bench\run_list.ps1" -Scripts $scripts -Extra $extra
    }
} finally { Exit-RunLock $lock }
