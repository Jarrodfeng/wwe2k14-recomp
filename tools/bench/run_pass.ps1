# S105 H: the PASS word on a run_list line. A route's own PASS line (a `log <tag>: PASS` line, which the game logs as "[wwe2k14][input] line N: <tag>: PASS";
# release_smoke.txt carries `log smoke: PASS`) counts only when the run itself was clean: exit code 0 (EXITED ... exit code 0), no failure line
# (script FAILED, [crash], ...), and no STILL RUNNING / KILLED / ABORT. Case-sensitive on purpose: `[wwe2k14][fast_loader] at frame N: passes=...` is not a route PASS.
# A run that crashed (0xC0000005) or whose route stopped at a refused cvar never reads PASS (sweep runs 045-048, 052, 058, 064).
$script:RunPassRx = '\]\[input\] line \d+: \S+: PASS\b'
$script:RunExitRx = '^(EXITED after|STILL RUNNING after|KILLED after|ABORT:)'   # review F1: the exit column takes only real exit lines, not "depth-convert passes still recorded"
function Get-RunExitColumn($out) {
    (($out | Select-String -CaseSensitive $script:RunExitRx | ForEach-Object Line) -join ' ')
}
function Get-RunPassWord($out, [string]$exit, [string]$fail) {
    if (-not ($out | Select-String -CaseSensitive $script:RunPassRx)) { return '' }
    if ($fail) { return '' }
    if ($exit -cnotmatch 'EXITED after [\d.,]+s, exit code 0\b') { return '' }
    if ($exit -cmatch 'STILL RUNNING after|KILLED after|ABORT:') { return '' }
    return ' PASS'
}
# the consumers (release.ps1, sweep_runlist.ps1) read the word off a whole run line: a standalone PASS only
function Test-RunLinePass([string]$line) { return ("$line" -cmatch '\sPASS(\s|$)') }
