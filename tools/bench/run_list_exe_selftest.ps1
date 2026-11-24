# S106 G2L: selftest of tools\bench\run_list_exe.ps1 (pure functions; no game, no lock).   pwsh -NoProfile -File tools\bench\run_list_exe_selftest.ps1
# entry: Get-RunListGuardVerdict
. "$PSScriptRoot\run_list_exe.ps1"
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$fail = 0
function T([string]$name, $got, $want) {
    $ok = ("$got" -eq "$want")
    if (-not $ok) { $script:fail++ }
    ('{0} {1}  (got "{2}", want "{3}")' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $got, $want)
}
T 'kind: out\exe\<any name>' (Get-RunListExeKind "$R\out\exe\lane-g2l-abcdef123\wwe2k14.exe" $R) 'exe'
T 'kind: out\exe\<c9>' (Get-RunListExeKind "$R\out\exe\abcdef123\wwe2k14.exe" $R) 'exe'
T 'kind: out\build is other' (Get-RunListExeKind "$R\out\build\win-amd64-release\wwe2k14.exe" $R) 'other'
T 'kind: a sibling named out\exefoo is other' (Get-RunListExeKind "$R\out\exefoo\wwe2k14.exe" $R) 'other'
T 'kind: dotdot out of out\exe is other' (Get-RunListExeKind "$R\out\exe\..\build\wwe2k14.exe" $R) 'other'
T 'want args: exe + ForCommit' ((Get-RunListWantArgs 'exe' 'abc1234') -join ' ') '--want abc1234'
T 'want args: exe, no ForCommit -> exe_info' ((Get-RunListWantArgs 'exe' '') -join ' ') '--want-info'
T 'want args: out\build, no ForCommit = none (HEAD)' ((Get-RunListWantArgs 'other' '') -join ' ') ''
T 'UNSAFE F6: -ForCommit with an out\build exe passes --want (never silently ignored)' ((Get-RunListWantArgs 'other' 'abc1234') -join ' ') '--want abc1234'
# verdicts: the UNSAFE direction first (can a wrong exe say ok?)
T 'UNSAFE: exe folder, wrong commit (3) aborts' (Get-RunListGuardVerdict 'exe' 3 $false) 'abort'
T 'UNSAFE: exe folder, cannot tell (4) aborts, not a warning' (Get-RunListGuardVerdict 'exe' 4 $false) 'abort'
T 'UNSAFE F5: exe folder, wrong commit (3) with -AllowStaleExe still aborts' (Get-RunListGuardVerdict 'exe' 3 $true) 'abort'
T 'F6: out\build with -ForCommit, wrong commit (3): aborts like any code 3 on out\build (override only via -AllowStaleExe)' (Get-RunListGuardVerdict 'other' 3 $false) 'abort'
T 'exe folder fresh (0)' (Get-RunListGuardVerdict 'exe' 0 $false) 'ok'
T 'stale (3) with -AllowStaleExe is allowed' (Get-RunListGuardVerdict 'other' 3 $true) 'allowed'
T 'out\build cannot tell (4) still only warns (unchanged)' (Get-RunListGuardVerdict 'other' 4 $false) 'warn'
$g = '--audio_mute=true'
T 'flags: exe gets the repo replay dir' (Add-RunListExeFlags $g 'exe' $R '') "$g --wwe2k14_replay_dir=$R\out\replays"
T 'flags: caller-named replay dir kept' (Add-RunListExeFlags $g 'exe' $R '--x --wwe2k14_replay_dir=D:\r') $g
T 'flags: out\build untouched' (Add-RunListExeFlags $g 'other' $R '') $g
# S109 WARM2 (WARM review R1): every run has a settings file under out\
$sf = Add-RunListSettingsFlag $g $R 'slot1' ''
T 'UNSAFE R1: a run with no settings flag gets one under out\ (never the player''s %APPDATA% file), beside the mirror' $sf "$g --wwe2k14_settings_file=$R\out\bench\slot1.settings.json"
T 'settings flag: given by the caller / route header, kept, none added' (Add-RunListSettingsFlag $g $R 'slot1' '--x --wwe2k14_settings_file=D:\s.json') $g
T 'settings flag: an out\build exe gets one too (not only out\exe)' (Add-RunListSettingsFlag (Add-RunListExeFlags $g 'other' $R '') $R 'slot2' '') "$g --wwe2k14_settings_file=$R\out\bench\slot2.settings.json"
T 'settings flag: the run_list.ps1 flags line is built through it (the function is not dead)' ((Select-String -LiteralPath "$PSScriptRoot\run_list.ps1" -Pattern 'Add-RunListSettingsFlag \$flags \$R \$Work' -Quiet)) 'True'
T 'bisect exemption: the bisect worktree path' (Test-RunListBisectExe "$R\out\harness\bisect\wt\ours\out\build\wwe2k14.exe") 'True'
T 'bisect exemption: sdk-wt bisect' (Test-RunListBisectExe "$R\thirdparty\sdk-wt\bisect\x.exe") 'True'
T 'UNSAFE: an out\exe folder NAMED bisect is not exempt' (Test-RunListBisectExe "$R\out\exe\bisect\wwe2k14.exe") 'False'
# S109 WARM: -Exe tip
T 'want args: the tip exe is judged against HEAD by content (no --want, no --want-info)' ((Get-RunListWantArgs 'exe' '' -Tip) -join ' ') ''
T 'tip names: tip and tip-stream only, exact case' ("$(Test-RunListTipName 'tip') $(Test-RunListTipName 'tip-stream') $(Test-RunListTipName 'TIP') $(Test-RunListTipName "$R\out\exe\tip-abc\wwe2k14.exe")") 'True True False False'
$sd = Join-Path ([IO.Path]::GetTempPath()) "rle_tip_$PID"; [void][IO.Directory]::CreateDirectory($sd)
$stub = Join-Path $sd 'stub_tip.ps1'
Set-Content -LiteralPath $stub -Value "param([string]`$Preset = 'win-amd64-release-off')`nWrite-Host 'tip_exe: current'`nif (`$env:STUB_TIP -eq 'gave') { 'tip_exe: REFUSED: not admitted: [admit] REFUSED after 180 s'; exit 75 }`n`"TIP_EXE C:\x\out\exe\`$(if (`$Preset -eq 'win-amd64-release') { 'tipstream' } else { 'tip' })-abc\wwe2k14.exe`"`nexit 0"
$r = Resolve-RunListTipExe 'tip' $stub ''
T 'resolve tip: the runner''s TIP_EXE path, its other lines kept for printing' "$($r.Exe)|$($r.Abort)|$($r.Lines -join ';')" 'C:\x\out\exe\tip-abc\wwe2k14.exe||tip_exe: current'
$r = Resolve-RunListTipExe 'tip-stream' $stub ''
T 'resolve tip-stream: asks the runner for the stream-keeping preset' $r.Exe 'C:\x\out\exe\tipstream-abc\wwe2k14.exe'
$env:STUB_TIP = 'gave'; $r = Resolve-RunListTipExe 'tip' $stub ''; Remove-Item env:STUB_TIP
T 'UNSAFE: the runner refused: no exe, an ABORT naming its exit and last line' "$($r.Exe)|$($r.Abort)" '|-Exe tip: no current tip exe (tip_exe.ps1 exit 75): tip_exe: REFUSED: not admitted: [admit] REFUSED after 180 s'
$r = Resolve-RunListTipExe 'tip' $stub 'abc1234'
T 'UNSAFE: -Exe tip with -ForCommit is refused (two judges), the runner is not started' "$($r.Exe)|$([bool]$r.Abort)" '|True'
Remove-Item -LiteralPath $sd -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "run_list_exe_selftest: $fail FAIL"; exit 1 }
'run_list_exe_selftest: all ok'
exit 0
