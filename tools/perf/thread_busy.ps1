# Session 19 item 1: CPU time of EVERY thread of the running game, once per second (perf_sample.ps1 keeps only the
# top 8). Long CSV: time (HH:mm:ss.fff), t_s (seconds since the first sample), tid, name (Windows thread
# description, '-' if none), cpu_ms (CPU time in the interval), interval_ms. Exits when the game exits.
#   pwsh -NoProfile -File tools\perf\thread_busy.ps1 [-Out <csv>] [-WaitForStart 60] [-IntervalMs 1000]
# Default output: logs\threads_NNN.csv next to the newest run log (start it right after the game starts).
# Spinning threads (the game's main thread, GPU Commands) read ~100% here by design: use the timeline for them.
# Session 102 lane S0-2: the CSV gains a `start` column (the module + offset of the thread's Win32 start address, NtQueryInformationThread class 9:
# it names the unnamed steady threads: ours, a driver's, the loader's) and the cycles column is renamed `mcycles_fixedrate`: QueryThreadCycleTime
# counts at a FIXED rate on the Surface (R5 section 4.1: 1.77-1.83 "GHz" whether 88 % or 545 % of a core was busy), so it measures work, not a clock.
# Never divide it by time and call the result GHz; the clock-like numbers are `perf_pct` in the [perf][sys] line and `cpu_perf` in power.csv.
# -ProcessName samples another process name than wwe2k14 (the S0-2 test samples perf_sys_test.exe with it).
# Session 95: -ExePath samples the game at that exact path; -ExeLike TEXT the game whose path holds TEXT (other games may be running; the default is the first).
param([string]$Out = '', [int]$WaitForStart = 60, [int]$IntervalMs = 1000, [string]$ExePath = '', [string]$ExeLike = '', [string]$ProcessName = 'wwe2k14')
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
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, IntPtr ret);
    // Session 102 lane S0-2: the thread's Win32 start address (class 9, ThreadQuerySetWin32StartAddress); 0 when it cannot be read.
    public static ulong StartAddress(uint id) {
        IntPtr h = OpenThread(0x0040, false, id);  // THREAD_QUERY_INFORMATION
        if (h == IntPtr.Zero) return 0;
        try { IntPtr a; return NtQueryInformationThread(h, 9, out a, IntPtr.Size, IntPtr.Zero) >= 0 ? (ulong)a.ToInt64() : 0; } finally { CloseHandle(h); }
    }
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
function Find-Game { @(Get-Process $ProcessName | Where-Object { (-not $ExePath -or $_.Path -ieq $ExePath) -and (-not $ExeLike -or $_.Path -like "*$ExeLike*") })[0] }
while (-not (Find-Game) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
$proc = Find-Game
if (-not $proc) { "no $ProcessName process"; exit 1 }
if (-not $Out) {
    Start-Sleep -Seconds 2  # the run log appears shortly after the process
    $logs = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs'
    $newest = Get-ChildItem "$logs\wwe2k14_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $num = if ($newest) { $newest.BaseName -replace '^wwe2k14_', '' } else { 'x' }
    $Out = Join-Path $logs "threads_$num.csv"
}
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine('time,t_s,tid,name,cpu_ms,interval_ms,mcycles_fixedrate,start')
# Session 102 lane S0-2: start address -> "module+0xOFFSET" ("-" when unreadable), once per new tid; the module list is re-read when an address is not in it.
$starts = @{}; $mods = @()
function Update-Modules { $script:mods = @(try { $proc.Modules | ForEach-Object { [pscustomobject]@{ Base = [uint64]$_.BaseAddress.ToInt64(); End = [uint64]$_.BaseAddress.ToInt64() + [uint64]$_.ModuleMemorySize; Name = $_.ModuleName } } } catch { @() }) }
function Resolve-Start([uint32]$id) {
    $a = [PerfThreadNames2]::StartAddress($id)
    if ($a -eq 0) { return '-' }
    for ($pass = 0; $pass -lt 2; $pass++) {
        $m = $script:mods | Where-Object { $a -ge $_.Base -and $a -lt $_.End } | Select-Object -First 1
        if ($m) { return ('{0}+0x{1:X}' -f $m.Name, ($a - $m.Base)) }
        if ($pass -eq 0) { Update-Modules }
    }
    return ('unknown+0x{0:X}' -f $a)
}
Update-Modules
$names = @{}; $prev = @{}; $prevCycles = @{}; $t0 = Get-Date; $prevTime = $t0
while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds $IntervalMs
    $proc.Refresh()
    if ($proc.HasExited) { break }
    $now = Get-Date; $iv = ($now - $prevTime).TotalMilliseconds; $prevTime = $now
    $ts = $now.ToString('HH:mm:ss.fff'); $tsec = [math]::Round(($now - $t0).TotalSeconds, 3)
    # Session 102 S0-2 review item 7: drop cached names / starts / counters of thread ids that are gone (Windows reuses ids).
    $live = @{}; foreach ($t in $proc.Threads) { $live[$t.Id] = $true }
    foreach ($k in @($starts.Keys)) { if (-not $live.ContainsKey($k)) { $starts.Remove($k) } }
    foreach ($k in @($names.Keys)) { if (-not $live.ContainsKey($k)) { $names.Remove($k) } }
    foreach ($k in @($prev.Keys)) { if (-not $live.ContainsKey($k)) { $prev.Remove($k); $prevCycles.Remove($k) } }
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
            if (-not $starts.ContainsKey($t.Id)) { $starts[$t.Id] = Resolve-Start ([uint32]$t.Id) }
            [void]$sb.AppendLine(('{0},{1},{2},{3},{4:F1},{5:F1},{6:F2},{7}' -f $ts, $tsec, $t.Id, $n, $d, $iv, (($cy - $prevCycles[$t.Id]) / 1e6), $starts[$t.Id]))
        }
        $prev[$t.Id] = $ms
        $prevCycles[$t.Id] = $cy
    }
    if ($sb.Length -gt 200000) { $sb.ToString() | Add-Content $Out -NoNewline; [void]$sb.Clear() }
}
$sb.ToString() | Add-Content $Out -NoNewline
"done: $Out"
