# CPU / memory or GPU sampler next to game runs (Session 8, task L3: timed runs in shared lanes). Reads Windows
# performance counters, the process table or nvidia-smi only; launches no game, kills nothing.
#   pwsh -NoProfile -File tools\bench\lanes_monitor.ps1 -Out FILE.csv [-Gpu] [-StopFile FILE] [-ParentPid P]
# Stops when -StopFile exists or -ParentPid has exited (so it never outlives the batch launcher).
# CPU mode (default), one row about once a second:
#   time (local, ms), cpu_total % (all logical processors), ccd0 % (logical processors 0-15), ccd1 % (16-31),
#   mem_avail_mb, games (wwe2k14 processes), game_cpu (sum over games, % of ONE logical processor),
#   per_game ("pid:cpu%:affinity_hex;..."; cpu % of one logical processor, from TotalProcessorTime deltas),
#   fg_pid (process owning the foreground window).
#   The CCD split assumes the 5950X numbering (logical processors 0-15 = CCD0, 16-31 = CCD1).
# GPU mode (-Gpu), one row every -GpuMs (500) ms: time (local, ms), util % (utilization.gpu), power W (power.draw),
#   clock MHz (clocks.gr), mem MiB (memory.used), pstate. One nvidia-smi query per row (~50 ms each): nvidia-smi's
#   own -lms loop buffers its file output and loses the tail when stopped, so it is not used.
param([Parameter(Mandatory)][string]$Out, [switch]$Gpu, [string]$StopFile = '', [int]$ParentPid = 0, [int]$GpuMs = 500)
$ErrorActionPreference = 'Continue'
function Test-Stop {
    if ($StopFile -and (Test-Path -LiteralPath $StopFile)) { return $true }
    if ($ParentPid -and -not (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue)) { return $true }
    return $false
}
function Now { (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff') }

if ($Gpu) {
    $smi = (Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
    if (-not $smi) { "no nvidia-smi" | Set-Content -LiteralPath $Out; exit 1 }
    "time,util,power_w,clock_mhz,mem_mib,pstate" | Set-Content -LiteralPath $Out -Encoding utf8
    $sw = [Diagnostics.Stopwatch]::StartNew(); $next = 0
    while (-not (Test-Stop)) {
        $t = Now
        $v = & $smi --query-gpu=utilization.gpu,power.draw,clocks.gr,memory.used,pstate --format=csv,noheader,nounits 2>$null | Select-Object -First 1
        if ($v) { Add-Content -LiteralPath $Out -Value ("$t," + (($v -split ',\s*') -join ',')) -Encoding utf8 }
        $next += $GpuMs
        $wait = $next - $sw.ElapsedMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait } else { $next = $sw.ElapsedMilliseconds }
    }
    exit 0
}

$nLp = [Environment]::ProcessorCount
$half = [int]($nLp / 2)
# fg_pid: the process owning the foreground window (Windows boosts the foreground process; with several games at
# once this tells which one had focus).
try {
    Add-Type -Namespace LanesMon -Name Fg -MemberDefinition @'
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
'@ -ErrorAction Stop
} catch { }
function Get-FgPid { try { $fp = [uint32]0; [void][LanesMon.Fg]::GetWindowThreadProcessId([LanesMon.Fg]::GetForegroundWindow(), [ref]$fp); $fp } catch { '' } }
"time,cpu_total,ccd0,ccd1,mem_avail_mb,games,game_cpu,per_game,fg_pid" | Set-Content -LiteralPath $Out -Encoding utf8
$prev = @{}      # pid -> (TotalProcessorTime ms, stopwatch ms)
$sw = [Diagnostics.Stopwatch]::StartNew()
$counters = @('\Processor(*)\% Processor Time', '\Memory\Available MBytes')
while (-not (Test-Stop)) {
    $total = ''; $c0 = ''; $c1 = ''; $mem = ''
    try {
        # Rate counters need two readings: -MaxSamples 1 waits one SampleInterval (1 s) and returns the rate over it.
        $s = Get-Counter -Counter $counters -SampleInterval 1 -MaxSamples 1 -ErrorAction Stop
        $lp = @{}
        foreach ($cs in $s.CounterSamples) {
            if ($cs.Path -match '\\processor\((\d+|_total)\)\\') {
                if ($Matches[1] -eq '_total') { $total = [Math]::Round($cs.CookedValue, 1) } else { $lp[[int]$Matches[1]] = $cs.CookedValue }
            } elseif ($cs.Path -match 'available mbytes') { $mem = [int]$cs.CookedValue }
        }
        if ($lp.Count -ge $nLp) {
            $c0 = [Math]::Round((@(0..($half - 1) | ForEach-Object { $lp[$_] }) | Measure-Object -Average).Average, 1)
            $c1 = [Math]::Round((@($half..($nLp - 1) | ForEach-Object { $lp[$_] }) | Measure-Object -Average).Average, 1)
        }
    } catch { Start-Sleep -Milliseconds 1000 }
    $now = $sw.Elapsed.TotalMilliseconds
    $per = @(); $sum = 0.0
    $games = @(Get-Process -Name wwe2k14 -ErrorAction SilentlyContinue)
    foreach ($g in $games) {
        try {
            $cpuMs = $g.TotalProcessorTime.TotalMilliseconds
            $aff = '{0:X}' -f [int64]$g.ProcessorAffinity
            if ($prev.ContainsKey($g.Id)) {
                $p = $prev[$g.Id]
                $pct = 100.0 * ($cpuMs - $p[0]) / [Math]::Max(1.0, $now - $p[1])
                $sum += $pct
                $per += ('{0}:{1:F0}:{2}' -f $g.Id, $pct, $aff)
            } else { $per += ('{0}:new:{1}' -f $g.Id, $aff) }
            $prev[$g.Id] = @($cpuMs, $now)
        } catch { }
    }
    $line = '{0},{1},{2},{3},{4},{5},{6:F0},{7},{8}' -f (Now), $total, $c0, $c1, $mem, $games.Count, $sum, ($per -join ';'), (Get-FgPid)
    Add-Content -LiteralPath $Out -Value $line -Encoding utf8
}
