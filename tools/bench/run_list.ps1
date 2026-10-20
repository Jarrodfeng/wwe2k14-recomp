# Runs each input script in order on a fresh copy of the frozen user data (-Data userdata_frozen | userdata_coldcache),
# timeline on, extra flags as ONE string (-Extra); prints "script -> run log, exit, failures" per script. Session 6.
# Usage (in-process, so the -Extra string isn't split):
#   & C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('a.txt','b.txt') -Extra '--x=1 --y=2'
# Session 8: holds the run lock (tools\runlock.ps1) for the whole list, so no other run or build lands between its
# runs; a run still going after -WaitSeconds (720) is killed (-NoKill keeps the old behaviour). -Exe runs another
# build's exe. The failure column matches real failure lines only (it used to match 'vulkan_gpu_crash_diagnostics').
# Session 8 (task C): a failed run (failure column, no EXITED-with-exit-code-0 line, STILL RUNNING / KILLED / ABORT,
# no log) is triaged at once, still inside the lock so no relink lands first: py tools\triage\triage.py NNN --json.
# Its verdict goes on the end of the run's line as "  [triage] CLASS (confidence C), culprit X, saved PATH" (host
# offsets resolved against the build that ran). PATH = out\harness\triage\run_NNN.json for the default build, or
# out\harness\triage\<tag>_run_NNN.json for any other -Exe (tag = its folder relative to the repo, non-word runs '_',
# lower case: bisect snapshots, sdk_rebase scratch exes and the coverage build number their runs from 001 too).
# Not saved ("NOT saved (...)") when the verdict is in_progress or the run is still going (-NoKill), so a provisional
# verdict is never reused later. Clean runs skip triage; a triage error or timeout (300 s, longer than triage's own
# 120 s event-log limit) is printed in the same place and never fails the list. The part never contains '->'.
# -NoTriage turns it off.
# Session 8 (task 0): the lock is SHARED by default: up to 3 run_list / run_and_triage holders run games at once
# (ordinary untimed runs: selftests, routes, probes). -Exclusive = timed run (perf, fps, clips, A/B): no other game
# alongside. Inside a caller's hold (regress, bisect, ...) it re-enters that hold: exclusive stays exclusive. Each
# shared slot N gets its own user data copy, out\bench\userdata_work_sN (exclusive keeps out\bench\userdata_work),
# mirrored from -Data before each run; userdata_frozen is only read. Log numbers, frames/timeline CSVs and captures
# stay per run: run_and_triage serializes launches and identifies its own log (tools\runlock.ps1 header).
# Fixer: an -Exe that does not log "run log number" runs exclusive; a script whose slot's user data a game still runs
# on (-NoKill) is skipped with "ABORT: ..."; a log whose recorded number differs from its file name shows as a failure
# ("run log number MISMATCH"); a shared run's triage line says GPU-reset matching is unreliable for overlapping runs.
# Session 8 (task L1, parallel lists):
#   -Parallel N (1..3; default 1 = one at a time, exactly the behaviour above) runs the list N at a time, each in its
#   own SHARED slot: run_list starts N worker processes (this same script, -WorkerDir/-WorkerId, internal), and each
#   worker takes one shared slot (so its own out\bench\userdata_work_sN, mirrored before each run) and keeps it while
#   there is work: worker k starts with script k (reserved for it before any worker starts, so a worker never waits for
#   a slot with nothing to do and never takes another worker's first script), then takes the next script nobody has
#   taken yet (claim files created with CreateNew: each script runs once). Launches stay serialized by run_and_triage
#   (each run gets its own log number), failed runs are triaged by the worker inside its slot, exactly as above. When
#   all workers have ended, the result lines are printed in INPUT order, one per script, in the format above plus
#   "  [slot N]" (the shared slot that ran it) just before the "[triage]" part, which stays last:
#     selftest_title.txt -> wwe2k14_431  EXITED after 40.4s, exit code 0 (0x00000000)  [slot 2]
#   Progress ("[run_list] ...") is written with Write-Host while it runs; workers' own output is kept in
#   out\harness\run_list\par_<stamp>_<pid>\worker_K.log (with job.json and one line_I.txt per script). The workers are
#   tied to this process (Add-RunLockKillOnExit; not with -NoKill): if it is stopped, they and their games stop too.
#   -Parallel N > 1 with -Exclusive is an error (timed runs never run side by side). It runs one at a time, and says
#   so, when the exe cannot run shared (it does not log "run log number": exclusive, see above) or when the call runs
#   inside a caller's run-lock hold (regress, bisect, `runlock.ps1 run`: its games belong to that one hold).
#   -ScriptExtras @('', '--x=1', ...) (any -Parallel): flags for each script, same order and count as -Scripts ('' =
#   none), placed BEFORE -Extra on the game's command line (so -Extra wins a clash), e.g. the regress routes' own flags.
# Session 16 (step 0, Jarrod's rule): test runs are FAST-FORWARDED by default: every run gets --vsync=false (the game
# runs as fast as the PC allows). Normal speed (no --vsync flag added) when: -RealTime; -Exclusive (timed runs);
# the caller's flags already set --vsync=; or the script says so in its header (first 40 lines) with a line
#   # harness: realtime  (reason)
# (load-time routes, timed bench routes, audio checks, routes Jarrod watches). The flag goes BEFORE -ScriptExtras /
# -Extra on the command line. Each run's choice is printed: "[run_list] <script>: fast-forward (vsync off)" or
# "... normal speed (<why>)".
# Session 19 (Jarrod): -Step30 adds --wwe2k14_step_hz=30 to fast-forwarded runs (the game's own 30 fps mode as the
# step: a CPU match ran at 7.0x real time instead of 3.0x, run 1459; the Rumble check passed, run 1460). For untimed,
# event-driven routes (full matches, seed sweeps, the Rumble); frame-counted steps (screenshots at +N, menu presses)
# land at different game moments. A script can opt in with a header line (first 40 lines) "# harness: step30". Not
# added when the run is at normal speed (except an explicit --vsync=false in the flags) or the flags already set
# --wwe2k14_step_hz=. Printed per run: "[run_list] <script>: step 30 (...)".
# Session 30 (#10): test windows never steal the focus. Untimed runs stay fullscreen (borderless, as before) but are
# shown without activation and behind every other window (--window_activate_when_shown=false
# --window_show_behind=true; SDK cvars from Session 30; Jarrod: "full screen and still in the background"): the game
# keeps presenting and screenshots come from the guest frame, not the window. A `check` question brings the window to
# the top (still unfocused) and flashes its taskbar button (src/check_prompt.h). -Visible: fullscreen on top of the
# others, unfocused. -Windowed: a 1280x720 window instead (with -Parallel each slot at its own cascaded position).
# -Foreground or -Exclusive (timed runs: every baseline was measured that way; regress passes -Foreground for its
# perf routes): nothing added (the old fullscreen, activated window). A window flag the caller sets itself (--fullscreen=,
# --window_activate_when_shown=, --window_show_behind=, --window_position=) is not added again; the others still are.
# Printed per run: "[run_list] <script>: window ...".
param([string[]]$Scripts, [string]$Extra = '', [string]$Data = 'userdata_frozen', [int]$WaitSeconds = 720,
      [switch]$NoKill, [switch]$NoTriage,
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\wwe2k14.exe',
      [switch]$Exclusive,   # Session 8: -Exclusive = timed run (perf, fps, clips): no other game may run alongside.
      [int]$Parallel = 1,   # task L1: run the list N (1..3) at a time in shared slots; 1 = one at a time (as before)
      [string[]]$ScriptExtras = @(),   # task L1: per-script flags (same order as -Scripts), placed before -Extra
      [switch]$RealTime,    # Session 16: run at normal speed (vsync on); default = fast-forward, see below
      [switch]$Step30,      # Session 19: --wwe2k14_step_hz=30 on fast-forwarded runs, see above
      [switch]$Visible,     # Session 30: the window on top (unfocused) instead of behind the others
      [switch]$Windowed,    # Session 30: a 1280x720 window instead of fullscreen
      [switch]$Foreground,  # Session 30: the old fullscreen, activated window
      [string]$WorkerDir = '', [int]$WorkerId = 0)   # internal (task L1): a -Parallel worker started by run_list
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\runlock.ps1"

