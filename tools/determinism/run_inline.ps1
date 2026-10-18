# Session 8 task 2: test --wwe2k14_det_inline_motion (src/fp_probe.h) under ONE exclusive run-lock hold:
# optional build (-Build NAME), optional flags-off regress selftest (-Regress), then -Count same-seed runs of the
# determinism_lesnar route with the setting on (fixed seed, per-frame hash as run_det.ps1, distance probe and the
# skeleton probe without raw dump), each run exclusive. -Setting off runs the same list without the setting.
# Compare with: py tools\determinism\fp_compare.py A B --pair 0-1 ; py tools\determinism\det_compare.py A B --slack 1
#               py tools\determinism\skel_compare.py A B --self A8F78BE0,A90BC480
# Usage: pwsh -NoProfile -File tools\determinism\run_inline.ps1 -Build s8_t2det_3 -Regress -Count 2
#        -RegressOnly 'selftest,lesnar' picks the regress routes (default selftest; lesnar also runs a match, so it
#        exercises the flags-off early-outs of the skeleton and decompression hooks).
param([string]$Build = '', [switch]$Regress, [string]$RegressOnly = 'selftest', [int]$Count = 2, [long]$Seed = 1234,
      [int]$RawEvery = 30, [string]$Spec = 'default,objects', [string]$Setting = 'on', [string]$MoreArgs = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = '..\..\..\tools\input_scripts\determinism_lesnar.txt'
. "$R\tools\runlock.ps1"
$lock = Enter-RunLock -Name "run_inline build=$Build regress=$Regress x$Count"
try {
    if ($Build) {
        & pwsh -NoProfile -File "$R\tools\build.ps1" -Run $Build
        if ($LASTEXITCODE -ne 0) { Write-Output "build failed ($LASTEXITCODE)"; exit 1 }
    }
    if ($Regress) {
        & py "$R\tools\regress\regress.py" --only $RegressOnly
        Write-Output "regress exit $LASTEXITCODE"
        if ($LASTEXITCODE -ne 0) { Write-Output 'regress failed: no determinism runs'; exit 1 }
    }
    for ($i = 0; $i -lt $Count; $i++) {
        $extra = "--wwe2k14_fixed_seed=$Seed --wwe2k14_det_hash=$Spec --wwe2k14_det_hash_raw_every=$RawEvery"
        $extra = "$extra --wwe2k14_fp_probe=true --wwe2k14_skel_probe=true"
        if ($Setting -eq 'on') { $extra = "$extra --wwe2k14_det_inline_motion=true" }
        if ($MoreArgs) { $extra = "$extra $MoreArgs" }
        Write-Output "=== inline motion $Setting, run $($i + 1) of $Count : $extra"
        & "$R\tools\bench\run_list.ps1" -Scripts @($route) -Extra $extra -Exclusive
    }
} finally { Exit-RunLock $lock }
