# S105 SRJ: offline test of bench.ps1 Only=route (no game, no Surface): a fake "exe" (a .cmd) writes a log, a flight file and a screenshot; the job must run it
# Repeats times, pass the job's GameFlags, kill a run that hangs (TimeoutSeconds), and zip logs + flight + captures. Prints SELFTEST PASS / FAIL, exit 0 / 1.
#   pwsh -NoProfile -File tools\surface\route_job_selftest.ps1
$ErrorActionPreference = 'Stop'
$bench = Join-Path $PSScriptRoot 'bench.ps1'
$t = Join-Path ([IO.Path]::GetTempPath()) ("route_job_selftest_" + $PID)
New-Item -ItemType Directory -Force "$t\build\logs", "$t\build\captures", "$t\routes", "$t\userdata\cache" | Out-Null
Set-Content "$t\routes\r.txt" 'quit'; Set-Content "$t\userdata\save.dat" 'x'
# the fake game: argument log; mode "hang" if a --hang switch is given
@'
@echo off
setlocal
set N=%TIME::=%_%RANDOM%
set N=%N: =0%
set N=%N:,=.%
echo %* > "%~dp0logs\wwe2k14_%N%.log"
echo flight > "%~dp0logs\flight_%N%.txt"
echo png > "%~dp0captures\001_x.png"
echo %* | findstr /c:"--hang" >nul && ping -n 60 127.0.0.1 >nul
exit /b 0
'@ | Set-Content "$t\build\wwe2k14.cmd" -Encoding ascii
$fail = @()
function Run([string]$out, [string[]]$extra, [int]$to, [int]$rep) {
  & pwsh -NoProfile -File $bench -Exe "$t\build\wwe2k14.cmd" -GameData "$t\gd" -UserData "$t\userdata" -Routes "$t\routes" -Replays "$t\rp" -Out $out `
    -Only route -Route r.txt -Repeats $rep -NoPower -TimeoutSeconds $to -GameFlags ($extra -join ",") 2>&1 | Out-String
}
$o1 = Run "$t\o1" @('wwe2k14_a=1', 'audio_mute=true') 30 3
foreach ($d in 'route', 'route_r2', 'route_r3') {
  if (-not (Get-ChildItem "$t\o1\$d" -Filter 'wwe2k14_*.log' -ErrorAction SilentlyContinue)) { $fail += "$d : no log" }
  if (-not (Get-ChildItem "$t\o1\$d" -Filter 'flight_*.txt' -ErrorAction SilentlyContinue)) { $fail += "$d : no flight" }
  if (-not (Test-Path "$t\o1\$d\captures\001_x.png")) { $fail += "$d : no screenshot" }
}
if ((Get-Content (Get-ChildItem "$t\o1\route" -Filter 'wwe2k14_*.log')[0].FullName -Raw) -notmatch '--wwe2k14_a=1 --audio_mute=true|--audio_mute=true') { $fail += 'GameFlags did not reach the game' }
if (-not (Get-ChildItem "$t\o1" -Filter 'surface_bench_*.zip')) { $fail += 'no zip' }
# planted fault, the unsafe direction: a hang must be killed at the timeout, not pass silently
$sw = [Diagnostics.Stopwatch]::StartNew()
$o2 = Run "$t\o2" @('hang') 5 1
if ($o2 -notmatch 'timed out, killed') { $fail += 'hang: not reported as killed' }
if ($sw.Elapsed.TotalSeconds -gt 50) { $fail += 'hang: took too long' }
# a missing route must throw
$o3 = & pwsh -NoProfile -File $bench -Exe "$t\build\wwe2k14.cmd" -GameData x -UserData "$t\userdata" -Routes "$t\routes" -Replays x -Out "$t\o3" -Only route -Route nope.txt -NoPower 2>&1 | Out-String
if ($o3 -notmatch 'not in') { $fail += 'missing route did not throw' }
Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue
if ($fail) { "SELFTEST FAIL: $($fail -join '; ')"; exit 1 } else { 'SELFTEST PASS'; exit 0 }
