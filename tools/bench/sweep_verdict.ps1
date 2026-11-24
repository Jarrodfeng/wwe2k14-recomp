# S106 0FF F5: the verdict block of sweep_runlist.ps1, as functions (dot-source it): a missing run line or a missing / unreadable log is a FAIL, never a blank or a word the
# caller can skip. Verdict words: PASS | KNOWN (rows) | FAIL (why in the same field) | FAIL (run) | FAIL (no run line) | FAIL (no verdict). Only PASS and KNOWN are not a failure.
. "$PSScriptRoot\run_pass.ps1"   # Test-RunLinePass

function Get-RunVerdict {
  # $Hit = the run_list line of this run ('' / $null = none), $Run = its run number ('?' = unknown), $LogPath = wwe2k14_<run>.log of the exe, $Repo = the repo root (triage.py)
  param([string]$Hit, [string]$Run, [string]$LogPath, [string]$Repo, [string]$Accepted = 'tools\report\triage_accepted.json')
  if (-not $Hit) { return [pscustomobject]@{ Verdict = 'FAIL (no run line)'; Why = 'run_list printed no run line for this route' } }
  $failed = ($Hit -cmatch 'script FAILED|\]\[crash\]|\[rdr\]\[live\] WARN pace_game|device lost|Failed to await|run log number MISMATCH|STILL RUNNING after|KILLED after|ABORT:') -or ($Hit -cnotmatch 'EXITED after [\d.,]+s, exit code 0\b')
  $why = ''
  if ($failed) {
    $why = ($Hit -replace '\s+', ' ').Trim()
    if ($why.Length -gt 200) { $why = $why.Substring(0, 200) }
    return [pscustomobject]@{ Verdict = 'FAIL (run)'; Why = $why }
  }
  $tv = ''
  if ($Run -ne '?' -and (Test-Path $LogPath)) {
    $tv = @(& py -I (Join-Path $Repo 'tools\report\triage.py') $LogPath --accepted (Join-Path $Repo $Accepted) --verdict --brief 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match '^VERDICT ' }) | Select-Object -First 1
  }
  if ("$tv" -match '^VERDICT (PASS|KNOWN|FAIL)\b\s*(.*)$') { $v = $Matches[1]; $why = $Matches[2] }
  else { return [pscustomobject]@{ Verdict = 'FAIL (no verdict)'; Why = 'triage.py gave no verdict line (log missing or unreadable)' } }
  if (Test-RunLinePass $Hit) { $why = ($why + ' route PASS').Trim() }
  return [pscustomobject]@{ Verdict = $v; Why = $why }
}

function Get-SweepSummary {
  # -> @{ Line = '== verdicts: PASS a, KNOWN b, FAIL c'; Exit = 0 | 1 } ; exit 1 when any verdict is not PASS or KNOWN
  param([string[]]$Verdicts)
  $pass = @($Verdicts | Where-Object { $_ -eq 'PASS' }).Count
  $known = @($Verdicts | Where-Object { $_ -eq 'KNOWN' }).Count
  $fail = $Verdicts.Count - $pass - $known
  # S106 0FFN N5: an empty run list judged nothing: that is a failure, not a pass
  if ($Verdicts.Count -eq 0) { return @{ Line = '== verdicts: NONE (FAIL: the run list gave no verdicts)'; Exit = 1 } }
  return @{ Line = "== verdicts: PASS $pass, KNOWN $known, FAIL $fail (of $($Verdicts.Count))"; Exit = $(if ($fail -gt 0) { 1 } else { 0 }) }
}
