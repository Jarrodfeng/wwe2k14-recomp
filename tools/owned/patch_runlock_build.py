"""One-time patch (Session 82, harness builder): adds the BUILD class to tools/runlock.ps1 and tools/harness/runlock.py.
Kept in the repo as the record of exactly what changed; running it twice refuses (the marker is present).
Each replacement must match exactly once. CRLF files: matched on LF-normalised text, written back as CRLF."""
import os
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'


def apply(path, reps, marker):
    raw = open(path, 'rb').read().decode('utf-8')
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    if marker in text:
        print('already patched:', path)
        return
    for i, (old, new) in enumerate(reps):
        n = text.count(old)
        if n != 1:
            sys.exit('%s: replacement %d matches %d times:\n%s' % (path, i, n, old[:200]))
        text = text.replace(old, new)
    if crlf:
        text = text.replace('\n', '\r\n')
    tmp = path + '.new'
    open(tmp, 'wb').write(text.encode('utf-8'))
    os.replace(tmp, path)   # copy-then-rename: a starting process never reads half a file
    print('patched:', path, len(reps), 'replacements')


PS = os.path.join(REPO, 'tools', 'runlock.ps1')
ps = []

# 1. design note
ps.append(("# As a library (dot-source it):\n", r"""# BUILD class (Session 82, harness builder; Jarrod: builds should share the PC with untimed runs, only timed runs exclusive).
#   DESIGN NOTE - why it is small and what it does NOT change:
#   A build hold is a third kind of holder next to EXCLUSIVE and SHARED. It is made of the same two Windows primitives
#   as a shared hold, with its own slot files (build1.json .. build<RunLockMaxBuild>.json, default 2 at once):
#     - a record file held open read/write (the slot; its handle is what limits the count to RunLockMaxBuild), and
#     - a READ-ONLY handle on lock.json sharing read. Shared holders keep the same handle, so builds and shared runs
#       coexist, while an exclusive taker (new copy, or an OLD copy of this script, which only knows lock.json) finds
#       lock.json busy: a build excludes every exclusive hold (timed runs, benchmarks, regress, build_sdk.ps1, ...)
#       with no change to how those take the lock. The old exclusive take also never sees build records (not slot*.json).
#   Queue: tickets carry mode 'build' (an old copy's ticket has none = exclusive, unchanged). FIFO as before: a build
#   never overtakes an exclusive waiter (no starvation of timed runs); a build waiter goes when no exclusive ticket is
#   ahead and fewer than RunLockMaxBuild builds are held or ahead of it. Shared waiters count only SHARED tickets ahead
#   of them (a queued build does not take one of their 3 slots; with no build tickets the count is exactly as before).
#   Exclusive waiters wait while a build is held (the lock.json handle already forces it; the message names the build).
#   A build does not occupy a game slot (it runs no game) and is not limited by the 3-game cap; like a shared run it
#   waits for a game that no live holder started (a manual session or an orphan) to end, as exclusive and shared do.
#   Unchanged: Exclusive and Shared callers (same functions, same files, same messages; history gains only rows with
#   mode 'build', slot 11/12). Re-entry: a child of a build hold re-enters it (a SHARED request inside a build is
#   allowed and re-enters; an EXCLUSIVE request throws, like inside a shared hold). Use:  runlock.ps1 run -Build -Name N -- CMD
#   or  Enter-RunLock -Mode Build;  Python: ensure_lock(name, build=True).
#
# As a library (dot-source it):
"""))

# 2. constant
ps.append(("$script:RunLockMaxShared = 3   # THE setting: shared holders at once, and wwe2k14 processes at once in shared mode\n",
           "$script:RunLockMaxShared = 3   # THE setting: shared holders at once, and wwe2k14 processes at once in shared mode\n"
           "$script:RunLockMaxBuild = 2    # BUILD class: builds at once (see the design note); they run no game\n"))

# 3. build slot helpers + ticket mode
ps.append(("function Get-RunLockTicketMode($t) { if ($t -and \"$($t.mode)\" -eq 'shared') { 'shared' } else { 'exclusive' } }\n",
           r"""function Get-RunLockBuildFile([int]$Slot) { Join-Path $script:RunLockDir "build$Slot.json" }

# A build slot file opened the way its holder does (read/write, others may only read). $null = held, or being taken.
function Open-RunLockBuildSlot([int]$Slot) {
    try { return [System.IO.File]::Open((Get-RunLockBuildFile $Slot), [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read) }
    catch { return $null }
}

function Get-RunLockBuildSlots {
    @(1..$script:RunLockMaxBuild | ForEach-Object {
        $f = Get-RunLockBuildFile $_
        $r = Read-RunLockJson $f
        [pscustomobject]@{ Slot = $_; File = $f; Rec = $r; Live = (Test-RunLockRecHeld $r) } })
}

function Get-RunLockTicketMode($t) {
    $m = if ($t) { "$($t.mode)" } else { '' }
    if ($m -eq 'shared') { 'shared' } elseif ($m -eq 'build') { 'build' } else { 'exclusive' }
}
"""))

