# The compile queue's PowerShell helper (lane CQW, 2026-11-20; docs\agents\admit_wiring.md, contract docs\research\harness\CQW_contract.md).
# Every tool-build compile loop of 8 or more wide asks the queue itself (nothing above it holds a slot for it):
#   . (Join-Path $PSScriptRoot '..\admit\Admit.ps1'); try { $srcs | ForEach-Object -ThrottleLimit (Request-BuildParallel -Want 16) -Parallel {
#     ...
#   } } finally { Release-BuildParallel }
# Every use of admit.exe's command line from PowerShell lives in this file. This file has NO top-level statements, only functions (dot-sourcing it
# runs nothing and adds no variable to the caller's scope).
#
# Request-BuildParallel -Want N [-Kind cc.tool] [-TimeoutS 300] -> the number of compiles the caller may run at once:
#   1. $env:WWE_BUILD_PARALLEL set: min(N, that), no hold. A MANUAL override (by hand or a test): nothing sets it automatically.
#   2. $env:WWE_NO_ADMIT = 1: N, silently. No admit.exe: N, and ONE line per process: `[admit] not installed (<path>): this build is not queued`.
#   3. else `admit hold --kind K --want N --min ceil(N/4) --timeout T --pid $PID --detach --max-s 900`, whose answer is `granted M <word>`:
#        held      M compiles, reserved for this script until Release-BuildParallel (or this process's exit, or 900 s);
#        observed  the queue's observe mode: M = N, nothing limited (it logs what it would have done);
#        nested    under an outer hold of this process tree: M is the outer's count (accepted even below the minimum), no reservation of its own;
#        open      admit's error path: nothing is held; the build runs N.
#        narrowed  (S109 GATE) at the wait bound, below the minimum but above the floor: M compiles, held like `held` (an older admit says `forced`).
#      (a bare `granted M` from an older admit counts as held.) A held grant below the minimum, an error exit, or admit not answering within
#      ceil(1.1 x T) + 30 s (then that admit is killed): N, with one loud line.
#      REFUSED (S109 GATE): not admitted within min(T, admit.json timeout_s = 180 s) = admit exits 75; Request-BuildParallel THROWS
#      "[admit] REFUSED ..." (the reason, the holders, since when): the script stops, its try / finally releases, its caller prints it. Nothing ran.
#      The wait is in 500 ms slices and a stopped request (Ctrl+C) kills its admit, so it can never win a grant afterwards.
# Get-AdmitExe (S109 GATE): when the exe's source stamp (admit.exe.sha256) differs from the main checkout's admit.cpp it runs build_admit.ps1 -IfStale
# in a child pwsh first (its compile admitted by the existing exe in one bounded ask; not admitted / failed = the old exe, said once): the exe cannot run
# stale past the first ask after a merge. That rebuild-on-ask stands in for a Python port of the gate.
# Release-BuildParallel: ends THIS script's own `held` or `observed` hold (`admit release --pid $PID --kind K`); never a nested / open one, never another
# script's (the record lives in the calling script's own scope, $script:).
# Get-AdmitExe: $env:WWE_ADMIT_EXE when set (tests: a stub or a worktree's own exe), else the MAIN checkout's out\tools\admit\admit.exe, found through
# git's common dir (as tools\scene\nr_check.ps1 does), so a worktree uses the one machine-wide program; $null when the file is missing.

function Get-AdmitExePath {
  # where the exe should be (it may be missing): the override, else the main checkout's
  if ($env:WWE_ADMIT_EXE) { return $env:WWE_ADMIT_EXE }
  $saved = Get-Variable -Name LASTEXITCODE -Scope Global -ValueOnly -ErrorAction SilentlyContinue   # (strict mode: it may be unset in a fresh session)
  try {
    $common = (& git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir 2>$null)
    if (-not $common) { return $null }
    $main = Split-Path ([string]$common).Trim() -Parent
    return (Join-Path $main ('{1}\tools\admit\admit{0}' -f '.exe', 'out'))
  } catch {
    return $null
  } finally {
    $global:LASTEXITCODE = $saved
  }
}

function Test-AdmitExeCurrent([string]$Exe) {
  # S109 GATE: $true = the stamp is the SHA-256 of <main>\tools\admit\admit.cpp; $false = missing or different; $null = no source to compare
  $src = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $Exe -Parent) -Parent) -Parent) -Parent) 'tools\admit\admit.cpp'
  if (-not (Test-Path -LiteralPath $src)) { return $null }
  $st = "$Exe.sha256"
  if (-not (Test-Path -LiteralPath $st)) { return $false }
  return ([IO.File]::ReadAllText($st).Trim().ToLowerInvariant() -eq (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash.ToLowerInvariant())
}

