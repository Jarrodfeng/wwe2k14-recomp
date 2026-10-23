Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class WV {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static string List(uint want) {
    var sb = new StringBuilder();
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); if (pid == want) { var t = new StringBuilder(256); GetWindowText(h, t, 256); if (t.Length > 0) sb.Append("'" + t + "' visible=" + IsWindowVisible(h) + "; "); } return true; }, IntPtr.Zero);
    return sb.ToString();
  }
}
"@