# Session 16: '' when script $Path should run fast-forwarded, else why it runs at normal speed. $Flags = the run's
# other flags (-ScriptExtras entry + -Extra). Relative paths are tried from here and from the exe folder.
function Get-RunListRealTimeReason([string]$Path, [string]$Flags) {
    if ($RealTime) { return '-RealTime' }
    if ($Exclusive) { return '-Exclusive (timed run)' }
    if ($Flags -match '(^|\s)--vsync=') { return 'the flags set --vsync' }
    $p = $Path
    if (-not (Test-Path -LiteralPath $p -PathType Leaf) -and -not [IO.Path]::IsPathRooted($p)) { $p = Join-Path (Split-Path -Parent $Exe) $Path }
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $m = Get-Content -LiteralPath $p -TotalCount 40 | Select-String -Pattern '^#\s*harness:\s*realtime\b(.*)$' | Select-Object -First 1
        if ($m) { return "script header: realtime$($m.Matches[0].Groups[1].Value)" }
    }
    return ''
}

# Session 30 (#10): the window flags for script $Path (see the header) -> @(flags, description).
function Get-RunListWindowFlags([string]$Path, [string]$Flags, [int]$Slot) {
    if ($Foreground) { return @('', 'fullscreen, activated (-Foreground)') }
    if ($Exclusive) { return @('', 'fullscreen, activated (-Exclusive: timed run)') }
    $want = [ordered]@{ window_activate_when_shown = 'false' }
    # Session 32 (Jarrod: unfocused windows ran slower): out of Windows' background throttling (src\background_speed.h;
    # older exes ignore the unknown flag).
    $want['wwe2k14_background_full_speed'] = 'true'
    if ($Windowed) {
        $want['fullscreen'] = 'false'
        if ($Slot -gt 0) { $want['window_position'] = "$(40 + 60 * ($Slot - 1)),$(60 + 60 * ($Slot - 1))" }
    }
    if (-not $Visible) { $want['window_show_behind'] = 'true' }
    $f = @(); $own = @()
    foreach ($k in $want.Keys) {
        if ($Flags -match "(^|\s)--$k=") { $own += $k } else { $f += "--$k=$($want[$k])" }
    }
    $desc = "$(if ($Windowed -or $Flags -match '(^|\s)--fullscreen=false\b') { 'windowed' } else { 'fullscreen' }), unfocused, $(if ($Visible) { 'on top (-Visible)' } else { 'behind other windows' })"
    if ($own.Count) { $desc += " (the flags set $($own -join ', ') themselves)" }
    return @(($f -join ' '), $desc)
}

