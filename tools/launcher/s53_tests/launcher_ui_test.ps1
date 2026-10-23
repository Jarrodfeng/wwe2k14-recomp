# Session 53 item 2.4: the shortcut, the Settings page and a key cancelling QUICK START (test settings + local data).
param([string]$Launcher)
$R = 'C:\Users\Jarrod\wwe2k14-recomp\out\s53'
. "$PSScriptRoot\winvis.ps1"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PM { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t); }
"@
Remove-Item "$R\shortcut" -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$R\shortcut" | Out-Null
# 1. the shortcut + a shot of Settings -> GAME
$a = "--settings $R\set_data.json --local-data $R\localdata --log $R\launcher_g.log --no-wizard --shortcut-dir $R\shortcut --create-shortcut --page settings --settings-section 0 --wait 10 --shot $R\launcher_settings_game.png --exit"
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru; $null = $l.WaitForExit(30000)
$lnk = Get-ChildItem "$R\shortcut\*.lnk" | Select-Object -First 1
if ($lnk) { $s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk.FullName); "shortcut: $($lnk.Name) -> $($s.TargetPath) $($s.Arguments) (icon $($s.IconLocation))" } else { "no shortcut" }
Get-Content "$R\launcher_g.log" | Select-String -NotMatch '^step '
# 2. QUICK START cancelled by a key 1 s in
Remove-Item "$R\launcher_h.log" -ErrorAction SilentlyContinue
$a = "--settings $R\set_quick.json --local-data $R\localdata --log $R\launcher_h.log"
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru
for ($i = 0; $i -lt 200 -and -not ((Get-Content "$R\launcher_h.log" -ErrorAction SilentlyContinue) -match 'countdown shown'); $i++) { Start-Sleep -Milliseconds 25 }
"countdown shown $([int]((Get-Date) - $l.StartTime).TotalMilliseconds) ms after the start"
$l.Refresh(); $h = $l.MainWindowHandle; "window handle $h (FindWindow: $([PM]::FindWindow('wwe2k14_launcher', $null)))"
Start-Sleep -Milliseconds 1000
"posting the key $([int]((Get-Date) - $l.StartTime).TotalMilliseconds) ms after the start"
$null = [PM]::PostMessage($h, 0x0100, [IntPtr]0x20, [IntPtr]0x00390001)  # WM_KEYDOWN space
Start-Sleep -Milliseconds 80
$null = [PM]::PostMessage($h, 0x0101, [IntPtr]0x20, [IntPtr]0xC0390001)  # WM_KEYUP
Start-Sleep -Seconds 5
"game running after the key: $([bool](Get-Process wwe2k14 -ErrorAction SilentlyContinue)); launcher windows: $([WV]::List([uint32]$l.Id))"
Get-Content "$R\launcher_h.log"
Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force
