# Run lock + FIFO queue for anything that launches the game or rebuilds/restages its files (Session 8, tasks 1 and 0).
# Two modes (Session 8, task 0 "shared mode"):
#   EXCLUSIVE  one holder, no shared holder, no wwe2k14 process running. Builds, codegen, SDK builds, benchmarks
#              (run_bench), regress, bisect, sdk_rebase, `runlock.ps1 run` and `run_list -Exclusive` (timed runs).
#              This is the default of Enter-RunLock / `runlock.ps1 run` / ensure_lock(), exactly today's behaviour.
#   SHARED     up to $script:RunLockMaxShared (3) holders at once, never while an exclusive holder exists, at most
#              3 wwe2k14 processes in total, and never next to a game that no live shared holder started (a manual
#              session, or a run that outlived its launcher, blocks shared takers exactly as it blocks exclusive
#              ones). The default of a direct run_list / run_and_triage call that is not inside an exclusive hold,
#              when the run has its own --user_data_root and the exe logs "run log number" (Test-RunLockExeSharedSafe);
#              otherwise those callers take it exclusively. Ordinary (non-benchmark) game runs only: perf numbers
#              from a shared run are not comparable.
#
# As a library (dot-source it):
#   . C:\Users\Jarrod\wwe2k14-recomp\tools\runlock.ps1
#   $h = Enter-RunLock -Name 'run_and_triage selftest' [-Mode Shared]   # waits in the queue; throws on timeout
#   try { ... $h.Mode ('Exclusive'|'Shared'), $h.Slot (0 | 1..3) ... } finally { Exit-RunLock $h }
#   $l = Enter-RunLockLaunch; try { Start-Process ...; wait for the new log } finally { Exit-RunLockLaunch $l }
#   Known limit: liveness is the holder process only. If the holder is killed, a game or build it started keeps running
#   unguarded (a game still blocks the queue through the "wwe2k14 running" rule below; a build does not). A job the
#   holder joins does not help (under the agent harness children break away from it, tested), but a process the holder
#   starts can be tied to it explicitly: Add-RunLockKillOnExit $proc (Windows kills $proc when this process exits or is
#   killed). Only for unattended callers that never leave that process running on purpose (run_bench does it).
#
# As a command:
#   pwsh -NoProfile -File tools\runlock.ps1 status                       mode, holder, shared slots, queue, games
#   pwsh -NoProfile -File tools\runlock.ps1 clear-stale                  remove dead holders' records and dead tickets
#   pwsh -NoProfile -File tools\runlock.ps1 run [-Shared] -Name NAME -- CMD ARGS   run CMD while holding the lock
#                                                                         (exclusive unless -Shared; exit code passes
#                                                                         through; 127 = CMD not found; at release it
#                                                                         prints how long it held and how many waited)
#   pwsh -NoProfile -File tools\runlock.ps1 report [-Since HH:mm | -Hours N] [-Until HH:mm] [-Top N] [-ByName] [-NoLogs]
#                                                                         lane utilisation, waits per tool, longest
#                                                                         exclusive holds, runs per hour (docs\harness\lanes.md)
#
# History (Session 8 extra 3, task L2): every acquire, release, re-entry (Enter-RunLock's; a Python ensure_lock() that
# finds its process already inside a hold returns without a line) and queue timeout appends one line to
# out\harness\runlock\history.tsv (columns: $script:RunLockHistoryColumns). Best effort and off the lock path: written
# after the take / after the release, through an append-only handle (atomic per line across processes), three quick
# tries, then dropped; a history failure never fails or delays a lock operation by more than ~60 ms. Hold discipline
# (warn, never block): an EXCLUSIVE hold longer than $script:RunLockLongHoldMinutes (10) gets a one-line warning in
# `status`, in `report`, and from Exit-RunLock at its release.
#
# How it works:
#   lock.json is a reader/writer lock made of Windows share modes. The EXCLUSIVE holder keeps it open read/write
#   (others may only read) and writes its record into it. Each SHARED holder keeps it open READ-ONLY sharing read only:
#   any number of shared handles coexist, but nobody can open it for writing while one is open, so an exclusive taker
#   (new or OLD copy of this script, which only knows lock.json) simply finds it busy, and a shared taker cannot open
#   it while an exclusive holder has it. Shared holders' records live in slot1.json..slot3.json, each held open
#   read/write by its holder (that handle is what limits the count); an empty slot file reads as free.
#   When a holder dies, Windows closes its handles, and the next waiter sees a dead, empty or unreadable record and
#   overwrites it in place. Nobody deletes a lock another process may hold. An exclusive release empties lock.json,
#   closes it and deletes it (an empty lock.json left behind reads as free). The records carry token, pid + process
#   start time, name, mode, slot for status and re-entry.
#   Queue: queue\<utc ticks>_<pid>.json tickets (with the mode; an old copy's ticket has none = exclusive), written to
#   a .tmp and renamed (never seen half-written). FIFO: an exclusive waiter goes only when it is the first live ticket;
#   a shared waiter goes when no exclusive ticket is ahead of it and fewer shared tickets are ahead of it than there are
#   free slots. So shared waiters never overtake an exclusive one (no starvation of builds/benchmarks), and shared
#   waiters ahead of it run together. A waiter whose ticket vanished writes it again (same place in line).
#   Exclusive: a free lock still counts as busy while any wwe2k14.exe runs (a run that outlived its launcher, or a
#   manual launch). Shared: fewer than 3 live shared holders and games, and no game that no live shared holder started
#   (process ancestry: parent chain up to a holder's pid).
#   Old copies of this script still running somewhere (they only know lock.json): their exclusive takes are blocked by
#   the shared holders' read handles and by any running game; their tickets count as exclusive here.
#
# Launch window (Enter-RunLockLaunch): ReXGlue numbers wwe2k14_NNN.log at startup by scanning the logs folder, and the
# app names frames_/timeline_NNN.csv and captures NNN_* from it, so two instances starting together could pick the same
# number. run_and_triage holds launch.lock (opened with no sharing) from Start-Process until its own new log file exists
# and (exes that log it) its "run log number: NNN (pid P)" line is in it, so the app's one-time scan for the newest log
# has happened; and always at least 3.5 s, even when the game died at once, so no two runs start within 3 s:
# tools\triage\triage.py's event-log matching relies on that. The log that appeared inside its own window is its log,
# never "the newest log"; after the run both NNN (= the file's number) and P (= the launched pid) are checked. A dead
# holder's handle closes by itself.
#
# Re-entrant: the holder exports WWE2K14_RUNLOCK_TOKEN; any child process that sees a live holder's token in its
# environment proceeds without queueing: a child of an exclusive holder re-enters exclusively (regress -> run_list ->
# run_and_triage stay exclusive), a child of a shared holder re-enters that shared slot. Asking for EXCLUSIVE inside a
# shared hold throws (it would wait on its own parent forever). Python tools: tools\harness\runlock.py.

$script:RunLockDir = 'C:\Users\Jarrod\wwe2k14-recomp\out\harness\runlock'
$script:RunLockFile = Join-Path $script:RunLockDir 'lock.json'
$script:RunLockQueue = Join-Path $script:RunLockDir 'queue'
$script:RunLockLaunchFile = Join-Path $script:RunLockDir 'launch.lock'
$script:RunLockGameNames = @('wwe2k14')
$script:RunLockMaxShared = 3   # THE setting: shared holders at once, and wwe2k14 processes at once in shared mode
$script:RunLockHistoryFile = Join-Path $script:RunLockDir 'history.tsv'
$script:RunLockHistoryColumns = @('time', 'event', 'mode', 'slot', 'name', 'pid', 'waited_s', 'held_s', 'queue', 'games', 'token', 'note')
$script:RunLockLongHoldMinutes = 10   # hold discipline: exclusive holds longer than this get a warning (never blocked)
$script:RunLockRepo = 'C:\Users\Jarrod\wwe2k14-recomp'   # report: run logs are read from <repo>\out\build\*\logs
if ($null -eq $script:RunLockMsgToStderr) { $script:RunLockMsgToStderr = $false }

function Write-RunLockMsg([string]$Message) {
    if ($script:RunLockMsgToStderr) { [Console]::Error.WriteLine($Message) } else { Write-Host $Message }
}