# Where run NNN's triage verdict for the exe in $ExeDir is kept (tools\regress\regress.py triage_save_path: same rule).
function Get-TriageSavePath([int]$Num, [string]$ExeDir) {
    $full = [IO.Path]::GetFullPath($ExeDir).TrimEnd('\')
    $name = 'run_{0:D3}.json' -f $Num
    if ($full -ne "$R\out\build\win-amd64-release") {
        $tag = ([IO.Path]::GetRelativePath($R, $full) -replace '\W+', '_').Trim('_').ToLowerInvariant()
        $name = "${tag}_$name"
    }
    Join-Path $R "out\harness\triage\$name"
}

# triage.py --json for run NNN, saved to Get-TriageSavePath unless -NoSave or in_progress -> one line. Never throws.
function Get-RunTriageRaw([string]$RunNo, [string]$ExeDir, [switch]$NoSave) {
    try {
        $num = [int]$RunNo
        $psi = [System.Diagnostics.ProcessStartInfo]::new('py')
        foreach ($a in @("$R\tools\triage\triage.py", "$num", '--json', '--exe-dir', $ExeDir)) { $psi.ArgumentList.Add($a) }
        $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $psi.WorkingDirectory = $R
        $p = [System.Diagnostics.Process]::Start($psi)
        $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(300000)) { try { $p.Kill($true) } catch { }; return 'triage timed out after 300 s' }
        $p.WaitForExit()
        $txt = $so.Result
        $v = $null
        try { $v = $txt | ConvertFrom-Json -AsHashtable } catch { }
        if (-not ($v -is [System.Collections.IDictionary]) -or -not $v['class']) {
            $err = (($se.Result + "`n" + $txt) -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 1)
            return "triage error (exit $($p.ExitCode)): $("$err".Trim().Substring(0, [Math]::Min(160, "$err".Trim().Length)))"
        }
        $cls = "$($v['class'])"
        $culprit = if ($v['culprit']) { "$($v['culprit'])" } else { 'unknown' }
        $culprit = $culprit.Substring(0, [Math]::Min(120, $culprit.Length))
        $saved = Get-TriageSavePath $num $ExeDir
        $where = if ($NoSave) { 'NOT saved (run still going: -NoKill)' }
                 elseif ($cls -eq 'in_progress') { 'NOT saved (run still going; triage it again once it has ended)' }
                 else {
                     try {
                         New-Item -ItemType Directory -Force -Path (Split-Path $saved -Parent) | Out-Null
                         [IO.File]::WriteAllText($saved, $txt, [Text.UTF8Encoding]::new($false)); "saved $saved"
                     } catch { "NOT saved ($($_.Exception.Message))" }
                 }
        return "$cls (confidence $($v['confidence'])), culprit $culprit, $where"
    } catch { return "triage error: $($_.Exception.Message)" }
}
function Get-RunTriage([string]$RunNo, [string]$ExeDir, [switch]$NoSave) {
    "$(Get-RunTriageRaw $RunNo $ExeDir -NoSave:$NoSave)" -replace '->', '>'
}

