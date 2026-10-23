# Session 53 item 2.6: one dev-build run with a test save folder and an input script; prints its keybind log lines.
param([string]$Script, [string]$More = "")
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$exe = "$R\out\build\win-amd64-release\wwe2k14.exe"
$a = "--user_data_root=$R\out\s53\userdata_kb --wwe2k14_settings_file=$R\out\s53\set_data.json --mnk_mode=true --vsync=false --wwe2k14_input_script=$R\out\s53\$Script $More"
$before = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WorkingDirectory (Split-Path $exe)
if (-not $p.WaitForExit(180000)) { "timeout: killed"; Stop-Process -Id $p.Id -Force }
"exit code $($p.ExitCode)"
$log = Get-ChildItem "$R\out\build\win-amd64-release\logs\wwe2k14_*.log" | Where-Object LastWriteTime -gt $before | Sort-Object LastWriteTime | Select-Object -Last 1
"log $($log.Name)"
Select-String -Path $log.FullName -Pattern '\[keybinds\]|user_data_root:|Saved config' | ForEach-Object { "  " + $_.Line.Substring(26) }
$f = "$R\out\s53\userdata_kb\keybinds.toml"
if (Test-Path $f) { "keybinds.toml:"; Get-Content $f | ForEach-Object { "  $_" } } else { "no keybinds.toml" }
$t = "$R\out\build\win-amd64-release\wwe2k14.toml"
if (Test-Path $t) { "wwe2k14.toml next to the exe: $((Get-Item $t).LastWriteTime)" } else { "no wwe2k14.toml next to the exe" }
