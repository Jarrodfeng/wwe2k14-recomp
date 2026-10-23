# Session 53: a hidden launcher (QUICK START) closes with the game, or shows itself after a crash.
param([string]$Launcher, [string]$ArgsFile, [ValidateSet('close', 'crash')] [string]$How)
. "$PSScriptRoot\winvis.ps1"
$a = (Get-Content $ArgsFile -Raw).Trim()
$l = Start-Process -FilePath $Launcher -ArgumentList $a -PassThru -WorkingDirectory (Split-Path $Launcher)
$g = $null
for ($i = 0; $i -lt 40 -and -not $g; $i++) { Start-Sleep -Milliseconds 500; $g = Get-Process wwe2k14 -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $g) { "no game started"; Stop-Process -Id $l.Id -Force; exit 1 }
Start-Sleep -Seconds 12
"game pid $($g.Id); launcher windows: $([WV]::List([uint32]$l.Id))"
if ($How -eq 'close') { $null = $g.CloseMainWindow(); "sent WM_CLOSE to the game" } else { Stop-Process -Id $g.Id -Force; "killed the game (crash-like exit)" }
for ($i = 0; $i -lt 30 -and -not $g.HasExited; $i++) { Start-Sleep -Milliseconds 500 }
"game exited: $($g.HasExited) code $(if ($g.HasExited) { $g.ExitCode })"
Start-Sleep -Seconds 3
if ($l.HasExited) { "launcher exited: code $($l.ExitCode)" } else { "launcher running; windows: $([WV]::List([uint32]$l.Id))" }
Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force