# One script of the list, run inside the hold $Lock on user data out\bench\$Work -> its result line (one string).
# $ScriptExtra (task L1, -ScriptExtras) goes before -Extra; $Tag (task L1, "  [slot N]" of a -Parallel worker) goes
# before the [triage] part, which stays the last thing on the line. With both empty the line and the game's command
# line are exactly the pre-L1 ones.
function Invoke-RunListScript([string]$s, [string]$ScriptExtra, $Lock, [string]$Work, [string]$Tag = '') {
    # Fixer (review finding 6): never mirror over, or start a second game on, user data a game still runs on (a
    # -NoKill run that outlived -WaitSeconds; the slot's earlier game). Same outcome as the exclusive path's ABORT.
    $busy = @(Get-RunLockGamesOnUserData "$R\out\bench\$Work")
    if ($busy.Count) {
        return "$(Split-Path $s -Leaf) ->   ABORT: wwe2k14 pid $($busy -join ',') still runs on out\bench\$Work; not mirroring over it or starting a second game in this slot$Tag"
    }
    robocopy "$R\out\bench\$Data" "$R\out\bench\$Work" /MIR /NFL /NDL /NJH /NJS | Out-Null
    $flags = "--user_data_root=$R\out\bench\$Work --cache_root=$R\out\bench\$Work\cache --wwe2k14_timeline=true --wwe2k14_input_script=$s"
    $why = Get-RunListRealTimeReason $s "$ScriptExtra $Extra"
    if ($why) { Write-Host "[run_list] $(Split-Path $s -Leaf): normal speed ($why)" }
    else { $flags += ' --vsync=false'; Write-Host "[run_list] $(Split-Path $s -Leaf): fast-forward (vsync off)" }
    $win = Get-RunListWindowFlags $s "$ScriptExtra $Extra" $(if ($Lock.Mode -eq 'Shared') { [int]$Lock.Slot } else { 0 })
    if ($win[0]) { $flags += " $($win[0])" }
    Write-Host "[run_list] $(Split-Path $s -Leaf): window $($win[1])"
    if ($ScriptExtra) { $flags += " $ScriptExtra" }
    # Session 19: step 30 on fast-forwarded runs (-Step30 or the script header "# harness: step30").
    $allFlags = "$flags $Extra"
    $stepHeader = $false
    try {
        # Session 30: relative script paths are relative to the exe folder (as Get-RunListRealTimeReason tries them).
        $p30 = if ([IO.Path]::IsPathRooted($s)) { $s } elseif (Test-Path -LiteralPath (Join-Path $R $s) -PathType Leaf) { Join-Path $R $s } else { Join-Path (Split-Path -Parent $Exe) $s }
        $stepHeader = [bool](Get-Content -LiteralPath $p30 -TotalCount 40 -ErrorAction Stop | Select-String -Pattern '^#\s*harness:\s*step30\b' -Quiet)
    } catch { }
    if ($Step30 -or $stepHeader) {
        $fastForward = (-not $why) -or ($allFlags -match '(^|\s)--vsync=false\b')
        if ($allFlags -match '(^|\s)--wwe2k14_step_hz=') {
            Write-Host "[run_list] $(Split-Path $s -Leaf): step 30 not added (the flags set --wwe2k14_step_hz)"
        } elseif (-not $fastForward) {
            Write-Host "[run_list] $(Split-Path $s -Leaf): step 30 not added (normal speed: $why)"
        } else {
            $flags += ' --wwe2k14_step_hz=30'
            Write-Host "[run_list] $(Split-Path $s -Leaf): step 30 ($(if ($Step30) { '-Step30' } else { 'script header' }))"
        }
    }
    $out = & "$R\tools\run_and_triage.ps1" -WaitSeconds $WaitSeconds -KillAfterWait:(-not $NoKill) -Exe $Exe -Exclusive:($Lock.Mode -eq 'Exclusive') -ExtraArgs "$flags $Extra" 2>&1
    $log = ($out | Select-String '=== log: (wwe2k14_\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ','
    $exit = (($out | Select-String 'EXITED|STILL|KILLED|ABORT') | ForEach-Object Line) -join ' '
    $fail = ($out | Select-String 'script FAILED|\]\[crash\]|device lost|Failed to await|run log number MISMATCH' | Select-Object -First 2 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(160, $_.Line.Length)) }) -join ' | '
    $pass = if ($out | Select-String ': PASS') { ' PASS' } else { '' }
    $tri = ''
    if (-not $NoTriage) {
        # case-sensitive: $exit also collects game log lines that merely contain "still" / "aborted"
        $bad = $fail -or -not $log -or ($exit -cnotmatch 'EXITED after [\d.,]+s, exit code 0\b') -or ($exit -cmatch 'STILL RUNNING after|KILLED after|ABORT:')
        if ($bad) {
            $going = ($exit -cmatch 'STILL RUNNING after') -and ($exit -cnotmatch 'KILLED after')  # -NoKill
            $tri = if ($log -match '^wwe2k14_(\d+)$') { '  [triage] ' + (Get-RunTriage $Matches[1] (Split-Path $Exe -Parent) -NoSave:$going) }
                   elseif ($log) { '  [triage] not run: several logs for one run (' + $log + ')' }
                   else { '  [triage] not run: no log' }
            # Fixer (review finding 7): triage.py bounds a GPU reset's window by the neighbouring logs' times, which
            # overlap in shared mode (the 2nd/3rd run of a batch lose most of their window): say so on the line.
            if ($Lock.Mode -eq 'Shared' -and $log) { $tri += ' (shared run: GPU-reset/event matching unreliable; rerun -Exclusive to confirm)' }
        }
    }
    $line = "$(Split-Path $s -Leaf) -> $log  $exit$pass  $fail"
    if ($Tag) { $line = $line.TrimEnd() + $Tag }
    # Session 29: what the script checked + a contact sheet of its screenshots (tools\harness\run_verdict.py), shown
    # under the result line with Write-Host by the callers below: scripts that parse run_list's output never see it.
    $script:RunListChecks = ''
    if ($log -match '^wwe2k14_(\d+)$') {
        try {
            $v = & py "$R\tools\harness\run_verdict.py" $Matches[1] --exe-dir (Split-Path $Exe -Parent) 2>&1 | Select-Object -Last 1
            if ("$v".Trim()) { $script:RunListChecks = '    ' + ("$v".Trim() -replace '->', '>') }
        } catch { }
    }
    return "$line$tri"
}