# Local time as a plain string (ConvertFrom-Json turns ISO 'T' strings into DateTime, which then print US-style).
function Get-RunLockNow { (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') }
function Format-RunLockTime($Value) {
    if ($Value -is [datetime]) { return $Value.ToString('yyyy-MM-dd HH:mm:ss') }
    if ($Value) { return "$Value" }
    return '?'
}

# A record's 'acquired' / 'queued' value as a local DateTime ($null if missing or unreadable). Records hold
# 'yyyy-MM-dd HH:mm:ss' strings; ConvertFrom-Json may already have made a DateTime of them.
function ConvertTo-RunLockDate($Value) {
    if ($Value -is [datetime]) { return $Value }
    $d = [datetime]::MinValue
    if ($Value -and [datetime]::TryParseExact("$Value", 'yyyy-MM-dd HH:mm:ss', [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::None, [ref]$d)) { return $d }
    return $null
}

function Format-RunLockDuration([double]$Seconds) {
    if ($Seconds -lt 0) { $Seconds = 0 }
    if ($Seconds -lt 60) { return ('{0:N0} s' -f $Seconds) }
    if ($Seconds -lt 3600) { return ('{0} min {1:00} s' -f [Math]::Floor($Seconds / 60), [Math]::Floor($Seconds % 60)) }
    return ('{0} h {1:00} min' -f [Math]::Floor($Seconds / 3600), [Math]::Floor(($Seconds % 3600) / 60))
}

# Appends one line to history.tsv. Append-only handle (FILE_APPEND_DATA without FILE_WRITE_DATA): Windows writes each
# write at the current end of the file, whatever offset .NET passes, so lines from concurrent processes neither
# overwrite nor interleave (scratch test: 16 processes x 1000 lines, all 16000 intact; with a plain FileMode.Append
# handle, 8 processes x 500 lines, only 2809 of 4000 came through intact: ~27% lost, ~3% mangled). One unbuffered
# write per line. Never throws; three tries ~20 ms apart (the file opened without sharing by an editor), then the line
# is dropped.
function Write-RunLockHistory {
    param([string]$Kind, [string]$Mode = '', [int]$Slot = 0, [string]$Name = '', $Waited = $null, $Held = $null,
          $Queue = $null, $Games = $null, [string]$Token = '', [string]$Note = '')
    try {
        $num = { param($v) if ($null -eq $v -or "$v" -eq '') { '' } else { ([double]$v).ToString('0.0', [System.Globalization.CultureInfo]::InvariantCulture) } }
        $txt = { param($v, $max) $t = ("$v" -replace '[\t\r\n]+', ' ').Trim(); if ($t.Length -gt $max) { $t.Substring(0, $max) } else { $t } }
        $f = @((Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture), $Kind,
            $Mode.ToLowerInvariant(), $Slot, (& $txt $Name 300), $PID, (& $num $Waited), (& $num $Held), "$Queue", "$Games",
            $Token.Substring(0, [Math]::Min(8, $Token.Length)), (& $txt $Note 300))
        $line = ($f -join "`t") + "`n"
        $path = $script:RunLockHistoryFile
        if (-not [System.IO.File]::Exists($path)) { $line = ($script:RunLockHistoryColumns -join "`t") + "`n" + $line }   # a rare double header is skipped by the reader
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($line)
        for ($k = 0; $k -lt 3; $k++) {
            try {
                $fs = [System.IO.FileSystemAclExtensions]::Create([System.IO.FileInfo]::new($path), [System.IO.FileMode]::Append,
                    [System.Security.AccessControl.FileSystemRights]::AppendData, [System.IO.FileShare]'ReadWrite, Delete', 1,
                    [System.IO.FileOptions]::None, $null)
                try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
                return
            } catch { Start-Sleep -Milliseconds 20 }
        }
    } catch { }
}

# Live tickets in the queue right now (count, and how long the oldest has waited, from its file name's UTC ticks).
function Get-RunLockQueueInfo {
    $n = 0; $oldest = 0.0
    try {
        foreach ($t in @(Get-RunLockTickets)) {
            if (-not (Test-RunLockOwnerAlive $t)) { continue }
            $n++
            $ticks = [int64]0
            if ([int64]::TryParse(([System.IO.Path]::GetFileName("$($t.file)") -split '_')[0], [ref]$ticks) -and $ticks -gt 0) {
                $w = ([DateTime]::UtcNow - [DateTime]::new($ticks, [DateTimeKind]::Utc)).TotalSeconds
                if ($w -gt $oldest) { $oldest = $w }
            }
        }
    } catch { }
    [pscustomobject]@{ Count = $n; OldestSeconds = $oldest }
}

# Process start time as UTC ticks (an integer: ConvertFrom-Json turns ISO date strings into DateTime objects, which
# then never compare equal to the string). $null = not running, 0 = running but start time unreadable.
function Get-RunLockProcStart([int]$ProcId) {
    $p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
    if (-not $p) { return $null }
    try { return [int64]$p.StartTime.ToUniversalTime().Ticks } catch { return [int64]0 }
}

function Test-RunLockOwnerAlive($rec) {
    if (-not $rec -or -not $rec.pid) { return $false }
    $start = Get-RunLockProcStart ([int]$rec.pid)
    if ($null -eq $start) { return $false }
    $recStart = [int64]0
    if ($rec.pid_start) { try { $recStart = [int64]$rec.pid_start } catch { $recStart = 0 } }
    if ($start -ne 0 -and $recStart -ne 0 -and $start -ne $recStart) { return $false }   # pid reused by another process
    return $true
}

# A record naming this very process that this process does not hold any more (an older copy's release failed, or the
# token was lost): taking it over is safe, waiting on it would wait on ourselves.
function Test-RunLockOwnLeftover($rec) {
    if (-not $rec -or [int]$rec.pid -ne $PID) { return $false }
    $held = $global:WWE2K14RunLockHeld
    return -not ($held -and $held.Stream -and $held.Token -eq $rec.token) -and ($env:WWE2K14_RUNLOCK_TOKEN -ne $rec.token)
}

# Live and not our own leftover = really held by someone.
function Test-RunLockRecHeld($rec) { [bool]($rec -and (Test-RunLockOwnerAlive $rec) -and -not (Test-RunLockOwnLeftover $rec)) }

# Reads a lock or ticket file without getting in anyone's way: shares read, write AND delete (a reader that did not
# share delete made a holder's release fail silently, see the review). $null = missing or unreadable.
function Read-RunLockText([string]$path) {
    for ($i = 0; $i -lt 5; $i++) {
        try {
            $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
            try { return ([System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
        } catch [System.IO.FileNotFoundException], [System.IO.DirectoryNotFoundException] { return $null }
        catch { Start-Sleep -Milliseconds 50 }   # opened exclusively by an older copy right now, or delete pending
    }
    return $null
}

function ConvertFrom-RunLockText([string]$text) {
    if (-not $text) { return $null }
    $text = $text.TrimStart([char]0xFEFF).Trim()
    if (-not $text) { return $null }
    try { $o = $text | ConvertFrom-Json -ErrorAction Stop } catch { return $null }
    if ($o -and $o.token) { return $o }
    return $null
}

function Read-RunLockJson([string]$path) { ConvertFrom-RunLockText (Read-RunLockText $path) }

function Remove-RunLockFileQuiet([string]$path) { try { [System.IO.File]::Delete($path) } catch { } }

# Opens lock.json the way an EXCLUSIVE holder does (read/write, others may only read). $null = someone has it open
# (exclusive holder, shared holders, a taker checking it right now), delete pending, or missing with -Mode Open.
function Open-RunLockForTake([System.IO.FileMode]$Mode = [System.IO.FileMode]::OpenOrCreate) {
    try {
        [void][System.IO.Directory]::CreateDirectory($script:RunLockDir)
        return [System.IO.File]::Open($script:RunLockFile, $Mode, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
    } catch { return $null }
}

# Opens lock.json the way a SHARED holder does (read-only, others may only read): coexists with other shared holders,
# fails while an exclusive holder (or taker) has it open for writing. $null = not possible right now.
function Open-RunLockForShare {
    try {
        [void][System.IO.Directory]::CreateDirectory($script:RunLockDir)
        return [System.IO.File]::Open($script:RunLockFile, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    } catch { return $null }
}

function Get-RunLockSlotFile([int]$Slot) { Join-Path $script:RunLockDir "slot$Slot.json" }

# A slot file opened the way its holder does (read/write, others may only read). $null = held, or being taken.
function Open-RunLockSlot([int]$Slot) {
    try { return [System.IO.File]::Open((Get-RunLockSlotFile $Slot), [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read) }
    catch { return $null }
}

function Read-RunLockStream($fs) {
    $fs.Position = 0
    $ms = [System.IO.MemoryStream]::new()
    $fs.CopyTo($ms)
    $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
    $rec = ConvertFrom-RunLockText $text
    [pscustomobject]@{ Text = $text; Rec = $rec; Live = [bool]($rec -and (Test-RunLockOwnerAlive $rec)) }
}

function Write-RunLockStream($fs, $rec) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes(($rec | ConvertTo-Json -Compress))
    $fs.SetLength(0); $fs.Position = 0
    $fs.Write($bytes, 0, $bytes.Length); $fs.Flush($true)
}

function Get-RunLockGameProcs { @(Get-Process -Name $script:RunLockGameNames -ErrorAction SilentlyContinue) }

# Games that no live holder (or a descendant of one) started: a run that outlived its launcher, or a manual launch.
# Conservative: if the process table can't be read, every game counts as unowned.
function Get-RunLockUnownedGames($Games, [int[]]$HolderPids) {
    $Games = @($Games)
    if (-not $Games.Count) { return @() }
    if (-not $HolderPids -or -not $HolderPids.Count) { return $Games }
    $parent = @{}
    try { foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId -ErrorAction Stop)) { $parent[[int]$p.ProcessId] = [int]$p.ParentProcessId } }
    catch { return $Games }
    @($Games | Where-Object {
        $cur = [int]$_.Id; $owned = $false
        for ($k = 0; $k -lt 12 -and $parent.ContainsKey($cur); $k++) {
            $cur = $parent[$cur]
            if ($HolderPids -contains $cur) { $owned = $true; break }
        }
        -not $owned })
}

# Fixer (review finding 5): may this exe run SHARED? Only builds whose app takes its log number once and logs it
# ("[wwe2k14] run log number: NNN (pid P)", src\wwe2k14_app.h OnPostInitLogging, first in the 16:30 release build of
# 2026-10-09): older ones rescan for the newest log each time they name a capture / frames / timeline / coverage file,
# so concurrent runs of them write each other's files. Checked by looking for the format string in the exe (about
# 0.1 s for 120 MB). $false also when the exe can't be read (conservative: the caller then runs exclusive).
function Test-RunLockExeSharedSafe([string]$Exe) {
    $needle = 'run log number: {'
    try {
        $fs = [System.IO.File]::Open($Exe, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
    } catch { return $false }
    try {
        $chunk = 8MB; $keepN = $needle.Length - 1
        $buf = [byte[]]::new($chunk + $needle.Length)
        $carry = 0
        while ($true) {
            $n = $fs.Read($buf, $carry, $chunk)
            if ($n -le 0) { return $false }
            $len = $carry + $n
            if ([System.Text.Encoding]::Latin1.GetString($buf, 0, $len).IndexOf($needle, [System.StringComparison]::Ordinal) -ge 0) { return $true }
            $carry = [Math]::Min($keepN, $len)
            [Array]::Copy($buf, $len - $carry, $buf, 0, $carry)
        }
    } catch { return $false } finally { $fs.Dispose() }
}

# Fixer (review finding 6): running games whose command line names this user data folder (--user_data_root=PATH).
function Get-RunLockGamesOnUserData([string]$UserDataRoot) {
    $rx = '--user_data_root="?' + [regex]::Escape($UserDataRoot.TrimEnd('\')) + '\\?(?=["\s]|$)'
    $filter = ($script:RunLockGameNames | ForEach-Object { "Name='$_.exe'" }) -join ' OR '
    try {
        @(Get-CimInstance Win32_Process -Filter $filter -Property ProcessId, CommandLine -ErrorAction Stop |
            Where-Object { "$($_.CommandLine)" -match $rx } | ForEach-Object { [int]$_.ProcessId })
    } catch { @(Get-RunLockGameProcs | ForEach-Object { [int]$_.Id }) }   # conservative: every game counts
}

function Get-RunLockSlots {
    @(1..$script:RunLockMaxShared | ForEach-Object {
        $f = Get-RunLockSlotFile $_
        $r = Read-RunLockJson $f
        [pscustomobject]@{ Slot = $_; File = $f; Rec = $r; Live = (Test-RunLockRecHeld $r) } })
}

function Get-RunLockTicketMode($t) { if ($t -and "$($t.mode)" -eq 'shared') { 'shared' } else { 'exclusive' } }

function Get-RunLockTickets {
    if (-not (Test-Path -LiteralPath $script:RunLockQueue)) { return @() }
    @(Get-ChildItem -LiteralPath $script:RunLockQueue -Filter '*.json' -File -ErrorAction SilentlyContinue | Sort-Object Name |
        ForEach-Object { $r = Read-RunLockJson $_.FullName; if ($r) { $r | Add-Member -NotePropertyName file -NotePropertyValue $_.FullName -PassThru -Force } })
}

function Get-RunLockState {
    $exists = Test-Path -LiteralPath $script:RunLockFile
    $lock = if ($exists) { Read-RunLockJson $script:RunLockFile } else { $null }
    $slots = Get-RunLockSlots
    [pscustomobject]@{ Lock = $lock; LockFileExists = $exists; LockAlive = (Test-RunLockOwnerAlive $lock)
        Slots = $slots; LiveSlots = @($slots | Where-Object Live)
        Tickets = @(Get-RunLockTickets); Games = @(Get-RunLockGameProcs) }
}

function Write-RunLockTicket([string]$path, $rec) {
    $tmp = [System.IO.Path]::ChangeExtension($path, '.tmp')
    [System.IO.File]::WriteAllText($tmp, ($rec | ConvertTo-Json -Compress))
    [System.IO.File]::Move($tmp, $path, $true)
}

# Dead tickets (and junk that stayed unreadable for a minute) always; with -Lock also a dead holder's record or an
# empty lock.json, emptied under an exclusive handle (only if still dead then) and deleted, and dead shared-slot
# records (emptied). The waiters themselves never need the -Lock part (they take a dead lock or slot over in place);
# it is for `clear-stale` and older copies.
function Clear-RunLockStale([switch]$Quiet, [switch]$Lock) {
    foreach ($f in @(Get-ChildItem -LiteralPath $script:RunLockQueue -File -ErrorAction SilentlyContinue)) {
        $t = Read-RunLockJson $f.FullName
        $dead = if ($t) { -not (Test-RunLockOwnerAlive $t) } else { ((Get-Date) - $f.LastWriteTime).TotalSeconds -gt 60 }
        if ($dead) {
            Remove-RunLockFileQuiet $f.FullName
            if (-not $Quiet) { if ($t) { "removed stale ticket: '$($t.name)' pid $($t.pid)" } else { "removed unreadable queue file: $($f.Name)" } }
        }
    }
    if (-not $Lock) { return }
    for ($n = 1; $n -le $script:RunLockMaxShared; $n++) {
        if (-not (Test-Path -LiteralPath (Get-RunLockSlotFile $n))) { continue }
        $sf = Open-RunLockSlot $n
        if (-not $sf) { continue }
        try {
            $cur = Read-RunLockStream $sf
            if ($cur.Rec -and -not $cur.Live) {
                $sf.SetLength(0); $sf.Flush($true)
                if (-not $Quiet) { "removed stale shared slot $($n): '$($cur.Rec.name)' pid $($cur.Rec.pid) since $(Format-RunLockTime $cur.Rec.acquired)" }
            }
        } finally { $sf.Dispose() }
    }
    $fs = Open-RunLockForTake ([System.IO.FileMode]::Open)
    if (-not $fs) { return }
    $cleared = $null
    try {
        $cur = Read-RunLockStream $fs
        if (-not $cur.Live) { $fs.SetLength(0); $fs.Flush($true); $cleared = $cur }
    } finally { $fs.Dispose() }
    if ($cleared) {
        Remove-RunLockFileQuiet $script:RunLockFile
        if (-not $Quiet -and $cleared.Rec) { "removed stale lock: '$($cleared.Rec.name)' pid $($cleared.Rec.pid) since $(Format-RunLockTime $cleared.Rec.acquired)" }
    }
}
$script:RunLockJobSource = @'
using System;
using System.Runtime.InteropServices;
public static class WweRunLockJob {
    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimits {
        public long PerProcessUserTimeLimit; public long PerJobUserTimeLimit; public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize; public UIntPtr MaximumWorkingSetSize; public uint ActiveProcessLimit;
        public UIntPtr Affinity; public uint PriorityClass; public uint SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IoCounters { public ulong R, W, O, RB, WB, OB; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtendedLimits {
        public BasicLimits Basic; public IoCounters Io;
        public UIntPtr ProcessMemoryLimit; public UIntPtr JobMemoryLimit; public UIntPtr PeakProcessMemoryUsed; public UIntPtr PeakJobMemoryUsed;
    }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObjectW(IntPtr attrs, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    static IntPtr job = IntPtr.Zero;
    // One kill-on-close job per process; its handle is never closed by us, so it closes when this process exits or is
    // killed, and Windows then kills every process assigned to it.
    public static string Assign(IntPtr process) {
        if (job == IntPtr.Zero) {
            IntPtr j = CreateJobObjectW(IntPtr.Zero, null);
            if (j == IntPtr.Zero) return "CreateJobObject failed, error " + Marshal.GetLastWin32Error();
            ExtendedLimits info = new ExtendedLimits();
            info.Basic.LimitFlags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(j, 9, ref info, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) return "SetInformationJobObject failed, error " + Marshal.GetLastWin32Error();
            job = j;
        }
        return AssignProcessToJobObject(job, process) ? "ok" : "AssignProcessToJobObject failed, error " + Marshal.GetLastWin32Error();
    }
}
'@

# Ties a process this script started to this process: Windows kills it when this process exits or is killed. Returns
# 'ok' or why not (never throws). Only for a process the caller would kill anyway once it outlives its wait.
function Add-RunLockKillOnExit($Process) {
    try {
        if (-not ('WweRunLockJob' -as [type])) { Add-Type -TypeDefinition $script:RunLockJobSource -ErrorAction Stop }
        $r = [WweRunLockJob]::Assign($Process.Handle)
    } catch { $r = $_.Exception.Message }
    if ($r -ne 'ok') { Write-RunLockMsg "[runlock] warning: pid $($Process.Id) not tied to this process ($r)" }
    return $r
}

# The launch window: launch.lock opened with no sharing (a dead holder's handle closes by itself). Returns the open
# stream, or $null after -TimeoutSeconds (with a warning; the caller then launches unserialized).
function Enter-RunLockLaunch([double]$TimeoutSeconds = 300, [string]$Name = '') {
    [void][System.IO.Directory]::CreateDirectory($script:RunLockDir)
    $until = (Get-Date).AddSeconds($TimeoutSeconds)
    $said = $false
    while ($true) {
        try { return [System.IO.File]::Open($script:RunLockLaunchFile, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) }
        catch { }
        if ((Get-Date) -gt $until) { Write-RunLockMsg "[runlock] warning: launch window not free after $TimeoutSeconds s; launching '$Name' unserialized"; return $null }
        if (-not $said) { Write-RunLockMsg "[runlock] '$Name' waiting for the launch window (another instance is starting)"; $said = $true }
        Start-Sleep -Milliseconds 100
    }
}
function Exit-RunLockLaunch($Stream) { if ($Stream) { try { $Stream.Dispose() } catch { } } }

function Enter-RunLock {
    param([string]$Name = 'unnamed', [double]$TimeoutMinutes = 240, [int]$PollMs = 2000, [switch]$Quiet,
          [ValidateSet('Exclusive', 'Shared')][string]$Mode = 'Exclusive')
    [void][System.IO.Directory]::CreateDirectory($script:RunLockQueue)
    $max = $script:RunLockMaxShared
    # Re-entry: an ancestor (or this process, further up the stack) holds the lock and passed its token down.
    $inherited = $env:WWE2K14_RUNLOCK_TOKEN
    if ($inherited) {
        $cur = Read-RunLockJson $script:RunLockFile
        if ($cur -and $cur.token -eq $inherited -and (Test-RunLockOwnerAlive $cur)) {
            if (-not $Quiet) { Write-RunLockMsg "[runlock] re-entered the exclusive lock held by '$($cur.name)' (pid $($cur.pid)) for '$Name'" }
            Write-RunLockHistory -Kind 'reenter' -Mode 'exclusive' -Slot 0 -Name $Name -Waited 0 -Token $inherited -Note "inside '$($cur.name)' pid $($cur.pid)"
            return [pscustomobject]@{ Token = $inherited; Reentrant = $true; Name = $Name; Stream = $null; LockStream = $null; Mode = 'Exclusive'; Slot = 0 }
        }
        foreach ($sl in (Get-RunLockSlots)) {
            if ($sl.Rec -and $sl.Rec.token -eq $inherited -and (Test-RunLockOwnerAlive $sl.Rec)) {
                if ($Mode -eq 'Exclusive') {
                    throw "[runlock] '$Name' asks for the EXCLUSIVE lock inside shared slot $($sl.Slot) held by '$($sl.Rec.name)' (pid $($sl.Rec.pid)): it would wait on its own parent forever. Take the exclusive lock in the parent instead (e.g. run_list -Exclusive)."
                }
                if (-not $Quiet) { Write-RunLockMsg "[runlock] re-entered shared slot $($sl.Slot) held by '$($sl.Rec.name)' (pid $($sl.Rec.pid)) for '$Name'" }
                Write-RunLockHistory -Kind 'reenter' -Mode 'shared' -Slot $sl.Slot -Name $Name -Waited 0 -Token $inherited -Note "inside '$($sl.Rec.name)' pid $($sl.Rec.pid)"
                return [pscustomobject]@{ Token = $inherited; Reentrant = $true; Name = $Name; Stream = $null; LockStream = $null; Mode = 'Shared'; Slot = $sl.Slot }
            }
        }
    }
    # A lock this process still holds but whose token is gone (Exit-RunLock never ran): release it, or we'd wait on it.
    $held = $global:WWE2K14RunLockHeld
    if ($held -and $held.Stream) {
        if (-not $Quiet) { Write-RunLockMsg "[runlock] releasing a lock this process still held ('$($held.Name)') before queueing '$Name'" }
        Exit-RunLock $held
    }
    $shared = $Mode -eq 'Shared'
    $modeWord = if ($shared) { 'shared' } else { 'exclusive' }
    $myStart = Get-RunLockProcStart $PID
    $token = [guid]::NewGuid().ToString('N')
    $ticket = Join-Path $script:RunLockQueue ('{0:D19}_{1}.json' -f [DateTime]::UtcNow.Ticks, $PID)
    $rec = [ordered]@{ token = $token; pid = $PID; pid_start = $myStart; name = $Name;
        cmd = [Environment]::CommandLine; queued = (Get-RunLockNow); mode = $modeWord }
    Write-RunLockTicket $ticket $rec
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $lastMsg = ''
    $t0 = Get-Date
    try {
        while ($true) {
            if (-not (Test-Path -LiteralPath $ticket)) {
                [void][System.IO.Directory]::CreateDirectory($script:RunLockQueue)
                Write-RunLockTicket $ticket $rec
                if (-not $Quiet) { Write-RunLockMsg "[runlock] '$Name': queue ticket was gone (folder cleaned?); wrote it again, same place in line" }
            }
            Clear-RunLockStale -Quiet
            $s = Get-RunLockState
            $tokens = @($s.Tickets | ForEach-Object token)
            $idx = [array]::IndexOf($tokens, $token)
            $ahead = if ($idx -gt 0) { @($s.Tickets[0..($idx - 1)]) } else { @() }
            $exAhead = @($ahead | Where-Object { (Get-RunLockTicketMode $_) -eq 'exclusive' })
            $blocked = Test-RunLockRecHeld $s.Lock
            $why = $null
            $go = $false
            if ($idx -lt 0) { $why = 'own ticket not listed yet' }
            elseif ($blocked) { $why = "exclusive lock held by '$($s.Lock.name)' (pid $($s.Lock.pid), since $(Format-RunLockTime $s.Lock.acquired))" }
            elseif (-not $shared) {
                if ($idx -ne 0) { $why = 'waiting for earlier tickets' }
                elseif ($s.LiveSlots.Count) { $why = "$($s.LiveSlots.Count) shared holder(s) running: " + (($s.LiveSlots | ForEach-Object { "'$($_.Rec.name)' pid $($_.Rec.pid)" }) -join ', ') }
                elseif ($s.Games.Count) { $why = "free, but wwe2k14 is running (pid $(($s.Games | ForEach-Object Id) -join ','))" }
                else { $go = $true }
            } else {
                if ($exAhead.Count) { $why = "an exclusive waiter is ahead ('$($exAhead[0].name)'); not overtaking it" }
                elseif ($s.LiveSlots.Count + $idx -ge $max) { $why = "all $max shared slots busy or claimed by earlier tickets ($($s.LiveSlots.Count) held)" }
                elseif ($s.Games.Count -ge $max) { $why = "$($s.Games.Count) wwe2k14 processes running (max $max)" }
                else {
                    # Fixer (review finding 2): a game no live shared holder started (Jarrod's own play session, a
                    # double-clicked exe, an interactive run_and_triage that has ended, a run that outlived its
                    # launcher) blocks shared runs too, exactly as it blocks exclusive ones: no agent run starts next
                    # to a manual session (GPU contention, extra "newest" logs while he is watched live).
                    $holderPids = @($s.LiveSlots | ForEach-Object { [int]$_.Rec.pid })
                    $unowned = @(Get-RunLockUnownedGames $s.Games $holderPids)
                    if ($unowned.Count) { $why = "wwe2k14 running that no shared holder started (pid $(($unowned | ForEach-Object Id) -join ',')): a manual session or a run that outlived its launcher; waiting for it to exit" }
                    else { $go = $true }
                }
            }
            if ($go -and -not $shared) {
                $fs = Open-RunLockForTake
                if ($fs) {
                    $keep = $false; $took = $null
                    try {
                        $cur = Read-RunLockStream $fs
                        if (-not ($cur.Live -and -not (Test-RunLockOwnLeftover $cur.Rec)) -and (Get-RunLockGameProcs).Count -eq 0 -and -not @((Get-RunLockSlots) | Where-Object Live).Count) {
                            $rec.acquired = Get-RunLockNow
                            Write-RunLockStream $fs $rec
                            $keep = $true; $took = $cur.Rec
                        }
                    } catch {
                        Write-RunLockMsg "[runlock] '$Name': could not write lock.json ($($_.Exception.Message)); still waiting"
                        try { $fs.SetLength(0) } catch { }
                    } finally { if (-not $keep) { $fs.Dispose() } }
                    if ($keep) {
                        $env:WWE2K14_RUNLOCK_TOKEN = $token
                        $now = Get-Date
                        $h = [pscustomobject]@{ Token = $token; Reentrant = $false; Name = $Name; Stream = $fs; LockStream = $null; Mode = 'Exclusive'; Slot = 0
                            AcquiredAt = $now; WaitedSeconds = ($now - $t0).TotalSeconds }
                        $global:WWE2K14RunLockHeld = $h
                        if (-not $Quiet) {
                            if ($took) { Write-RunLockMsg "[runlock] took over a stale lock from '$($took.name)' (pid $($took.pid), gone)" }
                            Write-RunLockMsg ("[runlock] acquired EXCLUSIVE for '$Name' after {0:N0} s" -f $h.WaitedSeconds)
                        }
                        Write-RunLockHistory -Kind 'acquire' -Mode 'exclusive' -Slot 0 -Name $Name -Waited $h.WaitedSeconds `
                            -Queue ([Math]::Max(0, $s.Tickets.Count - 1)) -Games $s.Games.Count -Token $token `
                            -Note $(if ($took) { "took over a stale lock from '$($took.name)' pid $($took.pid)" } else { '' })
                        return $h
                    }
                }
                $why = 'lock.json is busy (being taken or released, or a shared holder is starting/finishing)'
            } elseif ($go) {
                $got = $null; $tookSlot = $null
                for ($n = 1; $n -le $max -and -not $got; $n++) {
                    $sf = Open-RunLockSlot $n
                    if (-not $sf) { continue }
                    $keep = $false; $lf = $null
                    try {
                        $cur = Read-RunLockStream $sf
                        if ($cur.Live -and -not (Test-RunLockOwnLeftover $cur.Rec)) { continue }   # a live holder that lost its handle?
                        $lf = Open-RunLockForShare
                        if (-not $lf) { $why = 'lock.json is busy (an exclusive holder is taking or releasing it)'; break }
                        $lcur = $null
                        try { $lcur = Read-RunLockJson $script:RunLockFile } catch { }
                        if (Test-RunLockRecHeld $lcur) { $why = "exclusive lock held by '$($lcur.name)'"; break }
                        $rec.acquired = Get-RunLockNow
                        $rec.slot = $n
                        Write-RunLockStream $sf $rec
                        $keep = $true; $tookSlot = $cur.Rec
                        $got = [pscustomobject]@{ Token = $token; Reentrant = $false; Name = $Name; Stream = $sf; LockStream = $lf; Mode = 'Shared'; Slot = $n
                            AcquiredAt = (Get-Date); WaitedSeconds = ((Get-Date) - $t0).TotalSeconds }
                    } catch {
                        Write-RunLockMsg "[runlock] '$Name': could not take shared slot $n ($($_.Exception.Message)); still waiting"
                        try { $sf.SetLength(0) } catch { }
                    } finally {
                        # Fixer (review nit): a failed take must not keep its read handle on lock.json (it would
                        # block every exclusive taker until this process exits).
                        if (-not $keep) { $sf.Dispose(); if ($lf) { try { $lf.Dispose() } catch { } } }
                    }
                }
                if ($got) {
                    $env:WWE2K14_RUNLOCK_TOKEN = $token
                    $global:WWE2K14RunLockHeld = $got
                    if (-not $Quiet) {
                        if ($tookSlot) { Write-RunLockMsg "[runlock] took over shared slot $($got.Slot) from '$($tookSlot.name)' (pid $($tookSlot.pid), gone)" }
                        Write-RunLockMsg ("[runlock] acquired SHARED slot $($got.Slot) of $max for '$Name' after {0:N0} s" -f $got.WaitedSeconds)
                    }
                    Write-RunLockHistory -Kind 'acquire' -Mode 'shared' -Slot $got.Slot -Name $Name -Waited $got.WaitedSeconds `
                        -Queue ([Math]::Max(0, $s.Tickets.Count - 1)) -Games $s.Games.Count -Token $token `
                        -Note $(if ($tookSlot) { "took over slot from '$($tookSlot.name)' pid $($tookSlot.pid)" } else { '' })
                    return $got
                }
                if (-not $why) { $why = 'no shared slot file could be opened (being taken or released)' }
            }
            if ((Get-Date) -gt $deadline) {
                Write-RunLockHistory -Kind 'timeout' -Mode $modeWord -Name $Name -Waited ((Get-Date) - $t0).TotalSeconds `
                    -Queue ([Math]::Max(0, $s.Tickets.Count - 1)) -Games $s.Games.Count -Token $token -Note "$why"
                throw "[runlock] timed out after $TimeoutMinutes min waiting for '$Name' ($modeWord)"
            }
            $msg = "[runlock] '$Name' ($modeWord) waiting: $why; queue position $($idx + 1) of $($s.Tickets.Count)"
            if (-not $Quiet -and $msg -ne $lastMsg) { Write-RunLockMsg $msg; $lastMsg = $msg }
            Start-Sleep -Milliseconds $PollMs
        }
    } finally {
        Remove-RunLockFileQuiet $ticket
        Remove-RunLockFileQuiet ([System.IO.Path]::ChangeExtension($ticket, '.tmp'))
    }
}

# -Report (`runlock.ps1 run`): always print how long it held and how many waited. Without it a release prints only the
# hold-discipline warning (exclusive hold over $script:RunLockLongHoldMinutes min). Either way after the release itself.
function Exit-RunLock {
    param($Handle, [switch]$Report)
    if (-not $Handle -or $Handle.Reentrant) { return }
    $released = $false
    if ("$($Handle.Mode)" -eq 'Shared') {
        # Empty the slot while still holding it (an empty slot reads as free), close it, then drop the read handle on
        # lock.json. The slot file itself stays (deleting it could race a taker that has just opened it).
        if ($Handle.Stream) {
            try { $Handle.Stream.SetLength(0); $Handle.Stream.Flush($true) } catch { }
            try { $Handle.Stream.Dispose() } catch { }
            $Handle.Stream = $null
            $released = $true
        }
        if ($Handle.LockStream) { try { $Handle.LockStream.Dispose() } catch { }; $Handle.LockStream = $null }
    } else {
        $mine = $false
        if ($Handle.Stream) {
            # Empty it while still holding it: if the delete below can't happen, an empty lock.json reads as free.
            try { $Handle.Stream.SetLength(0); $Handle.Stream.Flush($true) } catch { }
            try { $Handle.Stream.Dispose() } catch { }
            $Handle.Stream = $null
            $mine = $true
        } else {
            $cur = Read-RunLockJson $script:RunLockFile   # no stream: already released, or a handle from an older copy
            $mine = [bool]($cur -and $cur.token -eq $Handle.Token)
        }
        if ($mine) {
            $until = (Get-Date).AddSeconds(5)
            while ($true) {
                Remove-RunLockFileQuiet $script:RunLockFile
                if (-not (Test-Path -LiteralPath $script:RunLockFile)) { break }
                $now = Read-RunLockJson $script:RunLockFile
                if (-not $now) { break }   # empty: reads as free (a shared holder may have opened it already)
                if ($now.token -ne $Handle.Token) { break }   # the next holder has already taken it over
                if ((Get-Date) -gt $until) {
                    Write-RunLockMsg "[runlock] warning: lock.json still carries our record after release (something keeps it open without sharing delete); stale once this process exits"
                    break
                }
                Start-Sleep -Milliseconds 50
            }
        }
        $released = $mine
    }
    if ($env:WWE2K14_RUNLOCK_TOKEN -eq $Handle.Token) { Remove-Item Env:WWE2K14_RUNLOCK_TOKEN -ErrorAction SilentlyContinue }
    if ($global:WWE2K14RunLockHeld -and $global:WWE2K14RunLockHeld.Token -eq $Handle.Token) { $global:WWE2K14RunLockHeld = $null }
    if (-not $released) { return }
    # History + hold discipline, after the lock is free (never throws).
    try {
        $exclusive = "$($Handle.Mode)" -ne 'Shared'
        $heldS = $null
        if ($Handle.PSObject.Properties['AcquiredAt'] -and $Handle.AcquiredAt) { $heldS = ((Get-Date) - $Handle.AcquiredAt).TotalSeconds }
        $q = Get-RunLockQueueInfo
        $games = @(Get-RunLockGameProcs).Count
        Write-RunLockHistory -Kind 'release' -Mode $(if ($exclusive) { 'exclusive' } else { 'shared' }) -Slot ([int]$Handle.Slot) `
            -Name $Handle.Name -Held $heldS -Queue $q.Count -Games $games -Token $Handle.Token `
            -Note $(if ($q.Count) { 'oldest waiter queued {0:N0} s ago' -f $q.OldestSeconds } else { '' })
        $long = $exclusive -and $null -ne $heldS -and $heldS -gt $script:RunLockLongHoldMinutes * 60
        if ($Report -or $long) {
            $what = if ($exclusive) { 'EXCLUSIVE' } else { "SHARED slot $($Handle.Slot)" }
            $dur = if ($null -ne $heldS) { Format-RunLockDuration $heldS } else { '?' }
            $behind = if ($q.Count) { "$($q.Count) waiting in the queue at release (oldest for $(Format-RunLockDuration $q.OldestSeconds))" } else { 'nobody waiting' }
            Write-RunLockMsg "[runlock] released $what '$($Handle.Name)' after $dur; $behind"
            if ($long) { Write-RunLockMsg "[runlock] WARNING: exclusive hold of $dur (over $($script:RunLockLongHoldMinutes) min). Keep an exclusive hold to ONE timed run; builds, analysis and untimed runs outside it (docs\harness\lanes.md)" }
        }
    } catch { }
}

function Show-RunLockStatus {
    $s = Get-RunLockState
    $mode = if (Test-RunLockOwnerAlive $s.Lock) { 'EXCLUSIVE' } elseif ($s.LiveSlots.Count) { "SHARED ($($s.LiveSlots.Count) of $script:RunLockMaxShared slots)" } else { 'free' }
    "mode: $mode"
    if ($s.Lock) {
        $state = if ($s.LockAlive) { 'held' } else { 'STALE (owner gone; the next waiter takes it over, or run clear-stale)' }
        $since = ConvertTo-RunLockDate $s.Lock.acquired
        $for = if ($since) { " (for $(Format-RunLockDuration ((Get-Date) - $since).TotalSeconds))" } else { '' }
        "lock: exclusive $state by '$($s.Lock.name)' pid $($s.Lock.pid) since $(Format-RunLockTime $s.Lock.acquired)$for"
        if ($s.LockAlive -and $since -and ((Get-Date) - $since).TotalMinutes -gt $script:RunLockLongHoldMinutes) {
            "WARNING: exclusive hold running for $(Format-RunLockDuration ((Get-Date) - $since).TotalSeconds) (over $($script:RunLockLongHoldMinutes) min); $(@($s.Tickets).Count) waiting. Keep an exclusive hold to ONE timed run (docs\harness\lanes.md)"
        }
    } elseif ($s.LiveSlots.Count) { 'lock: held SHARED (see slots)' }
    elseif ($s.LockFileExists) { 'lock: free (lock.json is empty or unreadable, which reads as free)' }
    else { 'lock: free' }
    "shared slots (max $script:RunLockMaxShared):"
    foreach ($sl in $s.Slots) {
        if ($sl.Live) {
            $since = ConvertTo-RunLockDate $sl.Rec.acquired
            $for = if ($since) { " (for $(Format-RunLockDuration ((Get-Date) - $since).TotalSeconds))" } else { '' }
            "  slot$($sl.Slot): '$($sl.Rec.name)' pid $($sl.Rec.pid) since $(Format-RunLockTime $sl.Rec.acquired)$for"
        }
        elseif ($sl.Rec) { "  slot$($sl.Slot): STALE '$($sl.Rec.name)' pid $($sl.Rec.pid) (owner gone; taken over by the next shared waiter)" }
        else { "  slot$($sl.Slot): free" }
    }
    "queue: $($s.Tickets.Count)"
    foreach ($t in $s.Tickets) {
        $q = ConvertTo-RunLockDate $t.queued
        $wait = if ($q) { " (waiting $(Format-RunLockDuration ((Get-Date) - $q).TotalSeconds))" } else { '' }
        "  [$(Get-RunLockTicketMode $t)] '$($t.name)' pid $($t.pid) queued $(Format-RunLockTime $t.queued)$wait alive=$(Test-RunLockOwnerAlive $t)"
    }
    "game processes: " + $(if ($s.Games.Count) { ($s.Games | ForEach-Object { "$($_.Id) started $($_.StartTime.ToString('yyyy-MM-dd HH:mm:ss'))" }) -join '; ' } else { 'none' })
}

# ---- report: lane utilisation from history.tsv and the run logs (Session 8 extra 3, task L2) ----

function ConvertTo-RunLockNum([string]$Text) {
    $d = 0.0
    if ($Text -and [double]::TryParse($Text, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$d)) { return $d }
    return $null
}

function Get-RunLockMedian($Values) {
    $s = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if (-not $s.Count) { return $null }
    $n = $s.Count
    if ($n % 2) { return [double]$s[($n - 1) / 2] }
    return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2
}

# history.tsv as objects, oldest first (header lines, blank and malformed lines skipped).
function Read-RunLockHistory {
    $text = Read-RunLockText $script:RunLockHistoryFile
    if (-not $text) { return @() }
    $ci = [System.Globalization.CultureInfo]::InvariantCulture
    $rows = foreach ($ln in ($text -split "`n")) {
        $f = $ln.TrimEnd("`r") -split "`t"
        if ($f.Count -lt 11 -or $f[0] -eq 'time') { continue }
        $t = [datetime]::MinValue
        if (-not [datetime]::TryParseExact($f[0], 'yyyy-MM-dd HH:mm:ss.fff', $ci, [System.Globalization.DateTimeStyles]::None, [ref]$t)) { continue }
        $slot = 0; [void][int]::TryParse($f[3], [ref]$slot)
        $procId = 0; [void][int]::TryParse($f[5], [ref]$procId)
        [pscustomobject]@{ Time = $t; Kind = $f[1]; Mode = $f[2]; Slot = $slot; Name = $f[4]; ProcId = $procId
            Waited = (ConvertTo-RunLockNum $f[6]); Held = (ConvertTo-RunLockNum $f[7]); Queue = (ConvertTo-RunLockNum $f[8])
            Games = (ConvertTo-RunLockNum $f[9]); Token = $f[10]; Note = $(if ($f.Count -gt 11) { $f[11] } else { '' }) }
    }
    @($rows | Sort-Object Time)
}

# Acquire/release pairs (same token and pid). A hold with no release line is either still held (a lock file carries its
# token now: ends now) or ended unlogged (holder killed, or released by a copy of this script without history): its end
# is estimated as the next acquire that could not have run next to it, else zero length. State says which.
# $Rows must be the WHOLE history and $Now the current time (fixer, review finding 1: pairing only the rows up to a
# window end in the past cut every hold still running at that end off from its release line); callers clip the holds
# to their window.
function Get-RunLockHolds($Rows, [datetime]$Now) {
    $open = @{}
    $holds = [System.Collections.Generic.List[object]]::new()
    $reenter = @{}
    foreach ($r in $Rows) {
        $key = "$($r.Token)|$($r.ProcId)"
        if ($r.Kind -eq 'acquire') {
            $h = [pscustomobject]@{ Start = $r.Time; End = $null; Mode = $r.Mode; Slot = $r.Slot; Name = $r.Name; ProcId = $r.ProcId
                Token = $r.Token; Waited = $r.Waited; QueueAtAcquire = $r.Queue; QueueAtRelease = $null; State = 'released'; Reentries = 0 }
            $open[$key] = $h; $holds.Add($h)
        } elseif ($r.Kind -eq 'release' -and $open.ContainsKey($key)) {
            $h = $open[$key]; $h.End = $r.Time; $h.QueueAtRelease = $r.Queue; $open.Remove($key)
        } elseif ($r.Kind -eq 'reenter') { $reenter[$r.Token] = [int]$reenter[$r.Token] + 1 }
    }
    $live = @{}
    $liveRecs = @()
    try {
        $st = Get-RunLockState
        if ($st.Lock -and $st.LockAlive) { $liveRecs += [pscustomobject]@{ Rec = $st.Lock; Mode = 'exclusive'; Slot = 0 } }
        foreach ($sl in $st.LiveSlots) { $liveRecs += [pscustomobject]@{ Rec = $sl.Rec; Mode = 'shared'; Slot = $sl.Slot } }
    } catch { }
    $acquired = @{}
    foreach ($h in $holds) { $acquired[$h.Token] = $true }
    foreach ($lr in $liveRecs) {
        $tok = "$($lr.Rec.token)"; $tok = $tok.Substring(0, [Math]::Min(8, $tok.Length))
        $live[$tok] = $true
        # Held right now by a process that loaded an older copy of this script (no acquire line): from its record.
        $since = ConvertTo-RunLockDate $lr.Rec.acquired
        if (-not $acquired.ContainsKey($tok) -and $since -and $since -lt $Now) {
            $holds.Add([pscustomobject]@{ Start = $since; End = $Now; Mode = $lr.Mode; Slot = $lr.Slot; Name = "$($lr.Rec.name)"; ProcId = [int]$lr.Rec.pid
                Token = $tok; Waited = $null; QueueAtAcquire = $null; QueueAtRelease = $null; State = 'held now, taken by an older copy (no acquire line)'; Reentries = 0 })
        }
    }
    foreach ($h in @($open.Values)) {
        if ($live.ContainsKey($h.Token)) { $h.End = $Now; $h.State = 'held now'; continue }
        $next = $Rows | Where-Object { $_.Kind -eq 'acquire' -and $_.Time -gt $h.Start -and $_.Token -ne $h.Token -and
            ($h.Mode -eq 'exclusive' -or $_.Mode -eq 'exclusive' -or $_.Slot -eq $h.Slot) } | Select-Object -First 1
        $h.End = if ($next) { $next.Time } else { $h.Start }
        $h.State = 'no release logged, end estimated'
    }
    foreach ($h in $holds) { $h.Reentries = [int]$reenter[$h.Token] }
    $holds.ToArray()
}

# First ~96 KB and last ~16 KB of a file, read without getting in a running game's way.
function Read-RunLockFileEnds([string]$Path, [int]$HeadBytes = 98304, [int]$TailBytes = 16384) {
    try { $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete') }
    catch { return $null }
    try {
        $len = $fs.Length
        $read = {
            param([int64]$at, [int]$n)
            $buf = [byte[]]::new($n); $got = 0; $fs.Position = $at
            while ($got -lt $n) { $r = $fs.Read($buf, $got, $n - $got); if ($r -le 0) { break }; $got += $r }
            [System.Text.Encoding]::UTF8.GetString($buf, 0, $got)
        }
        $head = & $read 0 ([int][Math]::Min($len, [int64]$HeadBytes))
        $tail = if ($len -gt $HeadBytes) { & $read ([Math]::Max([int64]0, $len - $TailBytes)) ([int][Math]::Min($len, [int64]$TailBytes)) } else { $head }
        return [pscustomobject]@{ Head = $head; Tail = $tail }
    } catch { return $null } finally { $fs.Dispose() }
}

# Game runs from the run logs (out\build\*\logs\wwe2k14_NNN.log) that overlap [From, To]: start = first log line,
# end = last log line (estimates: the process starts ~0.1-0.5 s before its first line; a hung or killed run's last line
# can be well before the process really ended). A run whose game is still running (the pid in its "run log number"
# line, or a wwe2k14 that started within 3 s before its first line, for older exes) ends now. Lane from the run's
# user_data_root (run_list: userdata_work_sN = shared slot N, userdata_work = exclusive).
function Get-RunLockGameRuns([datetime]$From, [datetime]$To) {
    $rxTime = [regex]'(?m)^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]'
    $rxUd = [regex]"modified: user_data_root = '([^']*)'"
    $rxRoute = [regex]"modified: wwe2k14_input_script = '([^']*)'"
    $rxPid = [regex]'\[wwe2k14\] run log number: \d+ \(pid (\d+)\)'
    $ci = [System.Globalization.CultureInfo]::InvariantCulture
    $alive = @(foreach ($p in (Get-RunLockGameProcs)) { try { [pscustomobject]@{ Id = [int]$p.Id; Start = $p.StartTime } } catch { } })
    # Build folders, exe copies of experiments (out\harness\<task>\exe, e.g. task L3's lanes snapshot) and bisect
    # worktree builds. Not out\harness\triage\*\logs (synthetic test logs). Wildcards only at fixed depths: the bisect
    # worktree has junctions into game_data, so nothing here recurses.
    $dirs = @(foreach ($pat in @('out\build\*\logs', 'out\harness\*\exe\logs', 'out\harness\bisect\wt\*\out\build\*\logs')) {
        Resolve-Path -Path (Join-Path $script:RunLockRepo $pat) -ErrorAction SilentlyContinue | ForEach-Object { $_.Path } })
    $runs = foreach ($d in $dirs) {
        foreach ($f in @(Get-ChildItem -LiteralPath $d -Filter 'wwe2k14_*.log' -File -ErrorAction SilentlyContinue)) {
            if ($f.CreationTime -gt $To.AddMinutes(1)) { continue }
            # Fixer (review): the listing's LastWriteTime of a log still open for writing (a running game) stays at
            # whatever it was when something last queried the file (often its creation), so it is re-read from the
            # file itself before a log is skipped as too old; and a log created near a running game's start is never
            # skipped (a game that has not written for a while).
            $lwt = $f.LastWriteTime
            if ($lwt -lt $From) { try { $lwt = [System.IO.File]::GetLastWriteTime($f.FullName) } catch { } }
            $nearLive = @($alive | Where-Object { [Math]::Abs(($_.Start - $f.CreationTime).TotalSeconds) -lt 120 }).Count -gt 0
            if ($lwt -lt $From -and -not $nearLive) { continue }
            $e = Read-RunLockFileEnds $f.FullName
            if (-not $e) { continue }
            $m1 = $rxTime.Match($e.Head)
            if (-not $m1.Success) { continue }
            $start = [datetime]::ParseExact($m1.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $ci)
            $ms = $rxTime.Matches($e.Tail)
            $end = if ($ms.Count) { [datetime]::ParseExact($ms[$ms.Count - 1].Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $ci) } else { $lwt }
            if ($end -lt $start) { $end = $start }
            $pm = $rxPid.Match($e.Head)
            $running = if ($pm.Success) {
                @($alive | Where-Object { $_.Id -eq [int]$pm.Groups[1].Value -and [Math]::Abs(($_.Start - $start).TotalSeconds) -lt 60 }).Count -gt 0
            } else { @($alive | Where-Object { ($start - $_.Start).TotalSeconds -ge -0.5 -and ($start - $_.Start).TotalSeconds -le 3 }).Count -gt 0 }
            if ($running) { $now = Get-Date; if ($now -gt $end) { $end = $now } }
            if ($end -lt $From -or $start -gt $To) { continue }
            $ud = $rxUd.Match($e.Head); $udv = if ($ud.Success) { $ud.Groups[1].Value.TrimEnd('\') } else { '' }
            $lane = if ($udv -match 'userdata_work_s(\d+)$') { "shared slot $($Matches[1])" } elseif ($udv -match 'userdata_work$') { 'exclusive (run_list)' }
                    elseif ($udv) { 'own user data' } else { 'default profile' }
            $rm = $rxRoute.Match($e.Head)
            [pscustomobject]@{ Run = ($f.BaseName -replace '^wwe2k14_', ''); Build = ((Split-Path $d -Parent) -replace '^.*?\\out\\', '')
                Start = $start; End = $end; Lane = $lane; Route = $(if ($rm.Success) { Split-Path $rm.Groups[1].Value -Leaf } else { '' }); Running = $running }
        }
    }
    @($runs | Sort-Object Start)
}

# Seconds of [From, To] spent in each state of a step function built from intervals {Start, End, Key}; $State maps the
# per-key counters (hashtable) to a state name.
function Measure-RunLockTimeline($Intervals, [datetime]$From, [datetime]$To, [scriptblock]$State) {
    $ev = [System.Collections.Generic.List[object]]::new()
    foreach ($iv in @($Intervals)) {
        if (-not $iv) { continue }
        $s = if ($iv.Start -lt $From) { $From } else { $iv.Start }
        $e = if ($iv.End -gt $To) { $To } else { $iv.End }
        if ($e -le $s) { continue }
        $ev.Add([pscustomobject]@{ T = $s; D = 1; K = $iv.Key }); $ev.Add([pscustomobject]@{ T = $e; D = -1; K = $iv.Key })
    }
    $count = @{}; $acc = @{}; $prev = $From
    foreach ($x in @($ev | Sort-Object T, D)) {
        if ($x.T -gt $prev) { $lvl = & $State $count; $acc[$lvl] = [double]$acc[$lvl] + ($x.T - $prev).TotalSeconds; $prev = $x.T }
        $count[$x.K] = [int]$count[$x.K] + $x.D
    }
    if ($To -gt $prev) { $lvl = & $State $count; $acc[$lvl] = [double]$acc[$lvl] + ($To - $prev).TotalSeconds }
    $acc
}

function Get-RunLockOverlapSeconds($Start, $End, [datetime]$From, [datetime]$To) {
    $s = if ($Start -lt $From) { $From } else { $Start }
    $e = if ($End -gt $To) { $To } else { $End }
    if ($e -gt $s) { ($e - $s).TotalSeconds } else { 0.0 }
}

function Format-RunLockShares($Acc, [string[]]$Order, [double]$Total) {
    (@($Order | ForEach-Object { '{0} {1:N1}%' -f $_, $(if ($Total -gt 0) { 100.0 * [double]$Acc[$_] / $Total } else { 0 }) }) -join ' | ')
}

# Fixer (review finding 3): the calling job an acquire belongs to, so the parallel workers / lanes of ONE call, which
# queue at the same time, count once. run_list -Parallel workers carry '(parent pid N)'; experiment lanes and idle
# slot holds of one batch differ only in 'lane N' / 'worker N of M' / 'hold N of M' / 'slot hold N' (and a
# parenthesised note); anything else is one caller per process (a process waits for one ticket at a time).
function Get-RunLockCallerKey($Row) {
    $n = "$($Row.Name)".Trim()
    if ($n -match '\(parent pid (\d+)\)') { return "parent pid $($Matches[1])" }
    $base = ($n -replace '\s*\(.*$', '').Trim()
    $batch = ($base -replace '\s+(idle\s+)?(slot\s+hold|hold|lane|worker|slot)\s+\d+(\s+of\s+\d+)?\b', '').Trim()
    if ($batch -and $batch -ne $base) { return "batch $batch" }
    return "pid $($Row.ProcId)"
}

# Seconds covered by the union of intervals {Start, End} (overlaps counted once), optionally clipped to [From, To].
function Measure-RunLockUnion($Intervals, $From = $null, $To = $null) {
    $tot = 0.0; $cs = $null; $ce = $null
    foreach ($iv in @($Intervals | Where-Object { $_ } | Sort-Object Start)) {
        $s = $iv.Start; $e = $iv.End
        if ($null -ne $From -and $s -lt $From) { $s = $From }
        if ($null -ne $To -and $e -gt $To) { $e = $To }
        if ($e -le $s) { continue }
        if ($null -eq $cs -or $s -gt $ce) {
            if ($null -ne $cs) { $tot += ($ce - $cs).TotalSeconds }
            $cs = $s; $ce = $e
        } elseif ($e -gt $ce) { $ce = $e }
    }
    if ($null -ne $cs) { $tot += ($ce - $cs).TotalSeconds }
    $tot
}

# Per-caller merged wait seconds of acquire/timeout rows: wait intervals [time - waited, time] of one caller merged.
function Measure-RunLockCallerWaits($AcqRows) {
    $g = @{}
    foreach ($r in @($AcqRows)) {
        if ($null -eq $r.Waited) { continue }
        $k = Get-RunLockCallerKey $r
        if (-not $g.ContainsKey($k)) { $g[$k] = [System.Collections.Generic.List[object]]::new() }
        $g[$k].Add([pscustomobject]@{ Start = $r.Time.AddSeconds(-[double]$r.Waited); End = $r.Time })
    }
    $sum = 0.0; foreach ($k in $g.Keys) { $sum += Measure-RunLockUnion $g[$k] }
    [pscustomobject]@{ Seconds = $sum; Callers = $g.Count }
}

function Show-RunLockReport([datetime]$From, [datetime]$To, [int]$Top = 5, [switch]$NoLogs, [switch]$ByName) {
    $span = ($To - $From).TotalSeconds
    $long = $script:RunLockLongHoldMinutes * 60
    "run lock report: $($From.ToString('yyyy-MM-dd HH:mm')) to $($To.ToString('yyyy-MM-dd HH:mm')) ($(Format-RunLockDuration $span))"
    $allRows = @(Read-RunLockHistory)
    $rows = @($allRows | Where-Object { $_.Time -le $To })   # the window's rows (counts, waits, acquires per hour)
    $realNow = Get-Date
    # Holds are paired over the whole history (a hold that runs past a window end in the past keeps its release
    # line) and clipped to the window where time is measured.
    $holds = @(if ($rows.Count) { Get-RunLockHolds $allRows $realNow })
    $winHolds = @($holds | Where-Object { $_.End -ge $From -and $_.Start -le $To })
    $runs = @()
    # 1. Games running, from the run logs.
    if (-not $NoLogs) {
        $runs = @(Get-RunLockGameRuns $From $To)
        $acc = Measure-RunLockTimeline @($runs | ForEach-Object { [pscustomobject]@{ Start = $_.Start; End = $_.End; Key = 'g' } }) $From $To { param($c) $n = [int]$c['g']; if ($n -ge 3) { '3+' } else { "$n" } }
        $gameSec = 0.0; foreach ($r in $runs) { $gameSec += Get-RunLockOverlapSeconds $r.Start $r.End $From $To }
        $busy = $span - [double]$acc['0']
        ''
        $nLive = @($runs | Where-Object Running).Count
        "games running (from run logs: each run's first to last log line, so estimated): $($runs.Count) runs$(if ($nLive) { " ($nLive still running: counted to now)" })"
        '  time with 0 / 1 / 2 / 3+ games: ' + (Format-RunLockShares $acc @('0', '1', '2', '3+') $span)
        '  game time {0}; while any game ran, {1:N2} games on average (3.00 = every lane full)' -f (Format-RunLockDuration $gameSec), $(if ($busy -gt 0) { $gameSec / $busy } else { 0 })
        if ($runs.Count) { '  runs by lane: ' + (($runs | Group-Object Lane | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ') }
        if (@($runs | Group-Object Build).Count -gt 1) { '  runs by exe folder (out\...): ' + (($runs | Group-Object Build | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ') }
    }
    # 2. Lock holds, from history.tsv.
    ''
    if (-not $rows.Count) {
        if ($allRows.Count) { "lock holds: none in history.tsv up to $($To.ToString('HH:mm')) (it starts $($allRows[0].Time.ToString('yyyy-MM-dd HH:mm:ss')))" }
        else { "lock holds: no history yet ($script:RunLockHistoryFile is written from the first acquire after this version)" }
    }
    else {
        $hFrom = if ($allRows[0].Time -gt $From) { $allRows[0].Time } else { $From }
        $hSpan = ($To - $hFrom).TotalSeconds
        $acc = Measure-RunLockTimeline @($holds | ForEach-Object { [pscustomobject]@{ Start = $_.Start; End = $_.End; Key = $_.Mode } }) $hFrom $To {
            param($c) if ([int]$c['exclusive'] -gt 0) { 'exclusive' } elseif ([int]$c['shared'] -gt 0) { "shared $([Math]::Min(3, [int]$c['shared']))" } else { 'free' } }
        $hist = if ($hFrom -gt $From) { "history starts $($hFrom.ToString('HH:mm:ss')), so this part covers $(Format-RunLockDuration $hSpan)" } else { 'history covers the whole window' }
        "lock holds (from history.tsv; $hist):"
        '  time free / exclusive / shared 1 / 2 / 3: ' + (Format-RunLockShares $acc @('free', 'exclusive', 'shared 1', 'shared 2', 'shared 3') $hSpan)
        if (-not $NoLogs) {
            # Holds against games: time the lock was held but no game ran (building, analysing, mirroring user data,
            # triage, waiting inside a hold) is lane time nobody used.
            $iv2 = @($holds | ForEach-Object { [pscustomobject]@{ Start = $_.Start; End = $_.End; Key = 'h' } }) +
                   @($runs | ForEach-Object { [pscustomobject]@{ Start = $_.Start; End = $_.End; Key = 'g' } })
            $acc2 = Measure-RunLockTimeline $iv2 $hFrom $To {
                param($c) $hh = [int]$c['h'] -gt 0; $gg = [int]$c['g'] -gt 0
                if ($hh -and -not $gg) { 'held, no game' } elseif ($hh) { 'held, game' } elseif ($gg) { 'game, no hold logged' } else { 'free' } }
            '  held but no game running {0:N1}% of the time, held with a game {1:N1}%, a game with no logged hold {2:N1}% (older-copy holders), neither {3:N1}%' -f `
                (100.0 * [double]$acc2['held, no game'] / $hSpan), (100.0 * [double]$acc2['held, game'] / $hSpan),
                (100.0 * [double]$acc2['game, no hold logged'] / $hSpan), (100.0 * [double]$acc2['free'] / $hSpan)
        }
        $wr = @($rows | Where-Object { $_.Time -ge $From })
        $est = @($winHolds | Where-Object { $_.State -like 'no release*' }).Count
        '  {0} exclusive and {1} shared holds, {2} re-entries, {3} queue timeouts{4}' -f @($winHolds | Where-Object Mode -eq 'exclusive').Count,
            @($winHolds | Where-Object Mode -eq 'shared').Count, @($wr | Where-Object Kind -eq 'reenter').Count, @($wr | Where-Object Kind -eq 'timeout').Count,
            $(if ($est) { "; $est without a release line (end estimated)" } else { '' })
        # 3. Waits per tool (first word of the holder name).
        $acq = @($wr | Where-Object { $_.Kind -in 'acquire', 'timeout' })
        if ($acq.Count) {
            ''
            $by = if ($ByName) { 'holder name (first 60 characters)' } else { 'tool (first word of the holder name; -ByName for full names)' }
            "waits in the queue per $by, over the window's acquires and timeouts:"
            '  (median / max per acquire; sum = every acquire added up, so the parallel workers or lanes of one call, which wait'
            '  together, each count; per call = the waits of one calling job merged: run_list -Parallel parent pid, experiment batch)'
            foreach ($g in @($acq | Group-Object { if ($ByName) { $n = "$($_.Name)".Trim(); $n.Substring(0, [Math]::Min(60, $n.Length)) } else { ("$($_.Name)" -split '\s+')[0] } } | Sort-Object { -(($_.Group | Measure-Object Waited -Sum).Sum) })) {
                $w = @($g.Group | ForEach-Object { [double]$_.Waited })
                $nTimeout = @($g.Group | Where-Object Kind -eq 'timeout').Count
                $cw = Measure-RunLockCallerWaits $g.Group
                '  {0,-16} n={1,-3} median {2,-12} max {3,-12} sum {4,-12} per call {5} ({6} call(s)){7}' -f $g.Name.PadRight($(if ($ByName) { 60 } else { 16 })), $g.Count, (Format-RunLockDuration (Get-RunLockMedian $w)),
                    (Format-RunLockDuration ($w | Measure-Object -Maximum).Maximum), (Format-RunLockDuration ($w | Measure-Object -Sum).Sum),
                    (Format-RunLockDuration $cw.Seconds), $cw.Callers, $(if ($nTimeout) { "  ($nTimeout timed out)" } else { '' })
            }
            $all = @($acq | ForEach-Object { [double]$_.Waited }); $allSum = 0.0; foreach ($x in $all) { $allSum += $x }
            $cwAll = Measure-RunLockCallerWaits $acq
            $anyone = Measure-RunLockUnion @($acq | Where-Object { $null -ne $_.Waited } | ForEach-Object { [pscustomobject]@{ Start = $_.Time.AddSeconds(-[double]$_.Waited); End = $_.Time } }) $hFrom $To
            '  all: sum {0} over {1} acquire(s); per calling job {2} over {3} job(s); someone was waiting {4} of the {5} ({6:N0}%)' -f `
                (Format-RunLockDuration $allSum), $acq.Count, (Format-RunLockDuration $cwAll.Seconds), $cwAll.Callers,
                (Format-RunLockDuration $anyone), (Format-RunLockDuration $hSpan), $(if ($hSpan -gt 0) { 100.0 * $anyone / $hSpan } else { 0 })
        }
        if (($realNow - $To).TotalMinutes -lt 1) {
            $qi = Get-RunLockQueueInfo
            if ($qi.Count) { "  waiting now (not in the figures above): $($qi.Count), the oldest for $(Format-RunLockDuration $qi.OldestSeconds)" }
        }
        # 4. Longest exclusive holds + hold discipline.
        $ex = @($winHolds | Where-Object Mode -eq 'exclusive' | Sort-Object { -($_.End - $_.Start).TotalSeconds })
        if ($ex.Count) {
            ''
            "longest exclusive holds (! = over $($script:RunLockLongHoldMinutes) min):"
            foreach ($h in @($ex | Select-Object -First $Top)) {
                $d = ($h.End - $h.Start).TotalSeconds
                $nm = if ($h.Name.Length -gt 80) { $h.Name.Substring(0, 77) + '...' } else { $h.Name }
                $extra = @()
                if ($h.State -ne 'released') { $extra += $h.State }
                if ($h.Start -lt $From) { $extra += 'started before the window' }
                if ($h.End -gt $To -and $h.State -ne 'held now') { $extra += "released after the window, $($h.End.ToString('HH:mm:ss'))" }
                if ($null -ne $h.QueueAtRelease) { $extra += "$([int]$h.QueueAtRelease) waiting at release" }
                if ($h.Reentries) { $extra += "$($h.Reentries) re-entries inside (child runs)" }
                '  {0}{1,-13} {2}  {3}{4}' -f $(if ($d -gt $long) { '!' } else { ' ' }), (Format-RunLockDuration $d), $h.Start.ToString('HH:mm:ss'), "'$nm'",
                    $(if ($extra.Count) { '  (' + ($extra -join '; ') + ')' } else { '' })
            }
            $over = @($ex | Where-Object { ($_.End - $_.Start).TotalSeconds -gt $long })
            if ($over.Count) {
                $os = 0.0; foreach ($h in $over) { $os += ($h.End - $h.Start).TotalSeconds }
                "WARNING: $($over.Count) exclusive hold(s) over $($script:RunLockLongHoldMinutes) min ($(Format-RunLockDuration $os) in all). Keep an exclusive hold to ONE timed run (docs\harness\lanes.md)"
            } else { "hold discipline: no exclusive hold over $($script:RunLockLongHoldMinutes) min" }
            $cur = @($winHolds | Where-Object { $_.State -like 'held now*' -and $_.Mode -eq 'exclusive' -and ($realNow - $_.Start).TotalSeconds -gt $long })
            foreach ($h in $cur) { "WARNING: '$($h.Name)' (pid $($h.ProcId)) has held the exclusive lock for $(Format-RunLockDuration ($realNow - $h.Start).TotalSeconds) and still holds it" }
        }
    }
    # 5. Per clock hour.
    ''
    'per hour       runs  game-min  avg games  acquires  excl-min  longest wait'
    $h0 = $From.Date.AddHours($From.Hour)
    for ($hs = $h0; $hs -lt $To; $hs = $hs.AddHours(1)) {
        $he = $hs.AddHours(1)
        $a = if ($hs -lt $From) { $From } else { $hs }; $b = if ($he -gt $To) { $To } else { $he }
        $len = ($b - $a).TotalSeconds
        if ($len -le 0) { continue }
        $nRuns = @($runs | Where-Object { $_.Start -ge $a -and $_.Start -lt $b }).Count
        $gs = 0.0; foreach ($r in $runs) { $gs += Get-RunLockOverlapSeconds $r.Start $r.End $a $b }
        $acqH = @($rows | Where-Object { $_.Kind -eq 'acquire' -and $_.Time -ge $a -and $_.Time -lt $b })
        $xs = 0.0; foreach ($h in @($holds | Where-Object Mode -eq 'exclusive')) { $xs += Get-RunLockOverlapSeconds $h.Start $h.End $a $b }
        $mw = ($acqH | Measure-Object Waited -Maximum).Maximum
        '{0}-{1}  {2,4}  {3,8:N1}  {4,9:N2}  {5,8}  {6,8:N1}  {7}' -f $a.ToString('HH:mm'), $b.ToString('HH:mm'), $nRuns, ($gs / 60), ($gs / $len), $acqH.Count, ($xs / 60),
            $(if ($acqH.Count) { Format-RunLockDuration $mw } else { '-' })
    }
    if ($NoLogs) { '(-NoLogs: runs / game columns not read)' }
    elseif (-not $rows.Count -or $rows[0].Time -gt $From) { '(acquires / excl-min / waits only from the start of history.tsv)' }
}

# ---- command-line entry (only when run as a script, not when dot-sourced) ----
if ($MyInvocation.InvocationName -ne '.' -and $args.Count -gt 0) {
    $verb = $args[0]
    switch ($verb) {
        'status' { Show-RunLockStatus; exit 0 }
        'clear-stale' { Clear-RunLockStale -Lock; exit 0 }
        'run' {
            $name = 'runlock run'; $timeout = 240; $runMode = 'Exclusive'; $i = 1
            while ($i -lt $args.Count -and $args[$i] -ne '--') {
                switch ($args[$i]) {
                    '-Name' { $name = $args[$i + 1]; $i += 2 }
                    '-TimeoutMinutes' { $timeout = [double]$args[$i + 1]; $i += 2 }
                    '-Shared' { $runMode = 'Shared'; $i += 1 }
                    '-Exclusive' { $runMode = 'Exclusive'; $i += 1 }
                    default { Write-Host "runlock run: unknown option $($args[$i])"; exit 2 }
                }
            }
            if ($i -ge $args.Count - 1) { Write-Host 'usage: runlock.ps1 run [-Shared] [-Name NAME] [-TimeoutMinutes M] -- CMD [ARGS...]'; exit 2 }
            $cmd = $args[$i + 1]; $rest = @(); if ($args.Count -gt $i + 2) { $rest = $args[($i + 2)..($args.Count - 1)] }
            # Checked before queueing: a typo shouldn't wait its turn and then "succeed".
            if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { [Console]::Error.WriteLine("runlock run: command not found: $cmd"); exit 127 }
            $script:RunLockMsgToStderr = $true   # keep the tool's stdout clean for whoever parses it
            try { $h = Enter-RunLock -Name $name -TimeoutMinutes $timeout -Mode $runMode }
            catch { [Console]::Error.WriteLine("runlock run: $($_.Exception.Message)"); exit 1 }
            $code = 1
            try {
                $global:LASTEXITCODE = $null
                $errBefore = $Error.Count
                & $cmd @rest
                $ok = $?
                # Native command or a script's `exit N`: that code. Otherwise 1 if it failed or wrote any error.
                $code = if ($null -ne $global:LASTEXITCODE) { [int]$global:LASTEXITCODE } elseif ($ok -and $Error.Count -eq $errBefore) { 0 } else { 1 }
            } catch {
                [Console]::Error.WriteLine("runlock run: $($_.Exception.Message)"); $code = 1
            } finally { Exit-RunLock $h -Report }
            exit $code
        }
        'report' {
            $usage = "usage: runlock.ps1 report [-Since HH:mm | -Since 'yyyy-MM-dd HH:mm' | -Hours N] [-Until HH:mm] [-Top N] [-ByName] [-NoLogs]"
            $since = $null; $until = $null; $hours = 0.0; $top = 5; $noLogs = $false; $byName = $false; $i = 1
            $ci = [System.Globalization.CultureInfo]::InvariantCulture
            while ($i -lt $args.Count) {
                # Fixer (review): every option that takes a value must have one (not the next option), -Hours a
                # positive number, -Top a whole number from 1; anything else prints the usage and exits 2.
                $opt = "$($args[$i])"
                $val = if ($i + 1 -lt $args.Count) { "$($args[$i + 1])".Trim() } else { '' }
                $bad = "runlock report: $opt needs a value, got '$val' ($usage)"
                switch ($opt) {
                    '-Since' { if (-not $val -or $val -match '^-[A-Za-z]') { Write-Host $bad; exit 2 }; $since = $val; $i += 2 }
                    '-Until' { if (-not $val -or $val -match '^-[A-Za-z]') { Write-Host $bad; exit 2 }; $until = $val; $i += 2 }
                    '-Hours' {
                        $d = 0.0
                        if (-not [double]::TryParse($val, [System.Globalization.NumberStyles]::Float, $ci, [ref]$d) -or $d -le 0 -or [double]::IsInfinity($d)) { Write-Host "runlock report: -Hours needs a number above 0, got '$val' ($usage)"; exit 2 }
                        $hours = $d; $i += 2
                    }
                    '-Top' {
                        $n = 0
                        if (-not [int]::TryParse($val, [System.Globalization.NumberStyles]::None, $ci, [ref]$n) -or $n -lt 1) { Write-Host "runlock report: -Top needs a whole number from 1, got '$val' ($usage)"; exit 2 }
                        $top = $n; $i += 2
                    }
                    '-NoLogs' { $noLogs = $true; $i += 1 }
                    '-ByName' { $byName = $true; $i += 1 }
                    default { Write-Host "runlock report: unknown option $opt ($usage)"; exit 2 }
                }
            }
            if ($since -and $hours -gt 0) { Write-Host "runlock report: give -Since or -Hours, not both ($usage)"; exit 2 }
            $now = Get-Date
            # 'HH:mm' = today (yesterday if that is still ahead); 'yyyy-MM-dd HH:mm' as given.
            $clock = {
                param([string]$Text)
                $d = [datetime]::MinValue; $ci = [System.Globalization.CultureInfo]::InvariantCulture
                if ([datetime]::TryParseExact($Text, 'HH:mm', $ci, [System.Globalization.DateTimeStyles]::None, [ref]$d)) {
                    $d = $now.Date.Add($d.TimeOfDay); if ($d -gt $now) { $d = $d.AddDays(-1) }; return $d
                }
                if ([datetime]::TryParseExact($Text, 'yyyy-MM-dd HH:mm', $ci, [System.Globalization.DateTimeStyles]::None, [ref]$d)) { return $d }
                return $null
            }
            $from = $null
            if ($since) { $from = & $clock $since; if (-not $from) { Write-Host "runlock report: bad -Since '$since' ($usage)"; exit 2 } }
            $to = $now
            if ($until) {
                $to = & $clock $until; if (-not $to) { Write-Host "runlock report: bad -Until '$until' ($usage)"; exit 2 }
                # Fixer: with -Since, an 'HH:mm' -Until is the first such time after -Since ('-Since 21:04 -Until 21:06'
                # at 21:05 used to become yesterday 21:06: an empty window); either way never later than now.
                if ($from -and $until -match '^\d\d:\d\d$') { $to = $from.Date.Add($to.TimeOfDay); if ($to -le $from) { $to = $to.AddDays(1) } }
                if ($to -gt $now) { $to = $now }
            }
            if (-not $from) { $from = if ($hours -gt 0) { $to.AddHours(-$hours) } else { $to.AddHours(-12) } }
            if ($from -ge $to) { Write-Host "runlock report: the window is empty ($($from.ToString('yyyy-MM-dd HH:mm:ss')) to $($to.ToString('yyyy-MM-dd HH:mm:ss')))"; exit 2 }
            Show-RunLockReport -From $from -To $to -Top $top -NoLogs:$noLogs -ByName:$byName
            exit 0
        }
        default { Write-Host "runlock.ps1: unknown verb '$verb' (status | clear-stale | run | report)"; exit 2 }
    }
}
