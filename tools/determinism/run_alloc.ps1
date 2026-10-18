# Task A: boot the game -Count times with the allocation trace on (src/fp_probe.h, --wwe2k14_alloc_trace_frames)
# and the fixed seed, under one run-lock hold. -Build NAME first runs tools\build.ps1 -Run NAME inside the same hold.
# Then compare two boots with different 0x836F3AAC values (the "[alloctrace] stopped" log line) with
#   py tools\determinism\alloc_compare.py A B
# -MoreArgs adds settings to every boot (one string, e.g. '--wwe2k14_det_boot_alloc=true').
# Usage: pwsh -NoProfile -File tools\determinism\run_alloc.ps1 -Count 5 [-Frames 120] [-Seed 1234] [-Build s8_tA_2]
param([int]$Count = 5, [int]$Frames = 120, [long]$Seed = 1234, [string]$Build = '', [string]$MoreArgs = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\runlock.ps1"
$lock = Enter-RunLock -Name "run_alloc x$Count"
try {
    if ($Build) {
        & pwsh -NoProfile -File "$R\tools\build.ps1" -Run $Build
        if ($LASTEXITCODE -ne 0) { Write-Output "build failed ($LASTEXITCODE)"; exit 1 }
    }
    $scripts = @()
    for ($i = 0; $i -lt $Count; $i++) { $scripts += '..\..\..\tools\determinism\alloc_boot.txt' }
    $extra = "--wwe2k14_fixed_seed=$Seed --wwe2k14_alloc_trace_frames=$Frames"
    if ($MoreArgs) { $extra = "$extra $MoreArgs" }
    & "$R\tools\bench\run_list.ps1" -Scripts $scripts -Extra $extra -WaitSeconds 180
} finally { Exit-RunLock $lock }
