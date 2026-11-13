# Pulls the phone's case zips (the report button's output) into out\cases, then runs case intake on each new one. The fallback when the phone has no
# WebDAV address typed in (Session 101, lane 5; docs\systems\case_zip.md). Over adb; nothing is deleted on the phone.
#   pwsh -NoProfile -File tools\android\pull_cases.ps1 [-NoIntake] [-Logs N] [-Board]
# Lane 240 (S101): the coverage board's PHONE column reads what this pulls. Case zips go to out\cases (the board takes the ones whose manifest says
# android); the newest -Logs (default 3, 0 = none) game logs (<files>\logs\wwe2k14_NNN.log) go to out\android\phone_logs\phone_NNN_<mtime>.log, where
# the board reads them as pseudo-zips (device phone, item from the scene lines, evidence = the census lines). A normal quit on the phone writes the
# end-of-run census block (lane 216: OnClosing runs RunQuitFlushes) but NO zip, so the log is the board's evidence for a plain session; quit the game
# before pulling (a log pulled mid-run lacks its final census). -Board then runs `py -I tools\board\coverage.py --write`.
param([switch]$NoIntake, [int]$Logs = 3, [switch]$Board)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$dest = Join-Path $ProjectRoot 'out\cases'
New-Item -ItemType Directory -Force $dest | Out-Null
$listing = & $Adb shell "ls $PhoneFiles/cases 2>/dev/null"
$zips = @($listing -split "`r?`n" | Where-Object { $_ -match '\.zip$' })
if (-not $zips.Count) { Write-Host 'no case zips on the phone' }
foreach ($z in $zips) {
  $local = Join-Path $dest $z
  if (Test-Path $local) { continue }
  & $Adb pull "$PhoneFiles/cases/$z" $local | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "adb pull of $z failed" }
  Write-Host "pulled $z"
  if (-not $NoIntake) { & py -I (Join-Path $ProjectRoot 'tools\case\intake.py') $local }
}
if ($Logs -gt 0) {
  $ldest = Join-Path $ProjectRoot 'out\android\phone_logs'
  New-Item -ItemType Directory -Force $ldest | Out-Null
  # "<epoch mtime> <path>" per log, newest first
  $rows = @((& $Adb shell "stat -c '%Y %n' $PhoneFiles/logs/wwe2k14_*.log 2>/dev/null") -split "`r?`n" | Where-Object { $_ -match '^\d+ .*wwe2k14_(\d+)\.log$' })
  $rows = $rows | Sort-Object { [int64](($_ -split ' ', 2)[0]) } -Descending | Select-Object -First $Logs
  foreach ($r in $rows) {
    $mt, $path = $r -split ' ', 2
    $num = [regex]::Match($path, 'wwe2k14_(\d+)\.log$').Groups[1].Value
    $local = Join-Path $ldest "phone_${num}_${mt}.log"
    if (Test-Path $local) { continue }
    & $Adb pull $path $local | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb pull of $path failed" }
    # a later copy of the same run (it grew since the last pull) replaces the earlier one
    Get-ChildItem $ldest -Filter "phone_${num}_*.log" | Where-Object { $_.Name -ne "phone_${num}_${mt}.log" } | Remove-Item -Force
    Write-Host "pulled log wwe2k14_$num.log -> $local"
  }
}
if ($Board) { & py -I (Join-Path $ProjectRoot 'tools\board\coverage.py') --write }
