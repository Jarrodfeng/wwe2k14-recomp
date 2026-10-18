# Session 21 item 2: present spread at step rates, a setting off / on (short\present_match.txt: launch, skip
# entrances, 3,000 match frames; timed, exclusive, normal speed). Per run: py tools\perf\present_spread.py RUN --hz N.
#   & .\tools\perf\s21_present_batch.ps1 -Tag T [-Rates 165,120,60] [-Off ''] [-On '--present_guest_frames_once=true'] [-Common '']
# 60 = the game's own rate (no step flags). In-process only (pwsh -File mangles strings, Session 20).
param([Parameter(Mandatory)][string]$Tag, [int[]]$Rates = @(165, 120, 60), [string]$Off = '',
      [string]$On = '--present_guest_frames_once=true', [string]$Common = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\tools\input_scripts\short\present_match.txt"
$log = "$R\out\harness\step21\present_$Tag.txt"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
$sw = @{ Exclusive = $true; RealTime = $true }
foreach ($hz in $Rates) {
    $step = if ($hz -ne 60) { "--wwe2k14_step_hz=$hz --wwe2k14_step_vblank=true" } else { '' }
    foreach ($arm in 'off', 'on') {
        $flags = if ($arm -eq 'off') { $Off } else { $On }
        $extra = "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_skip_entrances=true $step $Common $flags" -replace '\s+', ' '
        $out = & "$R\tools\bench\run_list.ps1" -Scripts @($route) @sw -Extra $extra.Trim() -WaitSeconds 900 2>&1
        $line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
        "$(Get-Date -Format 'HH:mm:ss') $Tag $hz $arm -> $line" | Tee-Object -FilePath $log -Append
        if ("$line" -match 'wwe2k14_(\d+)') {
            py "$R\tools\perf\present_spread.py" $Matches[1] --hz $hz 2>&1 | Tee-Object -FilePath $log -Append
        }
    }
}