# 'exclusive' / 'shared' when this process runs inside a live holder's run-lock hold (the holder's token is in the
# environment, as runlock.ps1's re-entry sees it); '' otherwise.
function Get-RunListHold {
    $tok = $env:WWE2K14_RUNLOCK_TOKEN
    if (-not $tok) { return '' }
    $cur = Read-RunLockJson $script:RunLockFile
    if ($cur -and $cur.token -eq $tok -and (Test-RunLockOwnerAlive $cur)) { return 'exclusive' }
    foreach ($sl in (Get-RunLockSlots)) {
        if ($sl.Rec -and $sl.Rec.token -eq $tok -and (Test-RunLockOwnerAlive $sl.Rec)) { return 'shared' }
    }
    return ''
}

function Write-RunListFile([string]$Path, [string]$Text) {
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllText($tmp, $Text, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($tmp, $Path, $true)
}

# Takes script claim file $Path: created with CreateNew, so it fails when the file exists and two workers (or the
# parent's reservations and a worker) never take the same script. -> $true when this call created it.
function New-RunListClaim([string]$Path, [string]$Text) {
    try { $fs = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None) }
    catch { return $false }
    try { $b = [Text.Encoding]::UTF8.GetBytes($Text); $fs.Write($b, 0, $b.Length) } catch { } finally { $fs.Dispose() }
    return $true
}

