# Session 20: a batch of arms of a short route (default short\profile_lesnar_bryan.txt; timed, exclusive), each with nvidia-smi every 100 ms
# and the background load sampled before it (out\harness\step20\background.txt). Fast-forwarded with GPU timestamps
# unless an arm says -RealTime. Arms are NAME=FLAGS; FLAGS may name skip lists from tools\perf\s20_skip_lists.txt as
# skip:LIST[+LIST...] (-> --gpu_debug_skip_vertex_shader=hashes).
#   & .\tools\perf\s20_batch.ps1 -Tag item2 -Arms @('base=','crowd=skip:crowd') [-Common '...'] [-RealTime] [-Route X.txt]
# In-process only: pwsh -File turns the -Arms list into one quoted string (run 1466).
# Report per arm: py tools\bench\bench_report.py report RUN ; py tools\perf\gpu_clocks.py RUN SMI_CSV
param([Parameter(Mandatory)][string[]]$Arms, [string]$Tag = 'batch', [string]$Common = '', [switch]$RealTime,
      [string]$Route = 'profile_lesnar_bryan.txt')   # a route in tools\input_scripts\short
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$routePath = "$repo\tools\input_scripts\short\$Route"  # not $route: PowerShell names are case-insensitive
$dir = "$repo\out\harness\step20"
$bg = "$dir\background.txt"
New-Item -ItemType Directory -Force $dir | Out-Null
$lists = @{}
foreach ($l in Get-Content "$repo\tools\perf\s20_skip_lists.txt") { $k, $v = $l -split '=', 2; $lists[$k] = $v }
function Background([string]$tag) {
    $g = (1..4 | ForEach-Object { (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits).Trim(); Start-Sleep -Milliseconds 400 }) -join ' '
    $c = ((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples |
          ForEach-Object { [math]::Round($_.CookedValue, 1) }) -join ' '
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $tag background: GPU % [$g], CPU % [$c]" | Tee-Object -FilePath $bg -Append
}
$q = 'timestamp,pstate,clocks.gr,clocks.sm,clocks.mem,power.draw,enforced.power.limit,utilization.gpu,utilization.memory,temperature.gpu,clocks_throttle_reasons.active'
foreach ($arm in $Arms) {
    $name, $flags = $arm -split '=', 2
    $flags = [regex]::Replace("$flags", 'skip:(\S+)', { param($m) '--gpu_debug_skip_vertex_shader=' + (($m.Groups[1].Value -split '\+' | ForEach-Object { $lists[$_] }) -join ',') })
    $extra = "--wwe2k14_launch=lesnar_bryan --vulkan_gpu_timestamps=true $Common $flags".Trim()
    if (-not $RealTime) { $extra += ' --vsync=false' }
    Background "$Tag $name"
    $smiCsv = "$dir\smi_${Tag}_${name}_$(Get-Date -Format 'HHmmss').csv"
    $smi = Start-Process nvidia-smi -ArgumentList "--query-gpu=$q", '--format=csv', '-lms', '100' -RedirectStandardOutput $smiCsv -PassThru -WindowStyle Hidden
    # Switches as a hashtable splat (an empty array splat passes a positional $null: run 1463).
    $sw = @{ Exclusive = $true; RealTime = [bool]$RealTime }
    $out = & "$repo\tools\bench\run_list.ps1" -Scripts @($routePath) @sw -Extra $extra -WaitSeconds 900 2>&1
    Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
    $line = @($out | Where-Object { "$_" -match "$([regex]::Escape($Route)) -> " }) | Select-Object -Last 1
    "$Tag $name -> $("$line".Substring(0, [Math]::Min(160, "$line".Length))) | smi $smiCsv" | Tee-Object -FilePath $bg -Append
}
Background "$Tag end"
