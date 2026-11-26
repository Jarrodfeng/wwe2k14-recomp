# S106 0FF F5 (S111: no accepted list): the verdict block of sweep_runlist.ps1, as functions (dot-source it): a missing run line or a missing / unreadable log is a FAIL, never a blank or a word the
# caller can skip. Words: PASS (run ok, triage lists no finding) | FINDINGS (run ok; the why names what triage lists: reported, NOT judged, not a failure) | FAIL (run) | FAIL (no run line) |
# FAIL (log) (S111: the run line was clean but triage.py says the RUN failed: a crash / hang line, script FAILED, no quit line, no end marker; the why = its run_failures) | FAIL (no verdict).
. "$PSScriptRoot\run_pass.ps1"   # Test-RunLinePass

function Get-RunVerdict {
  # $Hit = the run_list line of this run ('' / $null = none), $Run = its run number ('?' = unknown), $LogPath = wwe2k14_<run>.log of the exe, $Repo = the repo root (triage.py)
  param([string]$Hit, [string]$Run, [string]$LogPath, [string]$Repo)
  if (-not $Hit) { return [pscustomobject]@{ Verdict = 'FAIL (no run line)'; Why = 'run_list printed no run line for this route' } }
  $failed = ($Hit -cmatch 'script FAILED|\]\[crash\]|\[rdr\]\[live\] WARN pace_game|device lost|Failed to await|run log number MISMATCH|STILL RUNNING after|KILLED after|ABORT:') -or ($Hit -cnotmatch 'EXITED after [\d.,]+s, exit code 0\b')
  $why = ''
  if ($failed) {
    $why = ($Hit -replace '\s+', ' ').Trim()
    if ($why.Length -gt 200) { $why = $why.Substring(0, 200) }
    return [pscustomobject]@{ Verdict = 'FAIL (run)'; Why = $why }
  }
  $j = $null
  $rc = -1
  if ($Run -ne '?' -and (Test-Path $LogPath)) {
    $raw = (& py -I (Join-Path $Repo 'tools\report\triage.py') $LogPath --json 2>$null) -join "`n"
    $rc = $LASTEXITCODE      # 0 read, no run failure; 1 read, a RUN failed (S111); 2 unreadable
    if ($rc -eq 0 -or $rc -eq 1) { try { $j = $raw | ConvertFrom-Json } catch { $j = $null } }
  }
  if (-not $j -or -not $j.logs) { return [pscustomobject]@{ Verdict = 'FAIL (no verdict)'; Why = 'triage.py read nothing (log missing or unreadable)' } }
  $rf = @($j.logs[0].run_failures | Where-Object { $_ })
  if ($rc -eq 1 -or $rf.Count -gt 0) {
    $why = if ($rf.Count) { $rf -join '; ' } else { 'triage.py exit 1 (a run failure) with no run_failures in its JSON' }
    if ($why.Length -gt 200) { $why = $why.Substring(0, 200) }
    return [pscustomobject]@{ Verdict = 'FAIL (log)'; Why = $why }
  }
  $finds = @($j.logs[0].findings)
  if ($finds.Count -eq 0) { $v = 'PASS' }
  else {
    $v = 'FINDINGS'
    $why = "$($finds.Count) finding(s): " + ((@($finds | ForEach-Object { "[$($_.cls)]" }) | Select-Object -Unique | Select-Object -First 6) -join ' ')
  }
  if (Test-RunLinePass $Hit) { $why = ($why + ' route PASS').Trim() }
  return [pscustomobject]@{ Verdict = $v; Why = $why }
}

function Get-SweepSummary {
  # -> @{ Line = '== verdicts: PASS a, FINDINGS b, FAIL c'; Exit = 0 | 1 } ; exit 1 when any verdict is a FAIL (FINDINGS are reported, not judged)
  param([string[]]$Verdicts)
  $pass = @($Verdicts | Where-Object { $_ -eq 'PASS' }).Count
  $known = @($Verdicts | Where-Object { $_ -eq 'FINDINGS' }).Count
  $fail = $Verdicts.Count - $pass - $known
  # S106 0FFN N5: an empty run list judged nothing: that is a failure, not a pass
  if ($Verdicts.Count -eq 0) { return @{ Line = '== verdicts: NONE (FAIL: the run list gave no verdicts)'; Exit = 1 } }
  return @{ Line = "== verdicts: PASS $pass, FINDINGS $known, FAIL $fail (of $($Verdicts.Count))"; Exit = $(if ($fail -gt 0) { 1 } else { 0 }) }
}
