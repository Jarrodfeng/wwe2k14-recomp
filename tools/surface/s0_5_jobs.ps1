# S102 S0-5: run a list of Surface bench jobs one after another (each = one remote.ps1 start, wait for done, fetch the zip).
#   pwsh -NoProfile -File tools\surface\s0_5_jobs.ps1 -Out out\s102\s0_5 -Jobs 'name|BenchArgs', 'name|BenchArgs' ...
# BenchArgs as remote.ps1 takes them ('Only=frames_bryan;Profile=auto;Front=true'). A job is timed out after -JobMinutes; the driver logs
# every job's start / end / zip to <Out>\jobs.log and never starts a job while the previous one is still running.
#   (or -JobsFile FILE: one 'name|BenchArgs' per line; use it when starting through pwsh -File, which mangles string arrays)
param([Parameter(Mandatory)][string]$Out, [string[]]$Jobs = @(), [string]$JobsFile = '', [double]$JobMinutes = 40, [int]$GapSeconds = 60, [switch]$CheckOnly, [int]$PollSeconds = 30, [string]$RemoteScript = '')
$ErrorActionPreference = 'Stop'
if ($JobsFile) { $Jobs = @(Get-Content $JobsFile | Where-Object { $_.Trim() -and -not $_.StartsWith('#') }) }
if (-not $Jobs) { throw 'no jobs (-Jobs or -JobsFile)' }
# S102 main (2026-11-15 00:1x): @BASE = the kept levers (tools\surface\base_flags.txt), and an ab job's arm A must carry all of them (ab9b's A had
# left out T11 / T12 / T14, so every B arm was read against an older state). ';Base=none' on a line is a deliberate exception (stripped here).
$baseFlags = @(Get-Content (Join-Path $PSScriptRoot 'base_flags.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
$Jobs = @($Jobs | ForEach-Object { $_.Replace('@BASE', ($baseFlags -join ',')) })
$noBase = @{}
$Jobs = @($Jobs | ForEach-Object {
  $n0 = ($_ -split '\|', 2)[0]
  if ($_ -match '(^|;)Base=none(;|$)') { $noBase[$n0] = $true; ($_ -replace ';Base=none(?=;|$)', '' -replace '\|Base=none;', '|') } else { $_ } })
foreach ($j in $Jobs) {
  $n, $a = $j -split '\|', 2
  if ($noBase[$n] -or $a -notmatch '(^|;)Only=([^;]*,)?ab(,|;|$)') { continue }
  $armA = ''
  if ($a -match '(^|;)ArmFlags=([^;]*)') { $armA = (@($Matches[2] -split '\|' | Where-Object { $_ -match '^\s*A:' }) -join '') }
  foreach ($k in 'SceneFlags', 'ArmAFlags') { if ($a -match "(^|;)$k=([^;]*)") { $armA += ',' + $Matches[2] } }
  $have = @($armA -replace '^\s*A:', '' -split ',' | ForEach-Object { $_.Trim().TrimStart('-') })
  $miss = @($baseFlags | Where-Object { $have -notcontains $_ })
  if ($miss.Count) { throw "job $($n): arm A lacks the kept levers $($miss -join ', ') (use @BASE, or ;Base=none for a deliberate exception)" }
}
# S102 main (06:30): a job line is NAME|BenchArgs with a plain name and an Only= list; a broken line (an array append split one job in two,
# leaving ",flag=..,flag=.." as its own "job") would start a bench with default arguments. Refuse the whole list before anything starts.
foreach ($j in $Jobs) {
  $n, $a = $j -split '\|', 2
  if ($n -notmatch '^[A-Za-z0-9_.-]{1,40}$' -or -not $a -or $a -notmatch '(^|;)Only=') { throw "bad job line (want NAME|Only=...;...): $($j.Substring(0, [Math]::Min(80, $j.Length)))" }
  # S102 M5: ArmFlags=A:flags|g2:flags|g3:flags (several B arms in one ab job; the '|' is safe: the line is split at the FIRST '|' only). Refuse a malformed one here,
  # before the Surface is touched: arm A is required, a second arm too, names letters / digits, no duplicates, and not together with ArmAFlags / ArmBFlags.
  if ($a -match '(^|;)ArmFlags=([^;]*)') {
    $af = $Matches[2]
    if ($a -notmatch '(^|;)Only=([^;]*,)?ab(,|;|$)') { throw "bad job line $($n): ArmFlags needs Only=ab" }
    if ($a -match '(^|;)Arm[AB]Flags=') { throw "bad job line $($n): ArmFlags replaces ArmAFlags / ArmBFlags" }
    $names = @($af -split '\|' | Where-Object { $_.Trim() } | ForEach-Object { ($_ -split ':', 2)[0].Trim() })
    if ($names -notcontains 'A' -or $names.Count -lt 2 -or @($names | Select-Object -Unique).Count -ne $names.Count -or @($names | Where-Object { $_ -notmatch '^[A-Za-z0-9]{1,20}$' }).Count) { throw "bad job line $($n): ArmFlags wants A:flags|name:flags|... (A required, one more arm at least, letters and digits, no repeats)" }
  }
}
# S102 M7: AbFlip=<spec> (the in-run A/B, spec format src\ab_flip_math.h). Refused here, before the Surface is touched: characters, the keys, every switch must be a PerFrame entry of
# src\renderer_adapter\rdr_switches.h (the game refuses anything else at start and flips nothing: a whole job wasted), and the kept levers (@BASE) are on in EVERY phase, so a phase
# may not set one of them to off (an off phase that turned a base lever off would not be the base). An ab job's two arms are the complementary schedules: SceneFlags carries the base
# to both arms, ArmFlags / ArmAFlags / ArmBFlags are refused (they would break the pairing).
$perFrame = $null
foreach ($j in $Jobs) {
  $n, $a = $j -split '\|', 2
  if ($a -notmatch '(^|;)AbFlip=([^;]*)') { continue }
  $spec = $Matches[2]
  if ($spec -notmatch '^[A-Za-z0-9_=+/:]{8,600}$') { throw "bad job line $($n): AbFlip may hold only letters, digits and _ = + / and a colon: $spec" }
  if ($spec -notmatch '(^|/)anchor=\d') { throw "bad job line $($n): AbFlip needs anchor=<route mark id> (a pair is paired by route-mark time, never by the absolute guest frame)" }
  if ($spec -match '(^|/)first=') { throw "bad job line $($n): AbFlip must not hold first= (arm B adds it)" }
  if ($a -match '(^|;)(ArmFlags|ArmAFlags|ArmBFlags|ArmBExe)=') { throw "bad job line $($n): AbFlip is not for ArmFlags / ArmAFlags / ArmBFlags / ArmBExe (the two arms are the complementary schedules; put the base in SceneFlags)" }
  if ($null -eq $perFrame) {
    $perFrame = @{}
    $sw = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'src\renderer_adapter\rdr_switches.h'
    if (-not (Test-Path $sw)) { throw "AbFlip: $sw not found (cannot check which switches are per-frame)" }
    foreach ($m in [regex]::Matches((Get-Content $sw -Raw), 'X\(\w+,\s*"(\w+)",\s*(PerFrame|StartOnly)')) { $perFrame[$m.Groups[1].Value] = $m.Groups[2].Value }
    if ($perFrame.Count -lt 20) { throw 'AbFlip: the switch table was not read (fewer than 20 entries)' }
  }
  $phases = @($spec -split '/' | Where-Object { $_ -notmatch '^(period|settle|start|order|anchor)=' })
  if ($phases.Count -lt 2) { throw "bad job line $($n): AbFlip needs at least two phases (off=NAME=V+..., on=NAME=V+...)" }
  foreach ($ph in $phases) {
    $pn, $pv = $ph -split '=', 2
    foreach ($kv in ($pv -split '\+')) {
      $k, $v = $kv -split '=', 2
      if (-not $perFrame.ContainsKey($k)) { throw "bad job line $($n): AbFlip phase $($pn): '$k' is not in the switch table (rdr_switches.h)" }
      if ($perFrame[$k] -ne 'PerFrame') { throw "bad job line $($n): AbFlip phase $($pn): '$k' is start-only (it cannot change mid-run)" }
      if ($baseFlags -contains "$k=true" -or ($baseFlags | Where-Object { $_ -like "$k=*" })) {
        $bv = (($baseFlags | Where-Object { $_ -like "$k=*" }) | Select-Object -First 1) -replace '^[^=]*=', ''
        $norm = { param($x) if ($x -eq 'true') { '1' } elseif ($x -eq 'false') { '0' } else { $x } }
        if ((& $norm $v) -ne (& $norm $bv)) { throw "bad job line $($n): AbFlip phase $($pn) sets the kept lever $k=$v, but @BASE has $k=$bv (the base levers are on in every phase)" }
      }
    }
  }
}
# S102 B1: Warm=SECONDS / Repeats=N (bench.ps1; R28's power budget). Scene steps only, never with Only=ab; the job's own time limit must hold the estimate
# (a scene run is about 1.5 min with its start and exit; a warm-up load is a whole scene run: ceil(Warm / 40) of them).
foreach ($j in $Jobs) {
  $n, $a = $j -split '\|', 2
  $w = 0; $r = 1
  if ($a -match '(^|;)Warm=([^;]*)') { $wv = $Matches[2]; if ($wv -notmatch '^[0-9]{1,3}$' -or [int]$wv -gt 600) { throw "bad job line $($n): Warm=SECONDS wants a whole number 0 to 600 (got '$wv')" }; $w = [int]$wv }
  if ($a -match '(^|;)Repeats=([^;]*)') { $rv = $Matches[2]; if ($rv -notmatch '^[1-9]$') { throw "bad job line $($n): Repeats=N wants 1 to 9 (got '$rv')" }; $r = [int]$rv }
  if ($w -eq 0 -and $r -eq 1) { continue }
  $only = if ($a -match '(^|;)Only=([^;]*)') { @($Matches[2] -split ',' | ForEach-Object { $_.Trim() }) } else { @() }
  $scn = @($only | Where-Object { $_ -in 'frames_bryan', 'frames_brock', 'frames_match', 'frames_bryan_heavy' })
  if (-not $scn.Count -or $only -contains 'ab') { throw "bad job line $($n): Warm / Repeats are for the scene steps (Only=frames_bryan,frames_brock,frames_match), not ab or other steps" }
  $est = ($scn.Count * $r + $(if ($w) { [math]::Ceiling($w / 40) } else { 0 })) * 1.5
  if ($est -gt $JobMinutes) { throw "job $($n): Warm=$w Repeats=$r over $($scn.Count) scenes is about $([int]$est) min, the job limit is $JobMinutes min (-JobMinutes)" }
}
if ($CheckOnly) { Write-Host "job lines ok ($($Jobs.Count))"; return }
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$remote = if ($RemoteScript) { $RemoteScript } else { Join-Path $PSScriptRoot 'remote.ps1' }   # S102 TS1: -RemoteScript / -PollSeconds are for the offline hang test (wer_timeout_test.ps1)
New-Item -ItemType Directory -Force $Out | Out-Null
# S102 main (2026-11-14 20:15): one runner at a time. A switch script stopped its chain but not the old runner (a regex error), and two runners
# sent `start` to the Surface 50 s apart: the second overwrote bench_args.txt and cleared the status file under the first job. A runner now
# refuses to start while another one is alive (pid in out\surface_runner.lock).
$lockFile = Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'out\surface_runner.lock'
if (Test-Path $lockFile) {
  $other = 0; [void][int]::TryParse(((Get-Content $lockFile -ErrorAction SilentlyContinue) -join '').Trim(), [ref]$other)
  if ($other -and $other -ne $PID -and (Get-Process -Id $other -ErrorAction SilentlyContinue)) { throw "another Surface job runner is alive (pid $other, $lockFile): stop it first" }
}
Set-Content -LiteralPath $lockFile -Value $PID
# S102 main (2026-11-14 18:55): Surface results hold themselves. After the power cut the boot tidy treated the gap as "no session live" and
# deleted ab1-ab6's zips and csvs (6 h rule): only the ledger rows survived. A KEEP with a reason and an until date (tidy.py keep_info) holds
# the folder 14 days from the latest job; each start rewrites it.
Set-Content -LiteralPath (Join-Path $Out 'KEEP') -Value ("Surface bench job results (s0_5_jobs.ps1): raw zips / frames csvs behind the ledger rows. until " + (Get-Date).AddDays(14).ToString('yyyy-MM-dd'))
$log = Join-Path $Out 'jobs.log'
function Log([string]$m) { $l = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"; Add-Content $log $l; Write-Host $l }
. (Join-Path $PSScriptRoot 'wer_lib.ps1')   # S102 TS1: Save-JobWer
$werLog = { param($m) Log $m }
$i = 0
foreach ($j in $Jobs) {
  $i++
  $name, $args1 = $j -split '\|', 2
  if ($i -gt 1) { Start-Sleep $GapSeconds }
  Log "job $i/$($Jobs.Count) $name START args: $args1"
  # S102 main (2026-11-14 23:05): a start that never reached the Surface left the PREVIOUS job's 'done ok' status, and the runner fetched the
  # previous zip as this job's result. A status counts only when it is stamped after this job's start (Surface and PC clocks agree within a
  # minute: 2 min of slack); a job that never shows 'running' or a fresh status within 3 min is FAILED (never started), no fetch.
  $jobStart = Get-Date
  $startOut = @(& pwsh -NoProfile -File $remote start -BenchArgs $args1 2>&1); $startOut | ForEach-Object { Log "  $_" }
  # S102 TS1: remote.ps1 has hard ssh / scp timeouts and exits non-zero when start could not be sent (timed out, refused): a FAILED job with a log line, no 3-minute wait.
  if ($LASTEXITCODE -ne 0) { Log "job $name FAILED: start did not reach the Surface (remote.ps1 exit $LASTEXITCODE, see the lines above); next job"; continue }
  $deadline = (Get-Date).AddMinutes($JobMinutes); $done = $false; $seenFresh = $false
  while ((Get-Date) -lt $deadline) {
    Start-Sleep $PollSeconds
    $st = (& pwsh -NoProfile -File $remote status 2>&1 | Out-String)
    if ($st -match 'status: unknown') { Log "  status poll: $((($st -split "`n") | Where-Object { $_ -match 'status: unknown' } | Select-Object -First 1).Trim()) (polling on)" }   # S102 TS1: an unreadable status is neither done nor failed
    $stamp = $null
    if ($st -match 'status:\s*\w+(?:\s+\w+)?\s+(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)') { $stamp = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss', $null) }
    $fresh = $stamp -and $stamp -ge $jobStart.AddMinutes(-2)
    if ($fresh) { $seenFresh = $true }
    if ($fresh -and $st -match 'status:\s*done') { $done = $true; break }
    if (-not $seenFresh -and ((Get-Date) - $jobStart).TotalMinutes -gt 3) { break }
  }
  if (-not $seenFresh) { Log "job $name FAILED: it never started on the Surface (no fresh status within 3 min; last status: $(($st -split "`n" | Select-String 'status:') -join ' '))"; continue }
  if (-not $done) { Log "job $name TIMED OUT after $JobMinutes min (last status below); stopping the list"; Log $st; exit 3 }
  # S102 main (05:16): "done failed" means the bench made no zip; a fetch would take the PREVIOUS job's newest zip and file it under this
  # name (the dh540 arm got the dmode zip). Log FAILED with the status and move on, no fetch.
  if ($st -match 'status:\s*done\s+failed') { Log "job $name FAILED on the Surface (no zip fetched): $(($st -split "`n" | Select-String 'status:') -join ' ')"; Save-JobWer $name $jobStart (Join-Path $Out $name) $werLog; continue }
  $dest =Join-Path $Out $name
  $zip = (& pwsh -NoProfile -File $remote fetch -To $dest 2>&1 | Select-Object -Last 1)
  # S102 TS1: a fetch that timed out / failed exits non-zero (the zip stays on the Surface): FAILED with a log line, never 'DONE zip: <error text>'
  if ($LASTEXITCODE -ne 0) { Log "job $name FAILED: the zip fetch failed (remote.ps1 exit ${LASTEXITCODE}: $zip); the zip is still on the Surface"; Save-JobWer $name $jobStart $dest $werLog; continue }
  Log "job $name DONE zip: $zip"
  # S102 TS1: the GPU watchdog reports (Windows LiveKernelEvent 0x141 ...) newer than this job's start: one log line each, dumps into <job>\watchdog\ (local only). Never fails the job.
  Save-JobWer $name $jobStart $dest $werLog
}
Log "all $($Jobs.Count) jobs done"