# 4. state
ps.append(("""    $slots = Get-RunLockSlots
    [pscustomobject]@{ Lock = $lock; LockFileExists = $exists; LockAlive = (Test-RunLockOwnerAlive $lock)
        Slots = $slots; LiveSlots = @($slots | Where-Object Live)
""", """    $slots = Get-RunLockSlots
    $builds = Get-RunLockBuildSlots
    [pscustomobject]@{ Lock = $lock; LockFileExists = $exists; LockAlive = (Test-RunLockOwnerAlive $lock)
        Slots = $slots; LiveSlots = @($slots | Where-Object Live)
        BuildSlots = $builds; LiveBuilds = @($builds | Where-Object Live)
"""))

# 5. clear-stale
ps.append(("""    $fs = Open-RunLockForTake ([System.IO.FileMode]::Open)
    if (-not $fs) { return }
    $cleared = $null
""", """    for ($n = 1; $n -le $script:RunLockMaxBuild; $n++) {
        if (-not (Test-Path -LiteralPath (Get-RunLockBuildFile $n))) { continue }
        $sf = Open-RunLockBuildSlot $n
        if (-not $sf) { continue }
        try {
            $cur = Read-RunLockStream $sf
            if ($cur.Rec -and -not $cur.Live) {
                $sf.SetLength(0); $sf.Flush($true)
                if (-not $Quiet) { "removed stale build slot $($n): '$($cur.Rec.name)' pid $($cur.Rec.pid) since $(Format-RunLockTime $cur.Rec.acquired)" }
            }
        } finally { $sf.Dispose() }
    }
    $fs = Open-RunLockForTake ([System.IO.FileMode]::Open)
    if (-not $fs) { return }
    $cleared = $null
"""))

# 6. Enter-RunLock
ps.append(("[ValidateSet('Exclusive', 'Shared')][string]$Mode = 'Exclusive')", "[ValidateSet('Exclusive', 'Shared', 'Build')][string]$Mode = 'Exclusive')"))

ps.append(("""    }
    # A lock this process still holds but whose token is gone (Exit-RunLock never ran): release it, or we'd wait on it.
""", """        foreach ($bl in (Get-RunLockBuildSlots)) {
            if ($bl.Rec -and $bl.Rec.token -eq $inherited -and (Test-RunLockOwnerAlive $bl.Rec)) {
                if ($Mode -eq 'Exclusive') {
                    throw "[runlock] '$Name' asks for the EXCLUSIVE lock inside build slot $($bl.Slot) held by '$($bl.Rec.name)' (pid $($bl.Rec.pid)): it would wait on its own parent forever. Take the exclusive lock in the parent instead."
                }
                if (-not $Quiet) { Write-RunLockMsg "[runlock] re-entered build slot $($bl.Slot) held by '$($bl.Rec.name)' (pid $($bl.Rec.pid)) for '$Name'" }
                Write-RunLockHistory -Kind 'reenter' -Mode 'build' -Slot (10 + $bl.Slot) -Name $Name -Waited 0 -Token $inherited -Note "inside '$($bl.Rec.name)' pid $($bl.Rec.pid)"
                return [pscustomobject]@{ Token = $inherited; Reentrant = $true; Name = $Name; Stream = $null; LockStream = $null; Mode = 'Build'; Slot = $bl.Slot }
            }
        }
    }
    # A lock this process still holds but whose token is gone (Exit-RunLock never ran): release it, or we'd wait on it.
"""))

ps.append(("""    $shared = $Mode -eq 'Shared'
    $modeWord = if ($shared) { 'shared' } else { 'exclusive' }
""", """    $shared = $Mode -eq 'Shared'
    $build = $Mode -eq 'Build'
    $modeWord = if ($shared) { 'shared' } elseif ($build) { 'build' } else { 'exclusive' }
"""))

ps.append(("""            $exAhead = @($ahead | Where-Object { (Get-RunLockTicketMode $_) -eq 'exclusive' })
""", """            $exAhead = @($ahead | Where-Object { (Get-RunLockTicketMode $_) -eq 'exclusive' })
            $shAhead = @($ahead | Where-Object { (Get-RunLockTicketMode $_) -eq 'shared' }).Count   # shared waiters count only shared tickets ahead
            $buAhead = @($ahead | Where-Object { (Get-RunLockTicketMode $_) -eq 'build' }).Count
"""))

