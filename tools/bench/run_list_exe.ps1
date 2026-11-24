# S106 G2L: helpers for running an exe from out\exe\<folder>\ (lane_exe.ps1), dot-sourced by run_list.ps1.
#   Pure functions (no game, no lock, no git; Resolve-RunListTipExe only starts the runner it is given): unit-tested by tools\bench\run_list_exe_selftest.ps1.
# A run is judged against the commit it was QUEUED for, never HEAD (S105: a run queued for commit A was refused after HEAD moved to B).
# Any folder name directly under out\exe\ is accepted: the commit comes from the folder's stamp / exe_info.json, not from its name.

function Get-RunListExeKind([string]$ExePath, [string]$Repo) {
    # 'exe' = under <Repo>\out\exe\, 'other' = anything else (out\build, bisect, ...).
    try { $full = [IO.Path]::GetFullPath($ExePath) } catch { return 'other' }
    $repoFull = [IO.Path]::GetFullPath($Repo).TrimEnd('\') + '\'
    if ($full.StartsWith($repoFull + 'out\exe\', [StringComparison]::OrdinalIgnoreCase)) { return 'exe' }
    return 'other'
}

function Get-RunListWantArgs([string]$Kind, [string]$ForCommit, [switch]$Tip) {
    # The extra exe_vs_head.py arguments: an out\exe exe is judged against -ForCommit, else against the commit its own exe_info.json names. Others: none (HEAD).
    # S106 G2LF F6: a -ForCommit is passed whatever the kind (an out\build exe is judged against it too, never silently against HEAD).
    # S109 WARM: the tip exe (-Exe tip) is judged against HEAD by content (no --want): a tools / docs commit since its build never stales it.
    if ($Tip) { return @() }
    if ($ForCommit) { return @('--want', $ForCommit) }
    if ($Kind -ne 'exe') { return @() }
    return @('--want-info')
}

function Test-RunListBisectExe([string]$ExePath) {
    # S106 G2LF: the bisect exemption (bisect exes are old on purpose) covers the bisect worktrees only, not any folder that happens to be named bisect (e.g. out\exe\bisect).
    return [bool]($ExePath -match '\\(out\\harness|thirdparty\\sdk-wt)\\bisect\\')
}

function Get-RunListGuardVerdict([string]$Kind, [int]$Code, [bool]$AllowStale) {
    # -> 'ok' | 'warn' | 'allowed' | 'abort'. Code: exe_vs_head's exit (0 fresh, 3 stale / wrong exe, 4 cannot tell).
    if ($Code -eq 0) { return 'ok' }
    if ($Kind -eq 'exe' -and $Code -eq 3) { return 'abort' }   # S106 G2LF F5: an out\exe exe of the WRONG commit never runs: -AllowStaleExe / WWE_ALLOW_STALE_EXE do not apply
    if ($Code -eq 3) { if ($AllowStale) { return 'allowed' } else { return 'abort' } }
    if ($Kind -eq 'exe') { return 'abort' }   # an out\exe exe whose commit cannot be told (no stamp / no info) never runs: it is judged against a named commit
    return 'warn'
}

function Add-RunListExeFlags([string]$Flags, [string]$Kind, [string]$Repo, [string]$Given) {
    # An out\exe exe has no ..\..\replays beside it: give it the repo's own (the flag is not added when the caller or the script names one).
    if ($Kind -ne 'exe') { return $Flags }
    if ("$Given" -match '(^|\s)--wwe2k14_replay_dir=') { return $Flags }
    return "$Flags --wwe2k14_replay_dir=$Repo\out\replays"
}

function Add-RunListSettingsFlag([string]$Flags, [string]$Repo, [string]$Work, [string]$Given) {
    # S109 WARM2 (WARM review R1): EVERY run (any exe, any route) gets its own launcher settings file under out\ unless the caller or the route's header names one, so no
    # run can read or write %APPDATA%\wwe2k14-launcher\settings.json (a stream-keeping exe reads it at every start, the GRAPHICS menu reads and writes it in any build).
    # Beside the user-data mirror, not inside it: the mirror is robocopy /MIR'd from out\bench\<Data> at every run and would delete the file.
    if ("$Given $Flags" -match '(^|\s)--wwe2k14_settings_file=') { return $Flags }
    return "$Flags --wwe2k14_settings_file=$Repo\out\bench\$Work.settings.json"
}

function Test-RunListTipName([string]$Exe) { return ($Exe -ceq 'tip' -or $Exe -ceq 'tip-stream') }

function Resolve-RunListTipExe([string]$Name, [string]$Runner, [string]$ForCommit) {
    # S109 WARM: -Exe tip | tip-stream -> the current exe of the main checkout's HEAD, from tools\exe\tip_exe.ps1 in the foreground (it answers at once when the
    # warm build already ran, waits for a running one, or builds). -> @{ Exe; Abort; Lines }. Nothing else is needed on the command line: an out\exe exe finds
    # the game files and out\scenarios from its folder, run_list adds the replay dir, the mute and the speed (docs\research\s109\WARM_plan.md (c)).
    if ($ForCommit) { return @{ Exe = ''; Abort = "-Exe $Name is judged against HEAD by content; -ForCommit names another judge: give one of them"; Lines = @() } }
    $a = @('-NoProfile', '-File', $Runner)
    if ($Name -ceq 'tip-stream') { $a += @('-Preset', 'win-amd64-release') }
    $lines = @(& pwsh @a 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE
    $m = $lines | Where-Object { $_ -cmatch '^TIP_EXE (.+)$' } | Select-Object -Last 1
    if ($code -eq 0 -and $m -and ($m -cmatch '^TIP_EXE (.+)$')) {
        return @{ Exe = $Matches[1].Trim(); Abort = ''; Lines = @($lines | Where-Object { $_ -cnotmatch '^TIP_EXE ' }) }
    }
    $why = ($lines | Where-Object { "$_".Trim() } | Select-Object -Last 1)
    return @{ Exe = ''; Abort = "-Exe ${Name}: no current tip exe (tip_exe.ps1 exit $code): $why"; Lines = $lines }
}
