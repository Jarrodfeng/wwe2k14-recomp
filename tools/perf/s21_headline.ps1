# Session 21 last step: the headline check. The profile route (launch lesnar_bryan, both entrances, 2,000 match
# frames; seed 1 = the same camera variant) at normal speed with --wwe2k14_step_hz=N --wwe2k14_step_vblank=true (as
# Session 19's runs 1427 / 1431 / 1432), the winners off and on, then py tools\bench\rate_check.py RUN --hz N.
#   & .\tools\perf\s21_headline.ps1 -On '--gpu_skip_noop_draws=true --vulkan_upload_hoist=2' [-Rates 120,144,165] [-Tag head]
# In-process only (pwsh -File mangles strings, Session 20).
param([Parameter(Mandatory)][string]$On, [int[]]$Rates = @(120, 144, 165), [string]$Tag = 'head')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$log = "$R\out\harness\step21\headline_$Tag.txt"
foreach ($hz in $Rates) {
    $step = "--wwe2k14_fixed_seed=1 --wwe2k14_step_hz=$hz --wwe2k14_step_vblank=true"
    $arms = @("off$hz=$step", "on$hz=$step $On")
    $out = & "$R\tools\perf\s20_batch.ps1" -Tag "s21$Tag" -RealTime -Arms $arms 2>&1
    foreach ($line in @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' })) {
        [void]("$line" -match '^s21\S+ (\S+) -> .* -> wwe2k14_(\d+)')
        $arm = $Matches[1]; $run = $Matches[2]
        "$(Get-Date -Format 'HH:mm:ss') $arm run $run" | Tee-Object -FilePath $log -Append
        py "$R\tools\bench\rate_check.py" $run --hz $hz 2>&1 | Tee-Object -FilePath $log -Append
    }
}
