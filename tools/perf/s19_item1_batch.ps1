# Session 19 item 1: the profile batch (timed, exclusive; PC idle). Runs short\profile_lesnar_bryan.txt three times:
#   1. fast-forwarded (vsync off) + GPU timestamps, with tools\perf\thread_busy.ps1 alongside (game fps, thread CPU)
#   2. real time + GPU timestamps, with thread_busy.ps1 alongside (frame times)
#   3. fast-forwarded with the sampling profiler on the main thread and every thread named GPU* (stacks file)
# Background load (GPU utilisation, total CPU) is sampled before each run into out\harness\step19\item1_background.txt.
#   pwsh -NoProfile -File tools\perf\s19_item1_batch.ps1 [-Only 1,2,3]
param([int[]]$Only = @(1, 2, 3))
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$repo\tools\input_scripts\short\profile_lesnar_bryan.txt"
$bg = "$repo\out\harness\step19\item1_background.txt"
New-Item -ItemType Directory -Force (Split-Path $bg) | Out-Null
function Background([string]$tag) {
    $g = (1..4 | ForEach-Object { (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits).Trim(); Start-Sleep -Milliseconds 400 }) -join ' '
    $c = ((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples |
          ForEach-Object { [math]::Round($_.CookedValue, 1) }) -join ' '
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $tag background: GPU % [$g], CPU % [$c]" | Tee-Object -FilePath $bg -Append
}
$common = '--wwe2k14_launch=lesnar_bryan --vulkan_gpu_timestamps=true'
$runs = @{
    1 = @{ Extra = "$common --vsync=false"; Sampler = $true; Tag = 'item1 fast-forward' }
    2 = @{ Extra = $common; Sampler = $true; Tag = 'item1 real time' }
    3 = @{ Extra = '--wwe2k14_launch=lesnar_bryan --vsync=false --wwe2k14_profile_main=true --wwe2k14_profile_gpu_commands=true --wwe2k14_profile_threads=GPU --wwe2k14_profile_stacks=true'; Sampler = $false; Tag = 'item1 stacks' }
}
foreach ($k in $Only) {
    $r = $runs[$k]
    Background $r.Tag
    $sampler = $null
    if ($r.Sampler) {
        $sampler = Start-Process pwsh -ArgumentList '-NoProfile', '-File', "$repo\tools\perf\thread_busy.ps1", '-WaitForStart', '600' -PassThru -WindowStyle Hidden
    }
    & "$repo\tools\bench\run_list.ps1" -Scripts @($route) -Exclusive -Extra $r.Extra -WaitSeconds 900 2>&1 | Select-String ' -> '
    if ($sampler) { $sampler | Wait-Process -Timeout 60 -ErrorAction SilentlyContinue }
}
Background 'item1 end'
