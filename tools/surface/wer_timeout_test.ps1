# S102 TS1 planted test: Save-JobWer must return within its timeout when ssh / scp hang, and log a warning (the runner once hung ~50 min in a pipeline into ssh).
#   pwsh -NoProfile -File tools\surface\wer_timeout_test.ps1        (no ssh, no Surface: the fake is wer_fake_ssh.ps1)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wer_lib.ps1')
$fake = Join-Path $PSScriptRoot 'wer_fake_ssh.ps1'
$script:logs = New-Object System.Collections.Generic.List[string]
$lg = { param($m) $script:logs.Add($m) }
$dest = Join-Path ([IO.Path]::GetTempPath()) "wer_timeout_test_$PID"
$script:fails = 0
function T([bool]$c, [string]$w) { if ($c) { Write-Host "ok   $w" } else { Write-Host "FAIL $w"; $script:fails++ } }
try {
  $script:WerScanTimeoutSec = 4
  # 1. ssh hangs forever and never reads its stdin
  $script:WerSshExe = 'pwsh'; $script:WerSshPre = @('-NoProfile', '-File', $fake, 'hang')
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Save-JobWer 'hangjob' (Get-Date).AddMinutes(-5) $dest $lg
  T ($sw.Elapsed.TotalSeconds -lt 20) "Save-JobWer returned in $([math]::Round($sw.Elapsed.TotalSeconds,1)) s with a hanging ssh (timeout 4 s)"
  T (($script:logs -join "`n") -match 'hangjob: WER listing failed.*timed out after 4 s') "the warning names the timeout: $($script:logs -join ' / ')"
  T (-not (Test-Path (Join-Path $dest 'watchdog\reports.txt'))) 'no reports.txt is written for a failed listing (the readers print "no listing")'
  Start-Sleep 1
  T (-not (Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | Where-Object { $_.CommandLine -match 'wer_fake_ssh' })) 'the hung child process was killed'
  # 2. ssh fails at once
  $script:logs.Clear(); $script:WerSshPre = @('-NoProfile', '-File', $fake, 'fail')
  Save-JobWer 'failjob' (Get-Date).AddMinutes(-5) $dest $lg
  T (($script:logs -join "`n") -match 'failjob: WER listing failed.*exit 1.*connection refused') "a failing ssh is a warning: $($script:logs -join ' / ')"
  # 3. Invoke-Timed itself
  $sw.Restart(); $r = Invoke-Timed 'pwsh' @('-NoProfile', '-Command', 'Start-Sleep 60') -TimeoutSec 3
  T ($r.TimedOut -and $sw.Elapsed.TotalSeconds -lt 15) "Invoke-Timed kills a sleeper at its limit ($([math]::Round($sw.Elapsed.TotalSeconds,1)) s)"
  $r = Invoke-Timed 'pwsh' @('-NoProfile', '-Command', '[Console]::In.ReadToEnd().ToUpper(); exit 7') -Stdin 'abc' -TimeoutSec 30
  T (-not $r.TimedOut -and $r.Exit -eq 7 -and $r.Out.Trim() -eq 'ABC') 'Invoke-Timed: stdin in, stdout and exit code back'
  # 4. remote.ps1 (start / status / fetch) with every ssh / scp hanging: each returns at the limit (3 s here), status as "status: unknown" with exit 0, start / fetch non-zero
  $env:WWE_REMOTE_FAKE = $fake; $env:WWE_REMOTE_FAKE_MODE = 'hang'; $env:WWE_REMOTE_TIMEOUT_SEC = '3'
  $rem = Join-Path $PSScriptRoot 'remote.ps1'
  $sw.Restart(); $o = (& pwsh -NoProfile -File $rem status 2>&1 | Out-String); $ec = $LASTEXITCODE
  T ($sw.Elapsed.TotalSeconds -lt 20 -and $ec -eq 0 -and $o -match 'status: unknown \(ssh to surface timed out after 3 s') "remote.ps1 status with a hung ssh: 'status: unknown', exit 0, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s ($($o.Trim()))"
  $sw.Restart(); $o = (& pwsh -NoProfile -File $rem start -BenchArgs 'Only=frames_bryan' 2>&1 | Out-String); $ec = $LASTEXITCODE
  T ($sw.Elapsed.TotalSeconds -lt 20 -and $ec -ne 0 -and $o -match 'timed out after 3 s') "remote.ps1 start with a hung scp: non-zero exit, clear message, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
  $sw.Restart(); $o = (& pwsh -NoProfile -File $rem fetch -To $dest 2>&1 | Out-String); $ec = $LASTEXITCODE
  T ($sw.Elapsed.TotalSeconds -lt 20 -and $ec -ne 0 -and $o -match 'timed out after 3 s') "remote.ps1 fetch with a hung ssh: non-zero exit, clear message, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
  # 5. the RUNNER's poll loop: start answers, every status poll hangs. It must keep polling ("status unknown", not done / failed) and end at ITS OWN deadline (JobMinutes 0.25 = 15 s): exit 3, TIMED OUT.
  $env:WWE_REMOTE_FAKE_MODE = 'sthang'
  $rd = Join-Path $dest 'runner'; New-Item -ItemType Directory -Force $rd | Out-Null
  $jf = Join-Path $rd 'jobs.txt'; Set-Content $jf 'hungstatus|Only=frames_bryan'
  $sw.Restart(); $o = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 's0_5_jobs.ps1') -Out (Join-Path $rd 'out') -JobsFile $jf -JobMinutes 0.25 -PollSeconds 1 -GapSeconds 0 2>&1 | Out-String); $ec = $LASTEXITCODE
  $log = Get-Content (Join-Path $rd 'out\jobs.log') -Raw
  $polls = ([regex]::Matches($log, 'status poll: status: unknown')).Count
  T ($ec -eq 0 -and $log -match 'job hungstatus FAILED: it never started' -and $polls -ge 2 -and $sw.Elapsed.TotalSeconds -lt 90) "runner, status never readable: $polls 'status unknown' polls, then its own 'never started' FAILED, the list ends, exit $ec, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
  # 5b. a fresh 'running' status once, then every poll hangs: the runner keeps polling and ends at ITS deadline with TIMED OUT (exit 3)
  $env:WWE_REMOTE_FAKE_MODE = 'stfresh'; $env:WWE_REMOTE_FAKE_STATE = "$PID"
  Set-Content $jf 'freshthenhang|Only=frames_bryan'; Remove-Item (Join-Path $rd 'out\jobs.log')
  $sw.Restart(); $o = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 's0_5_jobs.ps1') -Out (Join-Path $rd 'out') -JobsFile $jf -JobMinutes 0.25 -PollSeconds 1 -GapSeconds 0 2>&1 | Out-String); $ec = $LASTEXITCODE
  $log = Get-Content (Join-Path $rd 'out\jobs.log') -Raw
  $polls = ([regex]::Matches($log, 'status poll: status: unknown')).Count
  T ($ec -eq 3 -and $log -match 'job freshthenhang TIMED OUT after 0.25 min' -and $polls -ge 2 -and $sw.Elapsed.TotalSeconds -lt 90) "runner, status readable once then hung: $polls 'status unknown' polls, then its own TIMED OUT at the deadline, exit $ec, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
  Remove-Item (Join-Path ([IO.Path]::GetTempPath()) "wer_fake_stfresh_$PID.flag") -ErrorAction SilentlyContinue
  # 6. the runner when START cannot be sent (every ssh hangs): FAILED at once with a log line, the next job is tried
  $env:WWE_REMOTE_FAKE_MODE = 'hang'
  Set-Content $jf @('badstart|Only=frames_bryan', 'badstart2|Only=frames_bryan')
  Remove-Item (Join-Path $rd 'out\jobs.log')
  $sw.Restart(); $o = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 's0_5_jobs.ps1') -Out (Join-Path $rd 'out') -JobsFile $jf -JobMinutes 0.25 -PollSeconds 1 -GapSeconds 0 2>&1 | Out-String); $ec = $LASTEXITCODE
  $log = Get-Content (Join-Path $rd 'out\jobs.log') -Raw
  T ($ec -eq 0 -and $log -match 'job badstart FAILED: start did not reach the Surface' -and $log -match 'job badstart2 FAILED: start did not reach' -and $log -match 'all 2 jobs done' -and $sw.Elapsed.TotalSeconds -lt 60) "runner with start unreachable: both jobs FAILED with a log line, the list ends normally, $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
} finally { Remove-Item Env:WWE_REMOTE_FAKE, Env:WWE_REMOTE_FAKE_MODE, Env:WWE_REMOTE_TIMEOUT_SEC -ErrorAction SilentlyContinue; Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue }
if ($script:fails) { Write-Host "$($script:fails) FAILED"; exit 1 } else { Write-Host 'ALL OK'; exit 0 }