# ---- task L1: a -Parallel worker (started by the parent below; never called by hand) ----
if ($WorkerId -gt 0) {
    $job = [IO.File]::ReadAllText((Join-Path $WorkerDir 'job.json')) | ConvertFrom-Json
    $Extra = "$($job.extra)"; $Data = "$($job.data)"; $WaitSeconds = [int]$job.wait; $Exe = "$($job.exe)"
    $NoKill = [switch][bool]$job.nokill; $NoTriage = [switch][bool]$job.notriage
    $RealTime = [switch][bool]$job.realtime
    $Step30 = [switch][bool]$job.step30
    $Visible = [switch][bool]$job.visible; $Foreground = [switch][bool]$job.foreground; $Windowed = [switch][bool]$job.windowed
    $items = @($job.items)
    $first = $WorkerId - 1   # the parent reserved this item for this worker (claim_<first>.txt) before starting it
    $names = ($items | ForEach-Object { Split-Path "$($_.script)" -Leaf }) -join ','
    $lockName = "run_list -Parallel worker $WorkerId of $($job.n) (parent pid $($job.parent)): $names $Extra"
    $lockName = $lockName.Substring(0, [Math]::Min(200, $lockName.Length))
    try { $lock = Enter-RunLock -Name $lockName -Mode Shared }
    catch {
        Write-RunListFile (Join-Path $WorkerDir "line_$first.txt") "$(Split-Path "$($items[$first].script)" -Leaf) ->   ABORT: $($_.Exception.Message)"
        exit 1
    }
    try {
        if ($lock.Reentrant -or $lock.Mode -ne 'Shared') {
            # Cannot happen (the parent starts workers without a run-lock token), but never run side by side in a hold.
            Write-RunListFile (Join-Path $WorkerDir "line_$first.txt") "$(Split-Path "$($items[$first].script)" -Leaf) ->   ABORT: worker $WorkerId re-entered a $($lock.Mode) hold instead of taking its own shared slot"
            exit 1
        }
        $work = "userdata_work_s$($lock.Slot)"
        Write-RunListFile (Join-Path $WorkerDir "worker_$WorkerId.slot") "$($lock.Slot)"
        Write-Host "[run_list] worker $WorkerId of $($job.n): run lock shared slot $($lock.Slot), user data out\bench\$work"
        $i = $first
        while ($i -ge 0) {
            $it = $items[$i]
            Write-Host "[run_list] worker ${WorkerId}: script $($i + 1) of $($items.Count): $($it.script) $($it.extra)"
            $line = @(Invoke-RunListScript "$($it.script)" "$($it.extra)" $lock $work "  [slot $($lock.Slot)]")
            # Session 29: the checks line first (the parent prints a script's result when its line file appears).
            if ($script:RunListChecks) { Write-RunListFile (Join-Path $WorkerDir "checks_$i.txt") $script:RunListChecks }
            Write-RunListFile (Join-Path $WorkerDir "line_$i.txt") "$($line[-1])"
            Write-Host "[run_list] worker ${WorkerId}: $($line[-1])"
            # Next: the first script nobody has taken yet (claim_<j>.txt created atomically: CreateNew).
            $i = -1
            for ($j = 0; $j -lt $items.Count; $j++) {
                $claim = Join-Path $WorkerDir "claim_$j.txt"
                if (Test-Path -LiteralPath $claim) { continue }
                if (New-RunListClaim $claim "worker $WorkerId") { $i = $j; break }
            }
        }
    } finally { Exit-RunLock $lock }
    exit 0
}

