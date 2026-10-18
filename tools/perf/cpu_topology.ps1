# Session 19 item 2: prints this PC's L3 cache groups (one per CCD on Ryzen) and core groups from Windows
# (GetLogicalProcessorInformationEx), instead of assuming logical processors 0-15 = CCD0.
#   pwsh -NoProfile -File tools\perf\cpu_topology.ps1
if (-not ('CpuTopo' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class CpuTopo {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetLogicalProcessorInformationEx(int rel, IntPtr buf, ref uint len);
    // Returns lines "L3 <size KB> group <g> mask 0x<mask>" and "CORE group <g> mask 0x<mask> eff <class>".
    public static List<string> Query() {
        var outp = new List<string>();
        uint len = 0;
        GetLogicalProcessorInformationEx(0xFFFF, IntPtr.Zero, ref len);
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
            if (!GetLogicalProcessorInformationEx(0xFFFF, buf, ref len)) { outp.Add("error " + Marshal.GetLastWin32Error()); return outp; }
            int off = 0;
            while (off < len) {
                IntPtr p = IntPtr.Add(buf, off);
                int rel = Marshal.ReadInt32(p, 0);
                int size = Marshal.ReadInt32(p, 4);
                if (rel == 2) {  // RelationCache: Level(1) Assoc(1) LineSize(2) CacheSize(4) Type(4) Reserved(18) GroupCount(2) GroupMask...
                    byte level = Marshal.ReadByte(p, 8);
                    int cacheSize = Marshal.ReadInt32(p, 12);
                    int type = Marshal.ReadInt32(p, 16);
                    // GROUP_AFFINITY at offset 8 + 32: Mask (8), Group (2)
                    long mask = Marshal.ReadInt64(p, 40);
                    short grp = Marshal.ReadInt16(p, 48);
                    if (level == 3) outp.Add(String.Format("L3 {0} KB type {1} group {2} mask 0x{3:X}", cacheSize / 1024, type, grp, mask));
                } else if (rel == 0) {  // RelationProcessorCore: Flags(1) EfficiencyClass(1) Reserved(20) GroupCount(2) GroupMask...
                    byte eff = Marshal.ReadByte(p, 9);
                    long mask = Marshal.ReadInt64(p, 32);
                    short grp = Marshal.ReadInt16(p, 40);
                    outp.Add(String.Format("CORE group {0} mask 0x{1:X} eff {2}", grp, mask, eff));
                }
                off += size;
            }
        } finally { Marshal.FreeHGlobal(buf); }
        return outp;
    }
}
'@
}
$lines = [CpuTopo]::Query()
$lines | Where-Object { $_ -like 'L3*' }
$cores = @($lines | Where-Object { $_ -like 'CORE*' })
"cores: $($cores.Count)"
$cores | ForEach-Object { $_ }
