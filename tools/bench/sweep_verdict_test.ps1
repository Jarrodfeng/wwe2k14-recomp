# S106 0FF F5: the proof of sweep_verdict.ps1 (called by triage_selftest.py). 0 game runs: the "run line" is a plain string, the log a synthetic one the selftest wrote.
#   pwsh -NoProfile -File tools\bench\sweep_verdict_test.ps1 -GoodLog X -MissingLog Y [-FlaggedLog F] [-FailLog C]   -> prints GOOD=.. / FLAGGED=.. / LOGFAIL=.. LOGFAIL_WHY=.. / MISSING=.. / NORUN=.. / CRASHED=.. / SUMMARY_OK=.. / SUMMARY_BAD=.. / EXIT_OK=n / EXIT_BAD=n
param([Parameter(Mandatory)][string]$GoodLog, [Parameter(Mandatory)][string]$MissingLog, [string]$FlaggedLog = '', [string]$FailLog = '')
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot\sweep_verdict.ps1"
$okLine = 'route.txt -> wwe2k14_999.log EXITED after 70.0s, exit code 0'
"GOOD=" + (Get-RunVerdict $okLine '999' $GoodLog $Repo).Verdict
if ($FlaggedLog) { "FLAGGED=" + (Get-RunVerdict $okLine '997' $FlaggedLog $Repo).Verdict }
if ($FailLog) { $fv = Get-RunVerdict $okLine '996' $FailLog $Repo; "LOGFAIL=" + $fv.Verdict; "LOGFAIL_WHY=" + $fv.Why }
"MISSING=" + (Get-RunVerdict $okLine '998' $MissingLog $Repo).Verdict
"NORUN=" + (Get-RunVerdict '' '?' $MissingLog $Repo).Verdict
"CRASHED=" + (Get-RunVerdict 'route.txt -> wwe2k14_999.log EXITED after 12.0s, exit code -1073741819' '999' $GoodLog $Repo).Verdict
$s1 = Get-SweepSummary @('PASS', 'FINDINGS', 'PASS')
$s2 = Get-SweepSummary @('PASS', 'FAIL (no verdict)', 'FINDINGS')
"SUMMARY_OK=" + $s1.Line
"SUMMARY_BAD=" + $s2.Line
"EXIT_OK=" + $s1.Exit
"EXIT_BAD=" + $s2.Exit
$s3 = Get-SweepSummary @()   # S106 0FFN N5: an empty run list is not a pass
"SUMMARY_EMPTY=" + $s3.Line
"EXIT_EMPTY=" + $s3.Exit
