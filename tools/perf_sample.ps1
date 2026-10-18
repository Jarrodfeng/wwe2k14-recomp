# Samples GPU and game-process CPU load once per second while wwe2k14 runs, to tell GPU-bound from CPU-bound
# slowdowns (pair with the guest frame log, logs\frames_NNN.csv). Writes a CSV; exits when the game exits.
#   pwsh -File tools\perf_sample.ps1 [-Out <csv>] [-WaitForStart 60]
# Columns: time, gpu_util %, gpu_clock MHz, gpu_mem_used MB, process_cpu % (of one core), threads,
#          top_threads: top 3 threads by CPU in the last second as tid:% (100 = one full core), space-separated
#          (unchanged, so older perf CSVs compare directly),
#          top_threads_named: top 8 threads as tid:name:%, ';'-separated. name = the Windows thread description
#          (what tools\list_thread_names.ps1 shows: GPU Commands, GPU Submit, GPU VSync, Main XThread,
#          XThreadNNNN...), '-' if the thread has none; ':' ';' ',' '"' in names become '_'.
param([string]$Out = '', [int]$WaitForStart = 60)
$ErrorActionPreference = 'SilentlyContinue'
# Thread descriptions (same P/Invoke as tools\list_thread_names.ps1).
if (-not ('PerfThreadNames' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PerfThreadNames {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr description);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    public static string Get(uint id) {
        IntPtr h = OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, false, id);
        if (h == IntPtr.Zero) return "";
        try {
            IntPtr p;
            if (GetThreadDescription(h, out p) < 0 || p == IntPtr.Zero) return "";
            string s = Marshal.PtrToStringUni(p);
            LocalFree(p);
            return s;
        } finally { CloseHandle(h); }
    }
}
'@
}
$names = @{}  # tid -> sanitized name; empty names are looked up again (threads can be named after they start)
function Get-ThreadName([int]$id) {
    if ($names.ContainsKey($id)) { return $names[$id] }
    $n = try { [PerfThreadNames]::Get([uint32]$id) } catch { '' }
    if (-not $n) { return '-' }
    $n = ($n -replace '[:;,"\r\n]', '_').Trim()
    $names[$id] = $n
    return $n
}
if (-not $Out) {
    $logs = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs'
    $newest = Get-ChildItem "$logs\wwe2k14_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $num = if ($newest) { $newest.BaseName -replace '^wwe2k14_', '' } else { 'x' }
    $Out = Join-Path $logs "perf_$num.csv"
}
$deadline = (Get-Date).AddSeconds($WaitForStart)
while (-not (Get-Process wwe2k14) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
$proc = Get-Process wwe2k14 | Select-Object -First 1
if (-not $proc) { "no wwe2k14 process"; exit 1 }
'time,gpu_util,gpu_clock_mhz,gpu_mem_mb,process_cpu,threads,top_threads,top_threads_named' | Set-Content $Out
$prevThreads = @{}; $prevTotal = $proc.TotalProcessorTime.TotalMilliseconds; $prevTime = Get-Date
while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds 1000
    $proc.Refresh()
    if ($proc.HasExited) { break }
    $now = Get-Date; $elapsed = ($now - $prevTime).TotalMilliseconds; $prevTime = $now
    $total = $proc.TotalProcessorTime.TotalMilliseconds
    $cpu = [math]::Round(100 * ($total - $prevTotal) / $elapsed); $prevTotal = $total
    $deltas = foreach ($t in $proc.Threads) {
        $ms = $t.TotalProcessorTime.TotalMilliseconds
        $d = if ($prevThreads.ContainsKey($t.Id)) { $ms - $prevThreads[$t.Id] } else { 0 }
        $prevThreads[$t.Id] = $ms
        [pscustomobject]@{ Id = $t.Id; Pct = [math]::Round(100 * $d / $elapsed) }
    }
    $alive = @{}; foreach ($d in $deltas) { $alive[$d.Id] = $true }
    foreach ($id in @($names.Keys)) { if (-not $alive.ContainsKey($id)) { $names.Remove($id) } }  # Windows reuses tids
    $sorted = @($deltas | Sort-Object Pct -Descending | Select-Object -First 8)
    $top = ($sorted | Select-Object -First 3 | ForEach-Object { "$($_.Id):$($_.Pct)" }) -join ' '
    $topNamed = ($sorted | ForEach-Object { "$($_.Id):$(Get-ThreadName $_.Id):$($_.Pct)" }) -join ';'
    $gpu = (nvidia-smi --query-gpu=utilization.gpu,clocks.gr,memory.used --format=csv,noheader,nounits) -replace ' ', ''
    "{0},{1},{2},{3},{4},{5}" -f $now.ToString('HH:mm:ss'), $gpu, $cpu, $proc.Threads.Count, $top, $topNamed | Add-Content $Out
}
"done: $Out"
