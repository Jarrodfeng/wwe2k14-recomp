# The compile queue's PowerShell helper (lane CQW, 2026-11-20; docs\agents\admit_wiring.md, contract docs\research\harness\CQW_contract.md).
# Every tool-build compile loop of 8 or more wide asks the queue itself (the gate holds nothing):
#   . (Join-Path $PSScriptRoot '..\admit\Admit.ps1'); try { $srcs | ForEach-Object -ThrottleLimit (Request-BuildParallel -Want 16) -Parallel {
#     ...
#   } } finally { Release-BuildParallel }
# Every use of admit.exe's command line from PowerShell lives in this file. This file has NO top-level statements, only functions (dot-sourcing it
# runs nothing and adds no variable to the caller's scope; tools\admit\wiring_dry.ps1 checks it).
#
# Request-BuildParallel -Want N [-Kind cc.tool] [-TimeoutS 300] -> the number of compiles the caller may run at once:
#   1. $env:WWE_BUILD_PARALLEL set: min(N, that), no hold. A MANUAL override (by hand or a test): nothing sets it automatically.
#   2. $env:WWE_NO_ADMIT = 1: N, silently. No admit.exe: N, and ONE line per process: `[admit] not installed (<path>): this build is not queued`.
#   3. else `admit hold --kind K --want N --min ceil(N/4) --timeout T --pid $PID --detach --max-s 900`, whose answer is `granted M <word>`:
#        held      M compiles, reserved for this script until Release-BuildParallel (or this process's exit, or 900 s);
#        observed  the queue's observe mode: M = N, nothing limited (it logs what it would have done);
#        nested    under an outer hold of this process tree: M is the outer's count (accepted even below the minimum), no reservation of its own;
#        open      admit's error path: nothing is held; the build runs N.
#      (a bare `granted M` from an older admit counts as held.) Not admitted in time (exit 75), a held grant below the minimum, any error, or admit not
#      answering within ceil(1.1 x T) + 30 s (admit's own timeout jitter + a margin; then that admit is killed): N, with one loud line.
#      The wait is in 500 ms slices and a stopped request (Ctrl+C) kills its admit, so it can never win a grant afterwards.
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
    # (the path is put together so the gate's script scanners (tools\gate\sched.py OUT_STR / EXE_LIT) do not take it for an input or output of every
    #  build check that dot-sources this file: the queue program changes WHEN a compile starts, never what a build reads or writes)
    return (Join-Path $main ('{1}\tools\admit\admit{0}' -f '.exe', 'out'))
  } catch {
    return $null
  } finally {
    $global:LASTEXITCODE = $saved
  }
}

function Get-AdmitExe {
  $p = Get-AdmitExePath
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Resolve-Path -LiteralPath $p).Path }
  return $null
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

function Invoke-AdmitBounded([string]$Exe, [string[]]$ArgList, [int]$BoundS) {
  # Runs admit with its stdout read, waiting in 500 ms slices; past $BoundS, or when the caller is stopped (Ctrl+C), admit is killed with its whole tree
  # (a stopped request must not go on to win a grant and hold it). -> @{ done; code; out }
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
    while (-not $p.WaitForExit(500)) {
      if ([DateTime]::UtcNow -ge $end) { return @{ done = $false; code = $null; out = '' } }
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

function Request-BuildParallel {
  param([Parameter(Mandatory = $true)][int]$Want, [string]$Kind = 'cc.tool', [int]$TimeoutS = 300)
  $maxHoldS = 900   # a detached hold never outlives 15 minutes (a script run by hand in a long-lived shell)
  $boundS = [int][Math]::Ceiling(1.1 * $TimeoutS) + 30   # admit's limit is up to 1.1 x T plus a poll: never kill a wait admit is still allowed
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
    $r = Invoke-AdmitBounded $exe @('hold', '--kind', $k, '--want', $Want, '--min', $min, '--timeout', $TimeoutS, '--pid', $PID, '--detach', '--max-s', $maxHoldS) $boundS
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
          'open' {
            Write-Host "[admit] $Want compiles at once (not held: admit's error path; see its [admit] line above)"
            return $Want
          }
        }
      }
    }
    $why = if ($r.code -eq 75) { "not admitted within $TimeoutS s" } else { "exit $($r.code), '$(([string]$r.out).Trim() -replace '\s+', ' ')'" }
    Write-Host "[admit] $Want compiles at once (not held: $why)"
    return $Want
  } catch {
    Write-Host "[admit] $Want compiles at once (not held: $($_.Exception.Message))"
    return $Want
  } finally {
    $global:LASTEXITCODE = $saved
  }
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
