# Session 20 item 6: is the card running flat out? Timed, exclusive. Runs short\profile_lesnar_bryan.txt with
# nvidia-smi logging every 100 ms (clocks, P-state, power vs limit, utilisation, temperature, throttle reasons):
#   ff = fast-forwarded (vsync off), rt = real time. Both with GPU timestamps (same as Session 19 runs 1435 / 1436).
# Background load is sampled before each run into out\harness\step20\background.txt.
# Report: py tools\perf\gpu_clocks.py RUN SMI_CSV
#   pwsh -NoProfile -File tools\perf\s20_item6_batch.ps1 [-Only ff,rt] [-Tag item6] [-Extra '...']
param([string[]]$Only = @('ff', 'rt'), [string]$Tag = 'item6', [string]$Extra = '')
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$repo\tools\input_scripts\short\profile_lesnar_bryan.txt"
$dir = "$repo\out\harness\step20"
$bg = "$dir\background.txt"
New-Item -ItemType Directory -Force $dir | Out-Null
function Background([string]$tag) {
    $g = (1..4 | ForEach-Object { (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits).Trim(); Start-Sleep -Milliseconds 400 }) -join ' '
    $c = ((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples |
          ForEach-Object { [math]::Round($_.CookedValue, 1) }) -join ' '
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $tag background: GPU % [$g], CPU % [$c]" | Tee-Object -FilePath $bg -Append
}
$q = 'timestamp,pstate,clocks.gr,clocks.sm,clocks.mem,power.draw,enforced.power.limit,utilization.gpu,utilization.memory,temperature.gpu,clocks_throttle_reasons.active'
$common = "--wwe2k14_launch=lesnar_bryan --vulkan_gpu_timestamps=true $Extra".Trim()
foreach ($arm in $Only) {
    $runExtra = if ($arm -eq 'ff') { "$common --vsync=false" } else { $common }
    # Switches go in a hashtable splat: an empty array from an if-block becomes $null, and splatting $null passes a
    # positional $null (it landed in run_list's -Data and mirrored all of out\bench as user data; run 1463).
    $runFlag = @{ RealTime = ($arm -ne 'ff') }
    Background "$Tag $arm"
    $smiCsv = "$dir\smi_${Tag}_${arm}_$(Get-Date -Format 'HHmmss').csv"
    $smi = Start-Process nvidia-smi -ArgumentList "--query-gpu=$q", '--format=csv', '-lms', '100' -RedirectStandardOutput $smiCsv -PassThru -WindowStyle Hidden
    & "$repo\tools\bench\run_list.ps1" -Scripts @($route) -Exclusive @runFlag -Extra $runExtra -WaitSeconds 900 2>&1 | Select-String ' -> '
    Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
    "$Tag $arm smi log: $smiCsv" | Tee-Object -FilePath $bg -Append
}
Background "$Tag end"