function Get-AdmitExe {
  $p = Get-AdmitExePath
  if (-not $p -or -not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
  $p = (Resolve-Path -LiteralPath $p).Path
  if (-not $env:WWE_ADMIT_EXE -and (Test-AdmitExeCurrent $p) -eq $false -and -not [AppDomain]::CurrentDomain.GetData('WWE_ADMIT_REBUILT')) {   # tests name their own exe: never rebuilt
    [AppDomain]::CurrentDomain.SetData('WWE_ADMIT_REBUILT', $true)   # once per process
    $script = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $p -Parent) -Parent) -Parent) -Parent) 'tools\admit\build_admit.ps1'
    # GATE review: a pre-GATE build_admit.ps1 ignores -IfStale and compiles at once, never asking the queue: not run (this tree is ahead of main)
    if (-not (Test-Path -LiteralPath $script) -or -not (Select-String -LiteralPath $script -SimpleMatch 'IfStale' -Quiet)) {
      Write-Host "[admit] admit.exe has no source stamp and $script predates the rebuild-on-ask (S109 GATE): this ask uses the exe as it is"
      return $p
    }
    $saved = Get-Variable -Name LASTEXITCODE -Scope Global -ValueOnly -ErrorAction SilentlyContinue
    try {
      $lines = & pwsh -NoProfile -File $script -IfStale 2>&1
      foreach ($ln in $lines) { if ("$ln" -match '^(\[admit\]|built )') { Write-Host "$ln" } }
    } catch {
      Write-Host "[admit] admit.exe is older than admit.cpp and its rebuild could not start ($($_.Exception.Message)): this ask uses the old exe"
    } finally { $global:LASTEXITCODE = $saved }
    if ((Test-AdmitExeCurrent $p) -eq $false) { Write-Host '[admit] admit.exe is still older than admit.cpp: this ask uses the old exe (session start shows it as STALE)' }
  }
  return $p
}

function Write-AdmitNotInstalled([string]$Path) {
  # once per process (an AppDomain flag: not inherited by children, never a variable in the caller's scope)
  if ([AppDomain]::CurrentDomain.GetData('WWE_ADMIT_NOT_INSTALLED_SAID')) { return }
  [AppDomain]::CurrentDomain.SetData('WWE_ADMIT_NOT_INSTALLED_SAID', $true)
  Write-Host "[admit] not installed ($(if ($Path) { $Path } else { 'no git checkout found' })): this build is not queued"
}

function Resolve-AdmitKind([string]$Kind) {
  # A kind name this wiring uses -> the name admit.json has (tools\admit\tool_build_parallel.json kind_alias, the one place; empty today).
  try {
    $t = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'tool_build_parallel.json') | ConvertFrom-Json
    $a = $t.kind_alias.$Kind
    if ($a) { return [string]$a }
  } catch { }
  return $Kind
}

function Get-AdmitPausePath {
  # S106 C5: the alone run's pause file (tools\runlock.ps1 -Alone): $env:WWE_ALONE_FILE (tests), else <main checkout>\out\harness\runlock\alone.lock, the main
  # checkout found through git's common dir (as Get-AdmitExePath does). One git call per process.
  if ($env:WWE_ALONE_FILE) { return $env:WWE_ALONE_FILE }
  $c = [AppDomain]::CurrentDomain.GetData('WWE_ALONE_FILE_PATH')
  if ($null -ne $c) { return [string]$c }
  $saved = Get-Variable -Name LASTEXITCODE -Scope Global -ValueOnly -ErrorAction SilentlyContinue
  $path = ''
  try {
    $common = (& git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir 2>$null)
    if ($common) { $path = Join-Path (Split-Path ([string]$common).Trim() -Parent) ('{0}\harness\runlock\alone.lock' -f 'out') }
  } catch { } finally { $global:LASTEXITCODE = $saved }
  [AppDomain]::CurrentDomain.SetData('WWE_ALONE_FILE_PATH', $path)
  return $path
}

function Get-AdmitPauseLeft {
  # S106 C5: seconds a LIVE alone pause has left (0 = none, expired, or this process's own tree). Liveness = the OS lock on byte 0 (FileStream.Lock: the same
  # Win32 lock admit.exe and python's msvcrt take), never the record. A live pause with no readable `until` counts as 900 s.
  $f = Get-AdmitPausePath
  if (-not $f -or -not [System.IO.File]::Exists($f)) { return 0.0 }
  $fs = $null
  try {
    $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete, 1)
  } catch { return 0.0 }
  try {
    try { $fs.Lock(0, 1); $fs.Unlock(0, 1); return 0.0 } catch [System.IO.IOException] { }   # held: a live pause
    $rec = $null
    try {
      $fs.Position = 1
      $buf = New-Object byte[] 8192
      $n = $fs.Read($buf, 0, 8192)
      if ($n -gt 0) { $rec = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n) | ConvertFrom-Json }
    } catch { }
    if ($rec -and $rec.token -and (("$($rec.token)" -eq $env:WWE2K14_RUNLOCK_TOKEN) -or ("$($rec.token)" -eq $env:WWE_ALONE_TOKEN))) { return 0.0 }   # S106 C5F note 3: WWE_ALONE_TOKEN = the alone run's dedicated token
    $until = 0.0
    if ($rec -and $rec.until) { $until = [double]$rec.until }
    if ($until -gt 0) { return [Math]::Max(0.0, $until - [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0) }
    return 900.0
  } finally { $fs.Dispose() }
}

