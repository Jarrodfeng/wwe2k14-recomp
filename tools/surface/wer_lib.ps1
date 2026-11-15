# S102 TS1: Surface GPU-watchdog reports of one job (a Windows LiveKernelEvent: Kernel_141 = GPU engine timeout, Kernel_1a8 / _117 / _1b8 the same family).
# Functions only (dot-sourced by s0_5_jobs.ps1, wer_reports.ps1, rplay_remote.ps1; no param block, so it cannot overwrite the caller variables).
# Every Vulkan device loss we found on the Surface left one, time-equal to the loss (docs\LESSONS.md, docs\renderer\DEVICE_LOST.md "Surface tools").
$script:WerScan = Join-Path $PSScriptRoot 'wer_scan.ps1'
# S102 TS1 fix (2026-11-15 ~08:50): a pipeline into ssh hung the job runner for ~50 min (ab33 relbrock). EVERY ssh / scp call now runs as its own process through
# Invoke-Timed below: stdin written by an async write (never a blocking pipe write), output read by async tasks, a hard timeout, kill of the process tree on timeout.
# The executables can be replaced for the planted test (tools\surface\wer_timeout_test.ps1): a fake ssh that sleeps forever.
$script:WerSshExe = 'ssh'; $script:WerSshPre = @()     # leading args before ssh's own (the fake)
$script:WerScpExe = 'scp'; $script:WerScpPre = @()
$script:WerScanTimeoutSec = 60; $script:WerCopyTimeoutSec = 120; $script:WerCleanTimeoutSec = 30

# Runs a process with a hard timeout. -Stdin text goes in by an async write; -ArgList (quoted by .NET) or -RawArgs (one string). Returns @{ Exit; Out; Err; TimedOut }.
# Never blocks past the timeout (+ about 5 s to read the pipes) and never throws for a hung or failed child (the caller reads TimedOut / Exit).
function Invoke-Timed([string]$Exe, [string[]]$ArgList = @(), [string]$RawArgs = '', $Stdin = $null, [int]$TimeoutSec = 60) {
  $psi = New-Object Diagnostics.ProcessStartInfo $Exe
  if ($RawArgs) { $psi.Arguments = $RawArgs } else { foreach ($a in $ArgList) { $psi.ArgumentList.Add($a) } }
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardInput = ($null -ne $Stdin)
  try { $p = [Diagnostics.Process]::Start($psi) } catch { return @{ Exit = -1; Out = ''; Err = "could not start $Exe`: $($_.Exception.Message)"; TimedOut = $false } }
  $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
  $wt = $null; $closed = $false
  if ($null -ne $Stdin) { $bytes = [Text.Encoding]::UTF8.GetBytes($Stdin); $wt = $p.StandardInput.BaseStream.WriteAsync($bytes, 0, $bytes.Length) }
  $sw = [Diagnostics.Stopwatch]::StartNew(); $timedOut = $false
  while (-not $p.HasExited) {
    if ($wt -and -not $closed -and $wt.IsCompleted) { try { $p.StandardInput.Close() } catch {}; $closed = $true }
    if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) { $timedOut = $true; try { $p.Kill($true) } catch {}; break }
    Start-Sleep -Milliseconds 100
  }
  if (-not $timedOut) { [void]$p.WaitForExit(5000) }
  $out = ''; $err = ''
  if ($so.Wait(5000)) { $out = $so.Result }
  if ($se.Wait(5000)) { $err = $se.Result }
  @{ Exit = $(if ($timedOut) { -2 } elseif ($p.HasExited) { $p.ExitCode } else { -3 }); Out = $out; Err = $err; TimedOut = $timedOut }
}
$script:SshBase = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4')

