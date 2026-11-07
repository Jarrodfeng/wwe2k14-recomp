# Session 19 item 1: CPU time of EVERY thread of the running game, once per second (perf_sample.ps1 keeps only the
# top 8). Long CSV: time (HH:mm:ss.fff), t_s (seconds since the first sample), tid, name (Windows thread
# description, '-' if none), cpu_ms (CPU time in the interval), interval_ms. Exits when the game exits.
#   pwsh -NoProfile -File tools\perf\thread_busy.ps1 [-Out <csv>] [-WaitForStart 60] [-IntervalMs 1000]
# Default output: logs\threads_NNN.csv next to the newest run log (start it right after the game starts).
# Spinning threads (the game's main thread, GPU Commands) read ~100% here by design: use the timeline for them.
# Session 95: -ExePath samples the game at that exact path; -ExeLike TEXT the game whose path holds TEXT (other games may be running; the default is the first).
param([string]$Out = '', [int]$WaitForStart = 60, [int]$IntervalMs = 1000, [string]$ExePath = '', [string]$ExeLike = '')
$ErrorActionPreference = 'SilentlyContinue'
if (-not ('PerfThreadNames2' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PerfThreadNames2 {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr description);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    [DllImport("kernel32.dll")] static extern bool QueryThreadCycleTime(IntPtr h, out ulong cycles);
    // Session 95: exact cycles the thread ran (TSC rate), not the 15.6 ms ticks behind TotalProcessorTime.
    public static ulong Cycles(uint id) {
        IntPtr h = OpenThread(0x0800, false, id);
        if (h == IntPtr.Zero) return 0;
        try { ulong c; return QueryThreadCycleTime(h, out c) ? c : 0; } finally { CloseHandle(h); }
    }
    public static string Get(uint id) {
        IntPtr h = OpenThread(0x0800, false, id);
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
$deadline = (Get-Date).AddSeconds($WaitForStart)
function Find-Game { @(Get-Process wwe2k14 | Where-Object { (-not $ExePath -or $_.Path -ieq $ExePath) -and (-not $ExeLike -or $_.Path -like "*$ExeLike*") })[0] }
while (-not (Find-Game) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
$proc = Find-Game
if (-not $proc) { "no wwe2k14 process"; exit 1 }
if (-not $Out) {
    Start-Sleep -Seconds 2  # the run log appears shortly after the process
    $logs = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs'
    $newest = Get-ChildItem "$logs\wwe2k14_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $num = if ($newest) { $newest.BaseName -replace '^wwe2k14_', '' } else { 'x' }
    $Out = Join-Path $logs "threads_$num.csv"
}
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine('time,t_s,tid,name,cpu_ms,interval_ms,mcycles')
$names = @{}; $prev = @{}; $prevCycles = @{}; $t0 = Get-Date; $prevTime = $t0
while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds $IntervalMs
    $proc.Refresh()
    if ($proc.HasExited) { break }
    $now = Get-Date; $iv = ($now - $prevTime).TotalMilliseconds; $prevTime = $now
    $ts = $now.ToString('HH:mm:ss.fff'); $tsec = [math]::Round(($now - $t0).TotalSeconds, 3)
    foreach ($t in $proc.Threads) {
        $ms = $t.TotalProcessorTime.TotalMilliseconds
        $cy = [PerfThreadNames2]::Cycles([uint32]$t.Id)
        if ($prev.ContainsKey($t.Id)) {
            $d = $ms - $prev[$t.Id]
            $n = $names[$t.Id]
            if (-not $n) {
                $n = try { [PerfThreadNames2]::Get([uint32]$t.Id) } catch { '' }
                $n = if ($n) { ($n -replace '[:;,"\r\n]', '_').Trim() } else { '' }
                if ($n) { $names[$t.Id] = $n }
            }
            if (-not $n) { $n = '-' }
            [void]$sb.AppendLine(('{0},{1},{2},{3},{4:F1},{5:F1},{6:F2}' -f $ts, $tsec, $t.Id, $n, $d, $iv, (($cy - $prevCycles[$t.Id]) / 1e6)))
        }
        $prev[$t.Id] = $ms
        $prevCycles[$t.Id] = $cy
    }
    if ($sb.Length -gt 200000) { $sb.ToString() | Add-Content $Out -NoNewline; [void]$sb.Clear() }
}
$sb.ToString() | Add-Content $Out -NoNewline
"done: $Out"