function Invoke-AdmitBounded([string]$Exe, [string[]]$ArgList, [int]$BoundS, [bool]$PauseAware = $false) {
  # Runs admit with its stdout read, waiting in 500 ms slices; past $BoundS, or when the caller is stopped (Ctrl+C), admit is killed with its whole tree
  # (a stopped request must not go on to win a grant and hold it). -> @{ done; code; out }
  # S106 C5: -PauseAware (a hold): the bound grows by the time an alone run's pause holds admit off (admit does not count it), and while
  # a pause is live admit is never killed (it would run the caller unseen).
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $ArgList) { $psi.ArgumentList.Add([string]$a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.CreateNoWindow = $true
  $p = [Diagnostics.Process]::Start($psi)
  $finished = $false
  try {
    $read = $p.StandardOutput.ReadToEndAsync()
    $end = [DateTime]::UtcNow.AddSeconds($BoundS)
    $lastT = [DateTime]::UtcNow
    while (-not $p.WaitForExit(500)) {
      $now = [DateTime]::UtcNow
      $paused = $PauseAware -and ((Get-AdmitPauseLeft) -gt 0)
      if ($paused) { $end = $end.AddSeconds(($now - $lastT).TotalSeconds) }
      $lastT = $now
      if (-not $paused -and $now -ge $end) { return @{ done = $false; code = $null; out = '' } }
    }
    $p.WaitForExit()
    $finished = $true
    $out = if ($read.Wait(5000)) { $read.Result } else { '' }
    return @{ done = $true; code = $p.ExitCode; out = $out }
  } finally {
    if (-not $finished) { try { if (-not $p.HasExited) { $p.Kill($true) } } catch { } }
    $p.Dispose()
  }
}

function Get-AdmitCfgTimeoutS {
  # S106 0CF (review B5): the queue's own wait limit (admit.json timeout_s, 900): $env:WWE_ADMIT_CONFIG when set, else the file beside this script
  try {
    $f = if ($env:WWE_ADMIT_CONFIG) { $env:WWE_ADMIT_CONFIG } else { Join-Path $PSScriptRoot 'admit.json' }
    $v = (Get-Content -Raw -LiteralPath $f | ConvertFrom-Json).timeout_s
    if ($null -ne $v -and [double]$v -gt 0) { return [int][Math]::Ceiling([double]$v) }
  } catch { }
  return 900
}

function Request-BuildParallel {
  # -Interactive: a person is waiting (keeps the caller's short limit); otherwise the limit is at least the queue's own (admit.json timeout_s)
  # S109 GATE: -Interactive is kept for the callers' signature; every limit is passed as given and admit cuts it to admit.json timeout_s
  param([Parameter(Mandatory = $true)][int]$Want, [string]$Kind = 'cc.tool', [int]$TimeoutS = 300, [switch]$Interactive)
  $maxHoldS = 900   # a detached hold never outlives 15 minutes (a script run by hand in a long-lived shell)
  $graceS = 30; $gv = 0; if ([int]::TryParse([string]$env:WWE_ADMIT_GRACE_S, [ref]$gv) -and $gv -ge 0) { $graceS = $gv }   # (S106 C5: a test shortens it)
  $boundS = [int][Math]::Ceiling(1.1 * [Math]::Min($TimeoutS, (Get-AdmitCfgTimeoutS))) + $graceS   # admit answers within min(T, the queue's limit) + its last ask
  $refusal = $null
  if ($Want -lt 1) { $Want = 1 }
  $g = $env:WWE_BUILD_PARALLEL
  if ($g) {
    $n = 0
    if ([int]::TryParse($g, [ref]$n) -and $n -ge 1) {
      $r = [Math]::Min($Want, $n)
      Write-Host "[admit] $r compiles at once (WWE_BUILD_PARALLEL=$g, a manual override; this build asks $Want; not queued)"
      return $r
    }
    Write-Host "[admit] WWE_BUILD_PARALLEL='$g' is not a count of 1 or more: ignored"
  }
  if ($env:WWE_NO_ADMIT -eq '1') { return $Want }
  $exe = Get-AdmitExe
  if (-not $exe) { Write-AdmitNotInstalled (Get-AdmitExePath); return $Want }
  $min = [int][Math]::Ceiling($Want / 4.0)
  $saved = Get-Variable -Name LASTEXITCODE -Scope Global -ValueOnly -ErrorAction SilentlyContinue
  try {
    $k = Resolve-AdmitKind $Kind
    $hargs = @('hold', '--kind', $k, '--want', $Want, '--min', $min, '--timeout', $TimeoutS, '--pid', $PID, '--detach', '--max-s', $maxHoldS)
    $r = Invoke-AdmitBounded $exe $hargs $boundS $true   # S106 C5: pause-aware
    if (-not $r.done) {
      Write-Host "[admit] $Want compiles at once (not held: admit gave no answer in $boundS s and was killed)"
      return $Want
    }
    $m = [regex]::Match([string]$r.out, '(?m)^granted (\d+)(?:[ \t]+(\w+))?[ \t]*\r?$')
    if ($r.code -eq 0 -and $m.Success) {
      $n = [int]$m.Groups[1].Value
      $word = if ($m.Groups[2].Success) { $m.Groups[2].Value } else { 'held' }   # (an older admit prints a bare `granted N`)
      if ($n -ge 1 -and $n -le $Want) {
        switch ($word) {
          'held' {
            if ($n -ge $min) {
              $script:AdmitOwnHold = @{ exe = $exe; kind = $k; pid = $PID }
              Write-Host "[admit] $n compiles at once (held: granted $n of $Want, $k, until the loop ends, at most $maxHoldS s)"
              return $n
            }
            $null = Invoke-AdmitBounded $exe @('release', '--pid', $PID, '--kind', $k) 30   # a held grant below the minimum is given back
            Write-Host "[admit] $Want compiles at once (not held: granted $n, below the minimum $min; given back)"
            return $Want
          }
          'nested' {
            $mk = [regex]::Match([string]$r.out, '(?m)^marker (\d+)')
            $under = if ($mk.Success) { "pid $($mk.Groups[1].Value)" } else { 'an outer hold of this process tree' }
            Write-Host "[admit] $n compiles at once (nested under $under; no reservation of its own)"
            return $n
          }
          'observed' {
            $script:AdmitOwnHold = @{ exe = $exe; kind = $k; pid = $PID }   # its own record: released when the loop ends, so it never lingers in later decisions
            Write-Host "[admit] $n compiles at once (observed: not limited; the queue logs what it would have done)"
            return $n
          }
          { $_ -in 'narrowed', 'forced' } {   # S109 GATE: at the wait bound, below the minimum but above the floor (an older admit says `forced`)
            $script:AdmitOwnHold = @{ exe = $exe; kind = $k; pid = $PID }
            Write-Host "[admit] $n compiles at once (narrowed at the wait bound: below the minimum $min, $n of $Want, $k, until the loop ends)"
            return $n
          }
          'open' {
            Write-Host "[admit] $Want compiles at once (not held: admit's error path; see its [admit] line above)"
            return $Want
          }
        }
      }
    }
    if ($r.code -eq 75) {   # S109 GATE: refused (an older admit prints no `refused` line); thrown below, outside this catch
      $rl = [regex]::Match([string]$r.out, '(?m)^refused (.*?)\r?$')
      $refusal = if ($rl.Success) { $rl.Groups[1].Value } else { "not admitted within $TimeoutS s (exit 75)" }
    } else {
      Write-Host "[admit] $Want compiles at once (not held: exit $($r.code), '$(([string]$r.out).Trim() -replace '\s+', ' ')')"
      return $Want
    }
  } catch {
    Write-Host "[admit] $Want compiles at once (not held: $($_.Exception.Message))"
    return $Want
  } finally {
    $global:LASTEXITCODE = $saved
  }
  throw "[admit] $Kind build of $Want`: $refusal (this script stops: nothing was compiled)"
}

function Release-BuildParallel {
  # only this script's own HELD or OBSERVED hold (Request-BuildParallel records nothing for nested / open answers or a fall-open)
  $h = Get-Variable -Name AdmitOwnHold -Scope Script -ValueOnly -ErrorAction SilentlyContinue   # (strict mode: never read an unset variable)
  if (-not $h) { return }
  $script:AdmitOwnHold = $null
  $saved = Get-Variable -Name LASTEXITCODE -Scope Global -ValueOnly -ErrorAction SilentlyContinue
  try {
    $r = Invoke-AdmitBounded $h.exe @('release', '--pid', $h.pid, '--kind', $h.kind) 30
    if (-not $r.done -or $r.code -ne 0) { Write-Host "[admit] release --pid $($h.pid) --kind $($h.kind) did not finish cleanly (the hold ends with this process or after 900 s)" }
  } catch {
    Write-Host "[admit] release failed: $($_.Exception.Message) (the hold ends with this process or after 900 s)"
  } finally {
    $global:LASTEXITCODE = $saved
  }
}
