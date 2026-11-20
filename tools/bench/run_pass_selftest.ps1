# S105 H selftest for run_pass.ps1: planted crash-exit and script-failed fixtures must not read PASS; the clean one must.
# Rows work at the line level like run_list.ps1 (exit column + failure column + PASS word) and also run the consumers' test (Test-RunLinePass).
. "$PSScriptRoot\run_pass.ps1"
$failRx = 'script FAILED|\]\[crash\]|\[rdr\]\[live\] WARN pace_game|device lost|Failed to await|run log number MISMATCH'
$script:bad = 0
$P = '[2026-11-19 22:40:53.409] [info] [core] [t1] [wwe2k14][input] line 40: smoke: PASS (frame 9)'
$FL = '[2026-11-19 22:40:53.409] [info] [core] [t1] [wwe2k14][fast_loader] at frame 9: passes=3'
$PL = '[2026-11-19 22:40:53.409] [info] [core] [t1] [rdr][post_levers] depth-convert passes still recorded 12'
function Verdict($lines) {
    $exit = Get-RunExitColumn $lines
    $fail = ($lines | Select-String $failRx | Select-Object -First 2 | ForEach-Object Line) -join ' | '
    $pass = Get-RunPassWord $lines $exit $fail
    "x.txt -> wwe2k14_1  $exit$pass  $fail"
}
function Case($name, $lines, $wantPass) {
    $line = Verdict $lines
    $got = Test-RunLinePass $line
    $word = $line -cmatch '\sPASS(\s|$)'
    if ($got -ne $wantPass -or $word -ne $wantPass) { Write-Host "FAIL $name : got $got want $wantPass :: $line"; $script:bad++ } else { Write-Host "ok   $name" }
}
$ok0 = 'EXITED after 40.4s, exit code 0 (0x00000000)'
$crash = 'EXITED after 40.4s, exit code -1073741819 (0xC0000005)'
Case 'clean' @($P, $ok0) $true
Case 'crash exit (planted)' @($P, $crash) $false
Case 'crash exit beside a post_levers "passes still" line (planted, F1)' @($P, $PL, $crash) $false
Case 'post_levers line must not feed the exit column (planted, F1)' @($P, $PL) $false
Case 'crash log line only (planted)' @($P, '[wwe2k14][crash] unhandled exception', $ok0) $false
Case 'script failed (planted)' @($P, 'script FAILED at line 21: cvar wwe2k14_rdr_live_picture was not set', $ok0) $false
Case 'no exit line (planted)' @($P) $false
Case 'killed (planted)' @($P, 'KILLED after 60s (-KillAfterWait)') $false
Case 'no pass line' @($ok0) $false
Case 'only the fast_loader passes= line: not a route PASS (F2)' @($FL, $ok0) $false
Case 'a lower-case "smoke: pass" line is not PASS (F2)' @('[wwe2k14][input] line 5: smoke: pass', $ok0) $false
# a real smoke log (release build, runs 043 / 044) with the route's new `log smoke: PASS` line appended passes
foreach ($n in '043', '044') {
    $f = "C:\Users\Jarrod\wwe2k14-recomp\out\release\build\logs\wwe2k14_$n.log"
    if (Test-Path -LiteralPath $f) {
        $real = @(Get-Content -LiteralPath $f)
        Case "real smoke log $n without the new PASS line is not PASS (the fast_loader line alone must not do it)" ($real + $ok0) $false
        Case "real smoke log $n + `log smoke: PASS` + a clean exit" ($real + $P + $ok0) $true
    } else { Write-Host "skip real smoke log $n (not on this PC)" }
}
if ($script:bad) { exit 1 }
Write-Host 'run_pass selftest OK'