ps.append(("""            elseif (-not $shared) {
                if ($idx -ne 0) { $why = 'waiting for earlier tickets' }
                elseif ($s.LiveSlots.Count) { $why = "$($s.LiveSlots.Count) shared holder(s) running: " + (($s.LiveSlots | ForEach-Object { "'$($_.Rec.name)' pid $($_.Rec.pid)" }) -join ', ') }
""", """            elseif ($build) {
                # BUILD class (design note in the header): coexists with shared holders, never next to an exclusive one.
                if ($exAhead.Count) { $why = "an exclusive waiter is ahead ('$($exAhead[0].name)'); not overtaking it" }
                elseif ($s.LiveBuilds.Count + $buAhead -ge $script:RunLockMaxBuild) { $why = "all $($script:RunLockMaxBuild) build slots busy or claimed by earlier tickets ($($s.LiveBuilds.Count) held)" }
                else {
                    $holderPids = @($s.LiveSlots | ForEach-Object { [int]$_.Rec.pid })
                    $unowned = @(Get-RunLockUnownedGames $s.Games $holderPids)
                    if ($unowned.Count) { $why = "wwe2k14 running that no shared holder started (pid $(($unowned | ForEach-Object Id) -join ',')): a manual session or a run that outlived its launcher; waiting for it to exit" }
                    else { $go = $true }
                }
            }
            elseif (-not $shared) {
                if ($idx -ne 0) { $why = 'waiting for earlier tickets' }
                elseif ($s.LiveSlots.Count) { $why = "$($s.LiveSlots.Count) shared holder(s) running: " + (($s.LiveSlots | ForEach-Object { "'$($_.Rec.name)' pid $($_.Rec.pid)" }) -join ', ') }
                elseif ($s.LiveBuilds.Count) { $why = "$($s.LiveBuilds.Count) build holder(s) running: " + (($s.LiveBuilds | ForEach-Object { "'$($_.Rec.name)' pid $($_.Rec.pid)" }) -join ', ') }
"""))

ps.append(("elseif ($s.LiveSlots.Count + $idx -ge $max) {", "elseif ($s.LiveSlots.Count + $shAhead -ge $max) {"))

ps.append(("            if ($go -and -not $shared) {\n", "            if ($go -and -not $shared -and -not $build) {\n"))

ps.append(("""                $got = $null; $tookSlot = $null
                for ($n = 1; $n -le $max -and -not $got; $n++) {
                    $sf = Open-RunLockSlot $n
""", """                $got = $null; $tookSlot = $null
                $nSlots = if ($build) { $script:RunLockMaxBuild } else { $max }
                for ($n = 1; $n -le $nSlots -and -not $got; $n++) {
                    $sf = if ($build) { Open-RunLockBuildSlot $n } else { Open-RunLockSlot $n }
"""))

ps.append(("Stream = $sf; LockStream = $lf; Mode = 'Shared'; Slot = $n\n",
           "Stream = $sf; LockStream = $lf; Mode = $(if ($build) { 'Build' } else { 'Shared' }); Slot = $n\n"))

ps.append(("""                        Write-RunLockMsg ("[runlock] acquired SHARED slot $($got.Slot) of $max for '$Name' after {0:N0} s" -f $got.WaitedSeconds)
                    }
                    Write-RunLockHistory -Kind 'acquire' -Mode 'shared' -Slot $got.Slot -Name $Name -Waited $got.WaitedSeconds `
""", """                        Write-RunLockMsg ("[runlock] acquired $($modeWord.ToUpperInvariant()) slot $($got.Slot) of $nSlots for '$Name' after {0:N0} s" -f $got.WaitedSeconds)
                    }
                    Write-RunLockHistory -Kind 'acquire' -Mode $modeWord -Slot $(if ($build) { 10 + $got.Slot } else { $got.Slot }) -Name $Name -Waited $got.WaitedSeconds `
"""))

# 7. Exit-RunLock
ps.append(("""    if ("$($Handle.Mode)" -eq 'Shared') {
        # Empty the slot while still holding it""", """    if ("$($Handle.Mode)" -in 'Shared', 'Build') {
        # Empty the slot while still holding it"""))

ps.append(("""        $exclusive = "$($Handle.Mode)" -ne 'Shared'
""", """        $exclusive = "$($Handle.Mode)" -notin 'Shared', 'Build'
        $isBuild = "$($Handle.Mode)" -eq 'Build'
"""))

