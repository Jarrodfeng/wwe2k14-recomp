# S106 0a selftest for run_fail_quit.ps1: the S105 fault route (s105_chk_lightsout: on_fail continue, idled to the kill) must GET the flag;
# a named flag, -NoKill and the s48 opt-out route must not; a different flag name (planted) must still get it.
. "$PSScriptRoot\run_fail_quit.ps1"
$R = (Resolve-Path "$PSScriptRoot\..\..").Path
$script:bad = 0
function Case($name, $got, $want) {
    if ($got -ne $want) { Write-Host "FAIL $name : got '$got' want '$want'"; $script:bad++ } else { Write-Host "ok   $name" }
}
function HeaderFlags($rel) {
    $m = Get-Content -LiteralPath (Join-Path $R $rel) -TotalCount 40 | Select-String -Pattern '^#\s*harness:\s*flags\s+(.*)$' | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value } else { return '' }
}
$on = ' --wwe2k14_input_fail_quit=true'
Case 'plain flags get it' (Get-RunFailQuitFlag '--audio_mute=true' $false) $on
Case 'empty flags get it' (Get-RunFailQuitFlag ' ' $false) $on
Case 'named =false: nothing' (Get-RunFailQuitFlag '--wwe2k14_input_fail_quit=false' $false) ''
Case 'named =true: nothing (no duplicate)' (Get-RunFailQuitFlag 'x --wwe2k14_input_fail_quit=true' $false) ''
Case '-NoKill: nothing' (Get-RunFailQuitFlag '--audio_mute=true' $true) ''
Case 'bare flag: nothing (not doubled)' (Get-RunFailQuitFlag 'x --wwe2k14_input_fail_quit' $false) ''
Case 'bare flag at the end of a line: nothing' (Get-RunFailQuitFlag '--audio_mute=true --wwe2k14_input_fail_quit ' $false) ''
Case '--no- spelling: nothing (not doubled)' (Get-RunFailQuitFlag '--no-wwe2k14_input_fail_quit' $false) ''
Case 'live session (live.py flag string): nothing' (Get-RunFailQuitFlag '--audio_mute=true --wwe2k14_live=menu' $false) ''
Case 'planted: a different live flag name still gets it' (Get-RunFailQuitFlag '--wwe2k14_livex=1' $false) $on
foreach ($rt in 'launch_boot.txt', 'launch_boot_watch.txt') {
    Case "fixture: $rt opts out by header" (HeaderFlags "tools\input_scripts\$rt") '--wwe2k14_input_fail_quit=false'
}
Case 'planted: a different flag name still gets it' (Get-RunFailQuitFlag '--wwe2k14_input_fail_quit_x=1' $false) $on
$lo = HeaderFlags 'tools\input_scripts\scenes\s105_chk_lightsout.txt'
Case 'fixture: s105_chk_lightsout (S105 fault route) gets it' (Get-RunFailQuitFlag $lo $false) $on
$n5 = HeaderFlags 'tools\input_scripts\short\s48_n5c_nogpu.txt'
Case 'fixture: s48_n5c_nogpu (opt-out header) does not' (Get-RunFailQuitFlag $n5 $false) ''
$rl = Get-Content -LiteralPath "$R\tools\bench\run_list.ps1" -Raw
Case 'run_list dot-sources it' ($rl -match 'run_fail_quit\.ps1') $true
Case 'run_list calls it' ($rl -match 'Get-RunFailQuitFlag') $true
if ($script:bad) { Write-Host "run_fail_quit selftest: $script:bad FAIL"; exit 1 }
Write-Host 'run_fail_quit selftest: PASS'
