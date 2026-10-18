# EXPERIMENTAL (Session 8, task L3): TIMED runs (fps, frame times, GPU ms, in-run A/B) in the run lock's SHARED
# lanes, to measure what neighbours cost. Not part of any normal workflow: today's rule is still "timed runs are
# exclusive" (docs\harness\runlock.md). Findings and the proposed rule: docs\harness\lanes_timed.md.
#
#   & tools\bench\run_timed_concurrent.ps1 -Scripts @('A.txt','B.txt') -Extra '--x=1' -Label NAME       one lane per script
#   & tools\bench\run_timed_concurrent.ps1 -Scripts @('A.txt') -Copies 3 -Extra '--x=1' -Label NAME     3 copies of A
#   & tools\bench\run_timed_concurrent.ps1 -Scripts @('A.txt') -Exclusive -Extra '--x=1' -Label NAME    1-lane reference
# Call it in-process (pwsh -NoProfile -Command "& ...") when -Extra starts with --. Script paths: absolute, or
# relative to the repo root (made absolute here, because -Exe may live in another folder).
#
# What it does:
#  - Slots (fixer rewrite after review): the LAUNCHER takes every slot the batch needs before any lane starts and
#    keeps them all until the last lane has ended: all 3 shared slots (the lanes' and, unless -NoIdleHold, the unused
#    ones, so no other agent's run can join the batch, not even after a lane ends early), or the exclusive lock with
#    -Exclusive (1-lane reference). The lanes are child processes (this file with -ChildJob) that re-enter their slot
#    by its token (WWE2K14_RUNLOCK_TOKEN, the run lock's re-entry, as regress -> run_list -> run_and_triage do), so a
#    lane gives nothing back when it ends, and no lane starts unless every slot is held. The slots show in
#    `runlock.ps1 status` as "lanes_timed LABEL hold K of 3".
#  - Gathering never stalls the queue: the first slot is an ordinary FIFO wait; the others are taken only when the
#    lock state says the take goes at once; and whenever an EXCLUSIVE ticket is waiting (a build, regress, another
#    timed run) every slot taken so far is given back at once and the launcher queues again behind it (an exclusive
#    waiter needs every shared slot free, so a held slot would stall it, and everyone queued behind it, until a
#    timeout). A part of the slots held for -PartialHoldMinutes (15) without the rest (other agents' runs keep them)
#    is given back too, and the gather starts again. -GatherMinutes (90; old name -BarrierMinutes) bounds the whole
#    gather: past it the batch ends and no lane was started.
#  - Each lane calls tools\bench\run_list.ps1 in-process inside its slot (own user data copy
#    out\bench\userdata_work_s<slot>, frozen user data mirrored, timeline on, killed after -WaitSeconds, launches
#    serialized by run_and_triage's launch window: lanes start ~3.5 s apart).
#  - -Affinity: per-lane processor mask (hex, e.g. 0xFFFF) or the preset 'ccd' (lane 1 = logical processors 0-15 =
#    CCD0, other lanes = 16-31 = CCD1). The game does not inherit its launcher's affinity (measured), so a thread job
#    in the lane finds the lane's game by its user data folder and sets the mask on the game process (re-applied
#    whenever it differs: ReXGlue resets it at startup); result_N.json / batch.json record what was set, and
#    lanes_monitor.ps1 logs each game's mask.
#  - Monitors while the lanes run (tools\bench\lanes_monitor.ps1, two processes, started once the slots are held;
#    they stop on the batch's 'stop' file or when this launcher is gone): GPU = nvidia-smi utilization.gpu,
#    power.draw, clocks.gr, memory.used every 500 ms; CPU = total / per CCD / per game, available memory, foreground
#    window's pid, every ~1 s.
#  - -DryRunSeconds N: lanes start no game; lane K checks that it re-enters its slot, then sleeps K x N s (so lanes
#    end at different times, as real ones do). Tests the slots, the gather and the monitors.
#  - Batch folder out\harness\lanes\batches\<stamp>_<label>\: batch.json (slots, gather, lanes, run numbers, run_list
#    lines, exe hash, times), gpu.csv, cpu.csv, lane_N.out. Analysis: py tools\bench\lanes_report.py BATCH_DIR...
#    (it also lists any game that was not one of the batch's lanes while they ran).
# Default -Exe is the experiment's snapshot out\harness\lanes\exe\wwe2k14.exe (its own logs folder, so these runs
# never land in the release logs or the regress baseline). The snapshot must log "run log number" (shared-safe).
param([string[]]$Scripts = @(), [int]$Copies = 0, [string]$Extra = '', [string[]]$LaneExtra = @(), [string]$Label = 'batch',
      [switch]$Exclusive, [switch]$NoIdleHold, [string[]]$Affinity = @(), [int]$WaitSeconds = 720,
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\harness\lanes\exe\wwe2k14.exe', [switch]$NoMonitor,
      [Alias('BarrierMinutes')][double]$GatherMinutes = 90, [double]$PartialHoldMinutes = 15, [int]$DryRunSeconds = 0,
      [string]$ChildJob = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------------------------------------ child (one lane)
if ($ChildJob) {
    $job = Get-Content -LiteralPath $ChildJob -Raw | ConvertFrom-Json
    $res = [ordered]@{ id = $job.id; slot = $job.slot; mode = $job.mode; started = $null; ended = $null; affinity = $null;
                       reentry = $null; lines = @(); error = $null }
    $resPath = Join-Path $job.dir ("result_{0}.json" -f $job.id)
    . "$R\tools\runlock.ps1"
    try {
        # Run only inside the hold the launcher took for this lane: its record must carry this lane's token and the
        # launcher's pid, and the launcher must be alive (a dead launcher's slots are free: start nothing).
        $rec = if ($job.mode -eq 'Exclusive') { Read-RunLockJson $script:RunLockFile } else { (Get-RunLockSlots | Where-Object Slot -eq ([int]$job.slot)).Rec }
        if (-not ($rec -and $rec.token -eq $job.token -and [int]$rec.pid -eq [int]$job.parent -and (Test-RunLockOwnerAlive $rec))) {
            throw "the launcher (pid $($job.parent)) does not hold $(if ($job.mode -eq 'Exclusive') { 'the exclusive lock' } else { "shared slot $($job.slot)" }) any more; no game started"
        }
        if (Test-Path -LiteralPath (Join-Path $job.dir 'abort')) { throw 'the batch was aborted before this lane started; no game started' }
        $env:WWE2K14_RUNLOCK_TOKEN = $job.token   # run_list / run_and_triage re-enter this hold instead of queueing
        $aff = $null
        if ($job.affinity -and -not $job.dry_run_seconds) {
            # The game does NOT inherit this process's affinity (measured: Start-Process in run_and_triage gave it
            # FFFFFFFF), so a thread job finds this lane's game by its user data folder (one game per slot) and
            # sets the mask on the game process itself.
            $mask = [int64][Convert]::ToInt64(($job.affinity -replace '^0x', ''), 16)
            $ud = if ($job.mode -eq 'Shared') { "$R\out\bench\userdata_work_s$($job.slot)" } else { "$R\out\bench\userdata_work" }
            $stopAff = Join-Path $job.dir ("affstop_{0}" -f $job.id)
            $aff = Start-ThreadJob -ScriptBlock {
                param($ud, $mask, $stopAff)
                # ReXGlue resets the process mask to all processors during startup (src\system\runtime.cpp calls
                # thread::EnableAffinityConfiguration -> SetProcessAffinityMask(system mask)), so a mask set at
                # launch is undone: keep checking the lane's game every 250 ms and set it again when it differs.
                $rx = '--user_data_root="?' + [regex]::Escape($ud) + '\\?(?=["\s]|$)'
                $game = $null; $lastPid = $null; $sets = 0; $first = $null; $last = $null
                while (-not (Test-Path -LiteralPath $stopAff)) {
                    if (-not $game -or $game.HasExited) {
                        $game = $null
                        $w = @(Get-CimInstance Win32_Process -Filter "Name='wwe2k14.exe'" -Property ProcessId, CommandLine -ErrorAction SilentlyContinue | Where-Object { "$($_.CommandLine)" -match $rx }) | Select-Object -First 1
                        if ($w) { try { $game = Get-Process -Id $w.ProcessId -ErrorAction Stop; $lastPid = $game.Id } catch { } }
                    }
                    if ($game) {
                        try {
                            $game.Refresh()
                            if ([int64]$game.ProcessorAffinity -ne $mask) {
                                $game.ProcessorAffinity = [IntPtr]$mask; $sets++
                                $t = '{0} (game started {1})' -f (Get-Date).ToString('HH:mm:ss.fff'), $game.StartTime.ToString('HH:mm:ss.fff')
                                if (-not $first) { $first = $t }; $last = $t
                            }
                        } catch { }
                    }
                    Start-Sleep -Milliseconds 250
                }
                # Fixer (review nit): $game is $null once the game has exited, so the summary uses the last pid seen.
                'pid {0}: mask 0x{1:X} set {2} time(s), first {3}, last {4}' -f $(if ($lastPid) { $lastPid } else { '?' }), $mask, $sets, $first, $last
            } -ArgumentList $ud, $mask, $stopAff
        }
        $res.started = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
        if ($job.dry_run_seconds) {
            $h = Enter-RunLock -Name "lanes_timed dry run lane $($job.id)" -Mode $job.mode -Quiet -TimeoutMinutes 0.2
            $res.reentry = "reentrant $($h.Reentrant), mode $($h.Mode), slot $($h.Slot)"
            Exit-RunLock $h
            if (-not $h.Reentrant -or [int]$h.Slot -ne [int]$job.slot) { throw "dry run: did not re-enter the launcher's hold ($($res.reentry))" }
            Start-Sleep -Seconds ([int]$job.dry_run_seconds * [int]$job.id)
            $out = "$(Split-Path $job.script -Leaf) -> DRYRUN slot $($job.slot) mode $($job.mode), $($res.reentry)"
        } else {
            $out = & "$R\tools\bench\run_list.ps1" -Scripts @($job.script) -Extra $job.extra -Exe $job.exe -WaitSeconds $job.wait -Exclusive:($job.mode -eq 'Exclusive') -Foreground 2>&1  # Session 30: timed = focused window
        }
        $res.ended = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
        $res.lines = @($out | ForEach-Object { "$_" })
        if ($aff) {
            Set-Content -LiteralPath $stopAff -Value 'stop'
            $res.affinity = "$($job.affinity): " + ((@(Receive-Job $aff -Wait -AutoRemoveJob) | ForEach-Object { "$_" }) -join '; ')
        }
    } catch {
        $res.error = $_.Exception.Message
        # Lanes not started yet see this and start nothing (they all start within ~1 s of each other).
        if (-not $res.started) { Set-Content -LiteralPath (Join-Path $job.dir 'abort') -Value "lane $($job.id): $($res.error)" -ErrorAction SilentlyContinue }
    } finally {
        $res | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resPath -Encoding utf8
    }
    exit 0
}

# ------------------------------------------------------------------------------------------------ launcher
. "$R\tools\runlock.ps1"
if (-not $Scripts.Count) { throw 'run_timed_concurrent: -Scripts is empty' }
if ($Copies -gt 0) { $Scripts = @(1..$Copies | ForEach-Object { $Scripts[0] }) }
$n = $Scripts.Count
$max = $script:RunLockMaxShared
if ($Exclusive -and $n -ne 1) { throw 'run_timed_concurrent: -Exclusive is the 1-lane reference; pass one script' }
if ($n -gt $max) { throw "run_timed_concurrent: $n lanes, the lock allows $max shared holders" }
if (-not (Test-Path -LiteralPath $Exe)) { throw "run_timed_concurrent: no exe at $Exe" }
if (-not $Exclusive -and -not (Test-RunLockExeSharedSafe $Exe)) { throw "run_timed_concurrent: $Exe does not log 'run log number': not safe to run shared" }
if ($env:WWE2K14_RUNLOCK_TOKEN) { throw 'run_timed_concurrent: called inside a run-lock hold (WWE2K14_RUNLOCK_TOKEN is set); it takes its own slots' }
$Scripts = @($Scripts | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { [IO.Path]::GetFullPath((Join-Path $R $_)) } })
foreach ($s in $Scripts) { if (-not (Test-Path -LiteralPath $s)) { throw "run_timed_concurrent: no script $s" } }
$masks = @()
if ($Affinity.Count -eq 1 -and $Affinity[0] -eq 'ccd') {
    $lp = [Environment]::ProcessorCount; $halfMask = (([int64]1 -shl ($lp / 2)) - 1)
    $masks = @(1..$n | ForEach-Object { if ($_ -eq 1) { '0x{0:X}' -f $halfMask } else { '0x{0:X}' -f ($halfMask -shl ($lp / 2)) } })
} elseif ($Affinity.Count) { $masks = $Affinity }