# ---- task L1: the -Parallel parent ----
function Invoke-RunListParallel {
    $all = @($Scripts)
    $n = [Math]::Min($Parallel, $all.Count)
    $dir = Join-Path $R ("out\harness\run_list\par_{0}_{1}" -f (Get-Date).ToString('yyyyMMdd_HHmmss_fff'), $PID)
    [void][IO.Directory]::CreateDirectory($dir)
    $items = @(for ($i = 0; $i -lt $all.Count; $i++) {
        [ordered]@{ i = $i; script = "$($all[$i])"; extra = $(if ($i -lt @($ScriptExtras).Count) { "$($ScriptExtras[$i])" } else { '' }) } })
    $job = [ordered]@{ n = $n; extra = $Extra; data = $Data; wait = $WaitSeconds; nokill = [bool]$NoKill;
        notriage = [bool]$NoTriage; exe = $Exe; parent = $PID; items = $items; realtime = [bool]$RealTime; step30 = [bool]$Step30;
        visible = [bool]$Visible; foreground = [bool]$Foreground; windowed = [bool]$Windowed }
    Write-RunListFile (Join-Path $dir 'job.json') ($job | ConvertTo-Json -Depth 5)
    Write-Host "[run_list] -Parallel ${n}: $($all.Count) scripts, up to $n at once, each worker in its own shared run-lock slot (work folder $dir)"
    $t0 = Get-Date
    # Fixer (review finding 3): every worker's first script is reserved BEFORE any worker starts (CreateNew, as the
    # workers' own claims), so a worker that finishes its first script quickly can never take a script reserved for
    # a worker not started yet (which would run it a second time in another slot).
    for ($k = 1; $k -le $n; $k++) {
        if (-not (New-RunListClaim (Join-Path $dir "claim_$($k - 1).txt") "worker $k (reserved at start)")) {
            "ABORT: could not reserve script $k in $dir (claim_$($k - 1).txt exists already); nothing started"; return
        }
    }
    $workers = @()
    for ($k = 1; $k -le $n; $k++) {
        $psi = [System.Diagnostics.ProcessStartInfo]::new('pwsh')
        foreach ($a in @('-NoProfile', '-File', "$R\tools\bench\run_list.ps1", '-WorkerDir', $dir, '-WorkerId', "$k")) { $psi.ArgumentList.Add($a) }
        $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $psi.WorkingDirectory = $R
        [void]$psi.Environment.Remove('WWE2K14_RUNLOCK_TOKEN')   # each worker queues for its own slot
        $p = [System.Diagnostics.Process]::Start($psi)
        # This process stopped -> its workers (and the games they started, which are in the same job) stop. Not with
        # -NoKill: a game that outlives -WaitSeconds is meant to keep running after the list (the job would end it).
        if (-not $NoKill) { $null = Add-RunLockKillOnExit $p }
        $workers += [pscustomobject]@{ K = $k; P = $p; Out = $p.StandardOutput.ReadToEndAsync(); Err = $p.StandardError.ReadToEndAsync(); Said = $false }
    }
    $done = @{}
    while ($true) {
        $alive = @($workers | Where-Object { -not $_.P.HasExited })
        foreach ($w in $workers) {
            $sf = Join-Path $dir "worker_$($w.K).slot"
            if (-not $w.Said -and (Test-Path -LiteralPath $sf)) {
                Write-Host ("[run_list] worker {0} took shared slot {1} after {2:N0} s" -f $w.K, ([IO.File]::ReadAllText($sf)), ((Get-Date) - $t0).TotalSeconds)
                $w.Said = $true
            }
        }
        for ($i = 0; $i -lt $all.Count; $i++) {
            $lf = Join-Path $dir "line_$i.txt"
            if (-not $done[$i] -and (Test-Path -LiteralPath $lf)) {
                $done[$i] = $true
                Write-Host ("[run_list] {0} of {1} done after {2:N0} s: {3}" -f $done.Count, $all.Count, ((Get-Date) - $t0).TotalSeconds, ([IO.File]::ReadAllText($lf) -replace '->', '>'))
            }
        }
        if (-not $alive.Count) { break }
        Start-Sleep -Milliseconds 1000
    }
    foreach ($w in $workers) {
        $w.P.WaitForExit()
        $txt = $w.Out.Result
        if ($w.Err.Result) { $txt += "`n--- stderr ---`n" + $w.Err.Result }
        try { Write-RunListFile (Join-Path $dir "worker_$($w.K).log") "$txt" } catch { }
    }
    Write-Host ("[run_list] -Parallel {0}: {1} scripts in {2:N1} s wall; result lines in input order:" -f $n, $all.Count, ((Get-Date) - $t0).TotalSeconds)
    for ($i = 0; $i -lt $all.Count; $i++) {
        $lf = Join-Path $dir "line_$i.txt"
        $claim = Join-Path $dir "claim_$i.txt"
        if (Test-Path -LiteralPath $lf) {
            ([IO.File]::ReadAllText($lf)).TrimEnd("`r", "`n")
            $cf = Join-Path $dir "checks_$i.txt"   # Session 29 (Write-Host: not part of the output callers parse)
            if (Test-Path -LiteralPath $cf) { Write-Host ([IO.File]::ReadAllText($cf)).TrimEnd("`r", "`n") }
        }
        elseif (Test-Path -LiteralPath $claim) {
            $who = ([IO.File]::ReadAllText($claim) -replace ' \(.*$', '').Trim()
            "$(Split-Path $all[$i] -Leaf) ->   ABORT: $who ended without a result line for it (see $dir\$($who -replace '^worker ', 'worker_').log)"
        } else { "$(Split-Path $all[$i] -Leaf) ->   ABORT: not run (every worker ended before reaching it; see $dir)" }
    }
}

