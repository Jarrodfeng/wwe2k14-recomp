# Session 53 items 2.1 / 2.4: start a release exe (or the launcher) outside the repo and report what runs.
param([string]$Exe, [string]$ArgsFile = "", [int]$Seconds = 15, [switch]$KeepLauncherAway)
$dir = Split-Path $Exe
$ArgsLine = if ($ArgsFile) { (Get-Content $ArgsFile -Raw).Trim() } else { "" }
$L = Join-Path $dir 'wwe2k14_launcher.exe'
if ($KeepLauncherAway -and (Test-Path $L)) { Rename-Item $L "$L.away" }
try {
  Remove-Item "$dir\logs" -Recurse -Force -ErrorAction SilentlyContinue
  "start: $Exe $ArgsLine"
  $sp = @{ FilePath = $Exe; PassThru = $true; WorkingDirectory = $dir }
  if ($ArgsLine) { $sp.ArgumentList = $ArgsLine }
  $p = Start-Process @sp
  Start-Sleep -Seconds $Seconds
  if ($p.HasExited) { "first process exited: code $($p.ExitCode)" } else { "first process running" }
  . "$PSScriptRoot\winvis.ps1"
  foreach ($q in Get-CimInstance Win32_Process -Filter "Name='wwe2k14.exe' OR Name='wwe2k14_launcher.exe'") {
    $g = Get-Process -Id $q.ProcessId -ErrorAction SilentlyContinue
    "process: $($q.Name) pid $($q.ProcessId) windows: $([WV]::List([uint32]$q.ProcessId))"
    if ($q.CommandLine -match "wwe2k14_settings_file=(\S+)") { "  settings_file flag: $($Matches[1])" }
    "  cmd: $(if ($q.CommandLine.Length -gt 400) { $q.CommandLine.Substring(0, 400) + ' ...' } else { $q.CommandLine })"
  }
  $log = Get-ChildItem "$dir\logs\wwe2k14_*.log" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($log) { "log: $($log.Name)"; Select-String -Path $log.FullName -Pattern 'game_data_root|User data|error\]|settings file' | Select-Object -First 8 | ForEach-Object { "  " + $_.Line.Substring([Math]::Min(26, $_.Line.Length)) } }
} finally {
  Get-Process wwe2k14, wwe2k14_launcher -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 500
  if (Test-Path "$L.away") { Rename-Item "$L.away" $L }
}