$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$dir = Join-Path $R ("out\harness\lanes\batches\{0}_{1}" -f $stamp, ($Label -replace '\W+', '_'))
if (Test-Path -LiteralPath $dir) { $dir = "${dir}_$PID" }   # two launches of one label in the same second
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$sha = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant()
$mode = if ($Exclusive) { 'Exclusive' } else { 'Shared' }
$total = if ($Exclusive) { 1 } elseif ($NoIdleHold) { $n } else { $max }
$idleN = $total - $n
Write-Host "[lanes] batch $dir : $n lane(s) $mode$(if ($idleN) { " + $idleN idle slot(s) held by the launcher" }), exe sha256 $($sha.Substring(0, 12))"

$held = [System.Collections.Generic.List[object]]::new()
$gather = [ordered]@{ queued = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); all_held = $null; waited_s = $null; yields = @() }
# Enter-RunLock records the newest hold in $env:WWE2K14_RUNLOCK_TOKEN / $global:WWE2K14RunLockHeld. This process holds
# several, so it clears both after each take: the next Enter-RunLock then takes a NEW slot (instead of re-entering the
# last one or releasing it as a lost hold), and no child inherits a token by accident (lanes get theirs explicitly).
function Clear-LanesHoldContext { Remove-Item Env:WWE2K14_RUNLOCK_TOKEN -ErrorAction SilentlyContinue; $global:WWE2K14RunLockHeld = $null }
function Exit-LanesHolds { foreach ($h in @($held)) { Exit-RunLock $h }; $held.Clear() }
function Get-LanesHoldName([int]$K) {
    if ($Exclusive) { return "lanes_timed $Label (EXPERIMENT: exclusive 1-lane reference)" }
    "lanes_timed $Label hold $K of $total (EXPERIMENT: timed lanes batch, $n lane(s)$(if ($idleN) { " + $idleN idle" }))"
}