ps.append(("""        Write-RunLockHistory -Kind 'release' -Mode $(if ($exclusive) { 'exclusive' } else { 'shared' }) -Slot ([int]$Handle.Slot) `
""", """        Write-RunLockHistory -Kind 'release' -Mode $(if ($exclusive) { 'exclusive' } elseif ($isBuild) { 'build' } else { 'shared' }) -Slot ([int]$Handle.Slot + $(if ($isBuild) { 10 } else { 0 })) `
"""))

ps.append(("""            $what = if ($exclusive) { 'EXCLUSIVE' } else { "SHARED slot $($Handle.Slot)" }
""", """            $what = if ($exclusive) { 'EXCLUSIVE' } elseif ($isBuild) { "BUILD slot $($Handle.Slot)" } else { "SHARED slot $($Handle.Slot)" }
"""))

# 8. status
ps.append(("""    $mode = if (Test-RunLockOwnerAlive $s.Lock) { 'EXCLUSIVE' } elseif ($s.LiveSlots.Count) { "SHARED ($($s.LiveSlots.Count) of $script:RunLockMaxShared slots)" } else { 'free' }
""", """    $mode = if (Test-RunLockOwnerAlive $s.Lock) { 'EXCLUSIVE' } elseif ($s.LiveSlots.Count) { "SHARED ($($s.LiveSlots.Count) of $script:RunLockMaxShared slots)" } elseif ($s.LiveBuilds.Count) { 'BUILD only' } else { 'free' }
    if ($mode -ne 'EXCLUSIVE' -and $s.LiveBuilds.Count -and $s.LiveSlots.Count) { $mode += " + BUILD ($($s.LiveBuilds.Count) of $script:RunLockMaxBuild)" }
"""))

ps.append(("""    } elseif ($s.LiveSlots.Count) { 'lock: held SHARED (see slots)' }
""", """    } elseif ($s.LiveSlots.Count) { 'lock: held SHARED (see slots)' }
    elseif ($s.LiveBuilds.Count) { 'lock: held by BUILD holder(s) only (see build slots)' }
"""))

ps.append(("""    "queue: $($s.Tickets.Count)"
    foreach ($t in $s.Tickets) {""", """    "build slots (max $script:RunLockMaxBuild; coexist with shared holders, exclude exclusive ones):"
    foreach ($bl in $s.BuildSlots) {
        if ($bl.Live) {
            $since = ConvertTo-RunLockDate $bl.Rec.acquired
            $for = if ($since) { " (for $(Format-RunLockDuration ((Get-Date) - $since).TotalSeconds))" } else { '' }
            "  build$($bl.Slot): '$($bl.Rec.name)' pid $($bl.Rec.pid) since $(Format-RunLockTime $bl.Rec.acquired)$for"
        }
        elseif ($bl.Rec) { "  build$($bl.Slot): STALE '$($bl.Rec.name)' pid $($bl.Rec.pid) (owner gone; taken over by the next build waiter)" }
        else { "  build$($bl.Slot): free" }
    }
    "queue: $($s.Tickets.Count)"
    foreach ($t in $s.Tickets) {"""))

# 9. report
ps.append(("""        foreach ($sl in $st.LiveSlots) { $liveRecs += [pscustomobject]@{ Rec = $sl.Rec; Mode = 'shared'; Slot = $sl.Slot } }
""", """        foreach ($sl in $st.LiveSlots) { $liveRecs += [pscustomobject]@{ Rec = $sl.Rec; Mode = 'shared'; Slot = $sl.Slot } }
        foreach ($bl in $st.LiveBuilds) { $liveRecs += [pscustomobject]@{ Rec = $bl.Rec; Mode = 'build'; Slot = (10 + $bl.Slot) } }
"""))

ps.append(("""            param($c) if ([int]$c['exclusive'] -gt 0) { 'exclusive' } elseif ([int]$c['shared'] -gt 0) { "shared $([Math]::Min(3, [int]$c['shared']))" } else { 'free' } }
""", """            param($c) if ([int]$c['exclusive'] -gt 0) { 'exclusive' } elseif ([int]$c['shared'] -gt 0) { "shared $([Math]::Min(3, [int]$c['shared']))" } elseif ([int]$c['build'] -gt 0) { 'build only' } else { 'free' } }
"""))

ps.append(("""(Format-RunLockShares $acc @('free', 'exclusive', 'shared 1', 'shared 2', 'shared 3') $hSpan)""",
           """(Format-RunLockShares $acc @('free', 'exclusive', 'shared 1', 'shared 2', 'shared 3', 'build only') $hSpan)"""))

