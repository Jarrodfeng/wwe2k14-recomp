# Lists the running game's threads with their Windows thread descriptions (names) and CPU time.
#   pwsh -File tools\list_thread_names.ps1
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ThreadNames {
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
$proc = Get-Process wwe2k14 -ErrorAction Stop | Select-Object -First 1
$proc.Threads | ForEach-Object {
    [pscustomobject]@{ Id = $_.Id; CpuSeconds = [math]::Round($_.TotalProcessorTime.TotalSeconds, 1); Name = [ThreadNames]::Get([uint32]$_.Id) }
} | Sort-Object CpuSeconds -Descending | Select-Object -First 25 | Format-Table -AutoSize | Out-String -Width 160
