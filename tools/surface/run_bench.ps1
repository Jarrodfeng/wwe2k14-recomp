# Session 80 (HD 520 arc, item 3): started on the Surface by the scheduled task 'wwe_bench' (interactive logon, Jarrod's
# own desktop session - a game started straight from SSH runs in the hidden service session, where its frames are not
# shown or paced as for players; Jarrod's OK 2026-11-01, the task is deleted at the end). Runs bench.ps1 with the kit's
# paths plus the options in C:\wwe\bench_args.txt (lines NAME=VALUE, e.g. Only=passes,frames / Levers=a,b /
# ColdDriver=true); writes C:\wwe\bench_status.txt (running / done) and C:\wwe\bench_transcript.txt.
# On the PC: tools\surface\push_kit.ps1 copies it with the kit; tools\surface\remote.ps1 starts and watches it.
$ErrorActionPreference = 'Continue'
$root = 'C:\wwe'
$p = @{ Exe = "$root\build\wwe2k14.exe"; GameData = "$root\wwe2k14_test.iso"; UserData = "$root\userdata_frozen";
        Routes = "$root\routes"; Replays = "$root\replays"; Scenarios = "$root\scenarios"; Out = "$root\out" }
if (Test-Path "$root\bench_args.txt") {
  foreach ($line in Get-Content "$root\bench_args.txt") {
    if ($line -match '^\s*(\w+)\s*=\s*(.*?)\s*$') {
      $name, $v = $Matches[1], $Matches[2]
      $p[$name] = if ($v -eq 'true') { $true } elseif ($name -in 'Only', 'Levers') { @($v -split ',') } else { $v }
    }
  }
}
New-Item -ItemType Directory -Force $p.Out | Out-Null
Set-Content "$root\bench_status.txt" "running $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Start-Transcript -Path "$root\bench_transcript.txt" -Force | Out-Null
$ok = $true
try { & "$root\bench.ps1" @p } catch { $ok = $false; "bench.ps1 failed: $_" }
Stop-Transcript | Out-Null
Set-Content "$root\bench_status.txt" "done $(if ($ok) { 'ok' } else { 'failed' }) $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