$fatal = $null
$kids = @(); $mon = @()
$t0 = Get-Date
try {
    # ---- gather every slot before any lane starts
    $deadline = (Get-Date).AddMinutes($GatherMinutes)
    if ($Exclusive) {
        $h = Enter-RunLock -Name (Get-LanesHoldName 1) -Mode Exclusive -TimeoutMinutes $GatherMinutes
        Clear-LanesHoldContext; $held.Add($h)
    }
    while ($held.Count -lt $total) {
        $left = ($deadline - (Get-Date)).TotalMinutes
        if ($left -le 0) { throw "gather: could not hold $total shared slots at once within $GatherMinutes min ($($gather.yields.Count) give-back(s): see batch.json gather.yields); no lane was started" }
        if ($held.Count -eq 0) {
            $h = Enter-RunLock -Name (Get-LanesHoldName 1) -Mode Shared -TimeoutMinutes $left   # an ordinary FIFO wait
            Clear-LanesHoldContext; $held.Add($h); $partialSince = Get-Date
            continue
        }
        if (((Get-Date) - $partialSince).TotalMinutes -gt $PartialHoldMinutes) {
            # Other agents' runs kept the rest busy: do not sit on part of the lanes any longer; start again.
            $y = '{0} gave back {1} slot(s) after {2} min without the other {3}' -f (Get-Date).ToString('HH:mm:ss'), $held.Count, $PartialHoldMinutes, ($total - $held.Count)
            Exit-LanesHolds
            $gather.yields += $y
            Write-Host "[lanes] gather: $y; queueing again"
            continue
        }
        $s = Get-RunLockState
        $mine = @($held | ForEach-Object Token)
        $ex = @($s.Tickets | Where-Object { (Get-RunLockTicketMode $_) -eq 'exclusive' -and (Test-RunLockOwnerAlive $_) })
        if ($ex.Count) {
            # An exclusive waiter needs every shared slot free: give ours back now and queue again behind it.
            $y = '{0} gave back {1} slot(s) to exclusive waiter ''{2}'' (pid {3})' -f (Get-Date).ToString('HH:mm:ss'), $held.Count, $ex[0].name, $ex[0].pid
            Exit-LanesHolds
            $gather.yields += $y
            Write-Host "[lanes] gather: $y; queueing again behind it"
            continue
        }
        # Other holders' live slots (ours are excluded by token), and live tickets: every ticket now in the queue is
        # shared (no exclusive one, checked above) and would go before a ticket written now.
        $others = @($s.Slots | Where-Object { $_.Rec -and $mine -notcontains $_.Rec.token -and (Test-RunLockOwnerAlive $_.Rec) })
        $waiting = @($s.Tickets | Where-Object { Test-RunLockOwnerAlive $_ })
        $free = $max - $others.Count - $held.Count
        $unowned = @(Get-RunLockUnownedGames $s.Games (@($others | ForEach-Object { [int]$_.Rec.pid }) + $PID))
        if ($free -gt $waiting.Count -and $s.Games.Count -lt $max -and -not $unowned.Count) {
            # Should go at once; a short timeout only covers a lost race (someone took the slot first, or an exclusive
            # ticket arrived in between), after which the loop looks again (and yields if it is an exclusive one).
            try {
                $h = Enter-RunLock -Name (Get-LanesHoldName ($held.Count + 1)) -Mode Shared -TimeoutMinutes 0.25 -PollMs 500 -Quiet
                Clear-LanesHoldContext; $held.Add($h)
            } catch { Clear-LanesHoldContext }
        } else {
            Start-Sleep -Milliseconds 1000
        }
    }
    $gather.all_held = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    $gather.waited_s = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $slotList = ($held | ForEach-Object { if ($_.Mode -eq 'Shared') { "slot $($_.Slot)" } else { 'exclusive' } }) -join ', '
    Write-Host ("[lanes] holding {0} after {1:N0} s{2}; starting {3} lane(s)" -f $slotList, $gather.waited_s,
        $(if ($gather.yields.Count) { " (after $($gather.yields.Count) give-back(s): $($gather.yields -join "; "))" } else { '' }), $n)

    # ---- monitors (first samples in before the first game starts)
    if (-not $NoMonitor) {
        foreach ($kind in @('gpu', 'cpu')) {
            $margs = @('-NoProfile', '-File', "$R\tools\bench\lanes_monitor.ps1", '-Out', (Join-Path $dir "$kind.csv"), '-StopFile', (Join-Path $dir 'stop'), '-ParentPid', "$PID")
            if ($kind -eq 'gpu') { $margs += '-Gpu' }
            $mon += Start-Process -FilePath 'pwsh' -ArgumentList $margs -PassThru -WindowStyle Hidden
        }
        $until = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $until) {
            $ok = @('gpu', 'cpu' | Where-Object { $f = Join-Path $dir "$_.csv"; (Test-Path -LiteralPath $f) -and @(Get-Content -LiteralPath $f -ErrorAction SilentlyContinue).Count -ge 2 }).Count
            if ($ok -eq 2) { break }
            Start-Sleep -Milliseconds 250
        }
    }

    # ---- lanes: lane i runs in held[i-1]
    for ($i = 1; $i -le $n; $i++) {
        $hh = $held[$i - 1]
        $job = [ordered]@{ id = $i; dir = $dir; parent = $PID; mode = $mode; slot = $hh.Slot; token = $hh.Token;
                           exe = $Exe; wait = $WaitSeconds; dry_run_seconds = $DryRunSeconds; script = $Scripts[$i - 1];
                           extra = ("$Extra " + $(if ($LaneExtra.Count -ge $i) { $LaneExtra[$i - 1] } else { '' })).Trim();
                           affinity = if ($masks.Count -ge $i) { $masks[$i - 1] } else { $null } }
        $jp = Join-Path $dir ("job_{0}.json" -f $i)
        $job | ConvertTo-Json | Set-Content -LiteralPath $jp -Encoding utf8
        $p = Start-Process -FilePath 'pwsh' -ArgumentList '-NoProfile', '-File', $PSCommandPath, '-ChildJob', $jp -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dir ("lane_{0}.out" -f $i)) -RedirectStandardError (Join-Path $dir ("lane_{0}.err" -f $i))
        # Best effort: ties the lane (and the game it starts) to this process, so stopping the launcher stops them.
        # If it fails (the runlock warning says why), a lane whose game is running finishes on its own (run_list
        # still kills a game past -WaitSeconds); this launcher's slots are freed when it exits either way, and its
        # games still running then block every other taker as games no live holder started (runlock.ps1 rule).
        $null = Add-RunLockKillOnExit $p
        $kids += [pscustomobject]@{ Id = $i; Proc = $p }
        Start-Sleep -Milliseconds 300       # lanes start in lane order
    }
    # A lane ends by itself (run_list kills a game past -WaitSeconds; triage <= 300 s). If one is still going 15 min
    # after that, stop holding the slots anyway (its game, if any, then blocks other takers as an unowned game).
    $limit = (Get-Date).AddSeconds($WaitSeconds + 900 + [int]$DryRunSeconds * $n)
    foreach ($k in $kids) {
        $ms = [Math]::Max(0, [int](($limit - (Get-Date)).TotalMilliseconds))
        if (-not $k.Proc.WaitForExit($ms)) { throw "lane $($k.Id) (pid $($k.Proc.Id)) still running $($WaitSeconds + 900) s after the start; giving the slots back" }
    }
} catch {
    $fatal = $_.Exception.Message
    Write-Host "[lanes] ERROR: $fatal"
} finally {
    # Every lane has ended (or none started): stop the monitors, then give every slot back.
    Set-Content -LiteralPath (Join-Path $dir 'stop') -Value (Get-Date).ToString('o')
    $relSlots = ($held | ForEach-Object { if ($_.Mode -eq 'Shared') { "slot $($_.Slot)" } else { 'exclusive' } }) -join ', '
    $heldInfo = @($held | ForEach-Object { [ordered]@{ mode = $_.Mode; slot = $_.Slot; name = $_.Name; acquired = $(if ($_.AcquiredAt) { $_.AcquiredAt.ToString('yyyy-MM-dd HH:mm:ss.fff') } else { $null }) } })
    Exit-LanesHolds
    $released = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    if ($relSlots) { Write-Host "[lanes] released $relSlots at $released" }
}
foreach ($m in $mon) { $null = $m.WaitForExit(10000) }   # they stop on the 'stop' file

