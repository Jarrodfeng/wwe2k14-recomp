# Session 37: every thread of the running game with its CPU time over a short window, its Windows thread name and its
# start address as module+offset (NtQueryInformationThread ThreadQuerySetWin32StartAddress), to identify unnamed busy
# threads (thread_busy.ps1 shows them as "tid N"). Map an offset in wwe2k14.exe with its .map file, in an SDK DLL
# with its PDB.
#   pwsh -NoProfile -File tools\perf\thread_starts.ps1 [-Seconds 3] [-Top 12]
param([int]$Seconds = 3, [int]$Top = 12)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ThreadStart37 {
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr d);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, IntPtr ret);
    public static IntPtr Start(uint id) {
        IntPtr h = OpenThread(0x0040, false, id);  // THREAD_QUERY_INFORMATION
        if (h == IntPtr.Zero) return IntPtr.Zero;
        IntPtr a; int s = NtQueryInformationThread(h, 9, out a, IntPtr.Size, IntPtr.Zero);
        CloseHandle(h);
        return s == 0 ? a : IntPtr.Zero;
    }
    public static string Name(uint id) {
        IntPtr h = OpenThread(0x0800, false, id);
        if (h == IntPtr.Zero) return "";
        IntPtr p; string n = "";
        if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { n = Marshal.PtrToStringUni(p); LocalFree(p); }
        CloseHandle(h);
        return n;
    }
}
'@
$p = Get-Process wwe2k14 -ErrorAction Stop | Select-Object -First 1
$mods = $p.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Base = [int64]$_.BaseAddress; End = [int64]$_.BaseAddress + $_.ModuleMemorySize } }
$t0 = @{}; foreach ($t in $p.Threads) { try { $t0[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Seconds $Seconds
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try { $ms = $t.TotalProcessorTime.TotalMilliseconds - $t0[$t.Id] } catch { continue }
    $a = [int64][ThreadStart37]::Start([uint32]$t.Id)
    $m = $mods | Where-Object { $a -ge $_.Base -and $a -lt $_.End } | Select-Object -First 1
    [pscustomobject]@{ tid = $t.Id; cpu_pct = [math]::Round(100 * $ms / ($Seconds * 1000), 1); name = [ThreadStart37]::Name([uint32]$t.Id)
                       start = if ($m) { '{0}+0x{1:X}' -f $m.Name, ($a - $m.Base) } else { '0x{0:X}' -f $a } }
}
$rows | Sort-Object cpu_pct -Descending | Select-Object -First $Top | Format-Table -AutoSize