ps.append(("""        '  {0} exclusive and {1} shared holds, {2} re-entries, {3} queue timeouts{4}' -f @($winHolds | Where-Object Mode -eq 'exclusive').Count,
            @($winHolds | Where-Object Mode -eq 'shared').Count, @($wr""", """        '  {0} exclusive and {1} shared holds{5}, {2} re-entries, {3} queue timeouts{4}' -f @($winHolds | Where-Object Mode -eq 'exclusive').Count,
            @($winHolds | Where-Object Mode -eq 'shared').Count, @($wr"""))

ps.append(("""            $(if ($est) { "; $est without a release line (end estimated)" } else { '' })
""", """            $(if ($est) { "; $est without a release line (end estimated)" } else { '' }),
            $(if (@($winHolds | Where-Object Mode -eq 'build').Count) { " (+ $(@($winHolds | Where-Object Mode -eq 'build').Count) build)" } else { '' })
"""))

# 10. CLI
ps.append(("""                    '-Exclusive' { $runMode = 'Exclusive'; $i += 1 }
""", """                    '-Exclusive' { $runMode = 'Exclusive'; $i += 1 }
                    '-Build' { $runMode = 'Build'; $i += 1 }
"""))
ps.append(("usage: runlock.ps1 run [-Shared] [-Name NAME]", "usage: runlock.ps1 run [-Shared | -Build] [-Name NAME]"))

apply(PS, ps, 'BUILD class (Session 82, harness builder')

# ---------------------------------------------------------------- python
PY = os.path.join(REPO, 'tools', 'harness', 'runlock.py')
py = []
py.append(("""LOCK_JSON = os.path.join(LOCK_DIR, 'lock.json')   # exclusive holder; shared holders: slot<N>.json next to it
""", """LOCK_JSON = os.path.join(LOCK_DIR, 'lock.json')   # exclusive holder; shared holders: slot<N>.json next to it
# (Session 82: BUILD class holders: build<N>.json; they coexist with shared holders and exclude exclusive ones, see
# the design note in tools/runlock.ps1.)
"""))
py.append(('''def held_by_us():
    """\'exclusive\' or \'shared\' when this process inherited the token of a live holder (exclusive lock.json, or a
    shared slot file); \'\' otherwise (falsy, so old `if held_by_us():` callers keep working)."""''',
           '''def read_build_slots():
    """{build slot number: record} of the BUILD holders' build<N>.json files that carry a record (live or not)."""
    out = {}
    try:
        names = os.listdir(LOCK_DIR)
    except OSError:
        return out
    for n in names:
        if n.startswith('build') and n.endswith('.json') and n[5:-5].isdigit():
            rec = _read_rec(os.path.join(LOCK_DIR, n))
            if rec:
                out[int(n[5:-5])] = rec
    return out


def held_by_us():
    """\'exclusive\', \'shared\' or \'build\' when this process inherited the token of a live holder (exclusive lock.json,
    a shared slot file or a build slot file); \'\' otherwise (falsy, so old `if held_by_us():` callers keep working)."""'''))
py.append(("""        if rec.get('token') == tok and _owner_alive(rec):
            return 'shared'
    return ''
""", """        if rec.get('token') == tok and _owner_alive(rec):
            return 'shared'
    for rec in read_build_slots().values():
        if rec.get('token') == tok and _owner_alive(rec):
            return 'build'
    return ''
"""))
py.append(("""def ensure_lock(name, timeout_minutes=240, shared=False):
    held = held_by_us()
    if held == 'exclusive' or (held == 'shared' and shared):
        return
    if held == 'shared':
        sys.stderr.write('[runlock] %s needs the EXCLUSIVE run lock but runs inside a shared hold (its parent took a '
                         'shared slot); it would wait on its own parent forever. Take the exclusive lock in the parent '
                         '(e.g. run_list -Exclusive).\\n' % name)
        sys.exit(1)
""", """def ensure_lock(name, timeout_minutes=240, shared=False, build=False):
    held = held_by_us()
    if held == 'exclusive' or (held == 'shared' and (shared or build)) or (held == 'build' and (build or shared)):
        return
    if held in ('shared', 'build'):
        sys.stderr.write('[runlock] %s needs the EXCLUSIVE run lock but runs inside a %s hold (its parent took a '
                         '%s slot); it would wait on its own parent forever. Take the exclusive lock in the parent '
                         '(e.g. run_list -Exclusive).\\n' % (name, held, held))
        sys.exit(1)
"""))
py.append(("""+ (['-Shared'] if shared else []) + ['--', sys.executable] + args""",
           """+ (['-Build'] if build else ['-Shared'] if shared else []) + ['--', sys.executable] + args"""))
apply(PY, py, 'read_build_slots')