$lanes = for ($i = 1; $i -le $n; $i++) {
    $rp = Join-Path $dir ("result_{0}.json" -f $i)
    $r = if (Test-Path -LiteralPath $rp) { Get-Content -LiteralPath $rp -Raw | ConvertFrom-Json } else { $null }
    $line = if ($r) { @($r.lines | Where-Object { $_ -match ' -> ' }) | Select-Object -Last 1 } else { $null }
    $run = if ($line -match ' -> (wwe2k14_\d+)') { $Matches[1] } else { $null }
    [ordered]@{ lane = $i; idle = $false; script = $Scripts[$i - 1];
                slot = if ($r) { $r.slot } else { $null }; affinity = if ($r) { $r.affinity } else { $null };
                run = $run; run_list_line = $line; started = if ($r) { $r.started } else { $null };
                ended = if ($r) { $r.ended } else { $null }; reentry = if ($r) { $r.reentry } else { $null };
                error = if ($r) { $r.error } elseif ($fatal) { "not started: $fatal" } else { 'no result file' } }
}
$batch = [ordered]@{ label = $Label; dir = $dir; mode = $mode; lanes_n = $n; idle_holds = $idleN; exe = $Exe; exe_sha256 = $sha;
                     extra = $Extra; lane_extra = $LaneExtra; affinity = $masks; started = $t0.ToString('yyyy-MM-dd HH:mm:ss');
                     ended = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); slot_holds = 'launcher'; holds = $heldInfo;
                     gather = $gather; released = $released; error = $fatal; lanes = @($lanes) }
$batch | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dir 'batch.json') -Encoding utf8
foreach ($l in $lanes) {
    Write-Host ("[lanes] lane {0} slot {1}{2}: {3}{4}" -f $l.lane, $l.slot, $(if ($l.affinity) { " affinity $($l.affinity)" } else { '' }),
        $(if ($l.run_list_line) { $l.run_list_line } else { '(no run_list line)' }), $(if ($l.error) { "  ERROR: $($l.error)" } else { '' }))
}
Write-Host "[lanes] done: $dir"
if ($fatal) { exit 1 }