# Runs wer_scan.ps1 on the Surface (script sent through ssh stdin: no command-line length limit, nothing written there but the staged dumps). Throws on a failure or a timeout.
function Invoke-WerScan([datetime]$Since, [string]$StageDir, [string]$HostName = 'surface') {
  $fmt = 'yyyy-MM-ddTHH:mm:ss'
  $body = Get-Content $script:WerScan -Raw
  # The script goes through stdin and is built there with ScriptBlock.Create (a plain `-Command -` swallowed the multi-line block silently); the values hold no spaces.
  $args1 = "-Since $($Since.ToString($fmt)) -PcNow $((Get-Date).ToString($fmt))" + $(if ($StageDir) { " -Stage $StageDir" } else { '' })
  $remote = "powershell -NoProfile -NonInteractive -Command `"& ([scriptblock]::Create([Console]::In.ReadToEnd())) $args1`""
  $r = Invoke-Timed $script:WerSshExe (@($script:WerSshPre) + $script:SshBase + @($HostName, $remote)) -Stdin $body -TimeoutSec $script:WerScanTimeoutSec
  if ($r.TimedOut) { throw "ssh listing timed out after $($script:WerScanTimeoutSec) s (killed)" }
  if ($r.Exit -ne 0) { throw "ssh listing failed (exit $($r.Exit)): $(($r.Err -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1))" }
  @($r.Out -split "`r?`n" | Where-Object { $_ -match '^(NOW|REP|WARN)\|' })
}

# After a job: list the reports newer than $JobStart, log one line per report, copy the dumps to <Dest>\watchdog\ (local only), write reports.txt there (even when empty:
# "listed, none" is a result the bench_line / ab_line readers print). Never throws and never waits past its timeouts (60 s listing, 120 s copy, 30 s cleanup):
# a failed or hung listing logs a warning and returns.
function Save-JobWer([string]$Name, [datetime]$JobStart, [string]$Dest, [scriptblock]$Log, [string]$StageDir = 'C:\wwe\out\wer_stage', [string]$HostName = 'surface') {
  try {
    $stageJob = "$StageDir\$Name"
    $lines = Invoke-WerScan $JobStart $stageJob $HostName
    $reps = @($lines | Where-Object { $_ -like 'REP|*' })
    foreach ($w in @($lines | Where-Object { $_ -like 'WARN|*' })) { & $Log "job $($Name): WER listing warning: $($w.Substring(5))" }
    $wd = Join-Path $Dest 'watchdog'
    New-Item -ItemType Directory -Force $wd | Out-Null
    Set-Content -LiteralPath (Join-Path $wd 'reports.txt') -Value (@("# job $Name, listed after the job; since $($JobStart.ToString('yyyy-MM-dd HH:mm:ss')) (PC clock)") + $lines)
    foreach ($r in $reps) {
      $f = $r -split '\|'   # REP|code|time|dump|dir|staged
      & $Log "job $($Name): GPU watchdog report $($f[1]) at $($f[2].Replace('T', ' ')) (dump $($f[3]))"
    }
    $staged = @($reps | ForEach-Object { ($_ -split '\|')[5] } | Where-Object { $_ -and $_ -ne '-' })
    if ($staged.Count) {
      $c = Invoke-Timed $script:WerScpExe (@($script:WerScpPre) + @('-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-r', "${HostName}:$($stageJob -replace '\\', '/')/*", "$wd\")) -TimeoutSec $script:WerCopyTimeoutSec
      if ($c.TimedOut) { & $Log "job $($Name): WER dump copy timed out after $($script:WerCopyTimeoutSec) s (killed); reports.txt is kept" }
      elseif ($c.Exit -ne 0) { & $Log "job $($Name): WER dump copy failed (scp exit $($c.Exit)); reports.txt is kept" }
      # Remove only the staging folder this call made (never anything else on the Surface).
      if ($stageJob -match '\\(wer_stage|ts1_test\\stage)\\[A-Za-z0-9_.-]+$') {
        $q = $stageJob.Replace("'", "''")
        $d = Invoke-Timed $script:WerSshExe (@($script:WerSshPre) + $script:SshBase + @($HostName, 'powershell -NoProfile -NonInteractive -Command -')) -Stdin "Remove-Item -LiteralPath '$q' -Recurse -Force -ErrorAction SilentlyContinue`n" -TimeoutSec $script:WerCleanTimeoutSec
        if ($d.TimedOut) { & $Log "job $($Name): WER staging cleanup timed out (killed); C:\wwe\out\wer_stage\$Name may remain" }
      }
    }
  } catch { & $Log "job $($Name): WER listing failed (warning, the job result is unaffected): $($_.Exception.Message)" }
}