# ---- the list ----
if ($Parallel -lt 1 -or $Parallel -gt $script:RunLockMaxShared) { "ABORT: -Parallel must be 1..$($script:RunLockMaxShared) (the shared slots), got $Parallel"; exit 2 }
if ($Parallel -gt 1 -and $Exclusive) { "ABORT: -Parallel $Parallel runs games side by side in shared slots; -Exclusive (timed runs) never runs next to another game: use one or the other"; exit 2 }
if (@($ScriptExtras).Count -and @($ScriptExtras).Count -ne @($Scripts).Count) { "ABORT: -ScriptExtras has $(@($ScriptExtras).Count) entries for $(@($Scripts).Count) scripts (one per script, '' for none)"; exit 2 }
# Session 14 (run 1019): a list passed through `pwsh -File` arrives as ONE string ('a.txt','b.txt'); the game then got a
# script path that does not exist, no launch flag, and played the attract demo for 400 s. Refuse missing scripts.
# Relative paths are tried from the current folder and from the exe folder (the game resolves them from there; regress
# passes ..\..\..\tools\...).
function Resolve-RunListScript([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return $Path }
    if ([IO.Path]::IsPathRooted($Path)) { return $null }
    $fromExe = Join-Path (Split-Path -Parent $Exe) $Path
    if (Test-Path -LiteralPath $fromExe -PathType Leaf) { return $fromExe }
    return $null
}
foreach ($s in @($Scripts)) {
    if (-not (Resolve-RunListScript "$s")) {
        "ABORT: input script not found: $s (several scripts through pwsh -File arrive as one string: call run_list.ps1 with & from PowerShell, or one script per call)"; exit 2
    }
}
# Session 37 (runs 2588 / 2589): 'tools\input_scripts\short\X.txt' passed the check above from the repo folder, but the
# game opens a relative path from the exe folder ("can't open script") and played the attract demo. Hand the game the
# absolute path of the file the check found.
$Scripts = @(foreach ($s in @($Scripts)) { (Resolve-Path -LiteralPath (Resolve-RunListScript "$s")).Path })
# Session 14 (load_times runs 1024-1027 without a launch flag sat at the main menu): a route whose header says
# "Use with --wwe2k14_launch" needs a --wwe2k14_launch= flag in -Extra or its -ScriptExtras entry.
for ($i = 0; $i -lt @($Scripts).Count; $i++) {
    $head = (Get-Content -LiteralPath (Resolve-RunListScript "$(@($Scripts)[$i])") -TotalCount 16) -join "`n"
    $flags = "$Extra $(if ($i -lt @($ScriptExtras).Count) { @($ScriptExtras)[$i] })"
    if ($head -match 'Use with --wwe2k14_launch' -and $flags -notmatch '--wwe2k14_launch=') {
        "ABORT: $(Split-Path -Leaf "$(@($Scripts)[$i])") says 'Use with --wwe2k14_launch' but no --wwe2k14_launch= flag was given"; exit 2
    }
    # Session 15 (runs 1093 / 1094): a flag with an empty value ('--x=') made the game start without its input script
    # (title screen, then the attract demo). Refuse it; give the setting a value that means "none" instead.
    if ($flags -match '(^|\s)(--[\w.]+=)(\s|$)') {
        "ABORT: empty flag value '$($Matches[2])' (the game then loses its other flags, e.g. the input script): give it a value"; exit 2
    }
}

# Fixer (review finding 5): an exe built before the per-run log number (no "run log number" string, e.g. the
# coverage build of 10:04 or an old bisect snapshot) rescans for the newest log whenever it names a capture, CSV or
# coverage folder, so it must not run next to other games: take the lock exclusively for it.
$listMode = if ($Exclusive) { 'Exclusive' } else { 'Shared' }
if (-not $Exclusive -and -not (Test-RunLockExeSharedSafe $Exe)) {
    Write-Host "[run_list] $Exe does not log 'run log number' (built before the per-run log number): running EXCLUSIVE, not shared"
    $listMode = 'Exclusive'
}
if ($Parallel -gt 1) {
    $held = Get-RunListHold
    $why = if ($listMode -eq 'Exclusive') { "this exe cannot run shared (see above)" }
           elseif ($held) { "this call runs inside a caller's $held run-lock hold (regress, bisect, runlock.ps1 run ...), which its games share" }
           else { $null }
    if ($why) { Write-Host "[run_list] -Parallel ${Parallel}: running the scripts ONE AT A TIME instead: $why" }
    elseif (-not @($Scripts).Count) { Write-Host "[run_list] -Parallel ${Parallel}: no scripts"; return }
    else { Invoke-RunListParallel; return }
}
try { $lock = Enter-RunLock -Name "run_list $(($Scripts | ForEach-Object { Split-Path $_ -Leaf }) -join ',') $Extra" -Mode $listMode }
catch { "ABORT: $($_.Exception.Message)"; exit 1 }
try {
$work = if ($lock.Mode -eq 'Shared') { "userdata_work_s$($lock.Slot)" } else { 'userdata_work' }
Write-Host "[run_list] run lock $($lock.Mode.ToLowerInvariant())$(if ($lock.Slot) { " slot $($lock.Slot)" }), user data out\bench\$work"
$k = 0
foreach ($s in $Scripts) {
    $se = if ($k -lt @($ScriptExtras).Count) { "$($ScriptExtras[$k])" } else { '' }
    $k++
    @(Invoke-RunListScript $s $se $lock $work)
    if ($script:RunListChecks) { Write-Host $script:RunListChecks }   # Session 29: under the result line
}
} finally { Exit-RunLock $lock }
