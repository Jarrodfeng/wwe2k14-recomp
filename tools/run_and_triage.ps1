# Launch the game, wait, then summarise its new log. -Summarize wwe2k14_NNN.log summarises an existing log
# without launching anything.
# Session 8: takes the run lock (tools\runlock.ps1) before launching, so parallel callers queue instead of failing;
# -KillAfterWait kills the game if it is still running after -WaitSeconds (unattended runs; a hung game would
# otherwise block the queue). -Exe runs another build's exe (e.g. the coverage preset) from its own folder.
# Session 8 (task 0): the lock is SHARED by default (up to 3 ordinary game runs at once) ONLY when -ExtraArgs gives
# the run its own --user_data_root (run_list passes userdata_work_s<slot>) and the exe logs "run log number";
# otherwise (e.g. the plain `-WaitSeconds 30` launch, which uses the default Documents\wwe2k14 profile) it is
# EXCLUSIVE as before: it waits while any wwe2k14 runs and never starts a second instance. -Exclusive forces
# exclusive (timed runs). Inside a caller's hold (run_list, regress, ...) it re-enters that hold, whatever its mode.
# The "ABORT if a wwe2k14 is already running" check applies to exclusive holds only. The launch is serialized
# (Enter-RunLockLaunch: from Start-Process until this run's new log exists and holds its "run log number" line, and
# at least 3.5 s), and this run's log is the wwe2k14_NNN.log that appeared inside its own launch window, never "the
# newest log"; afterwards the recorded number and pid are checked against the file and the launched process.
param([int]$WaitSeconds = 30, [string[]]$ExtraArgs = @(), [string]$Summarize = '', [switch]$KillAfterWait,
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\wwe2k14.exe', [switch]$Exclusive)
$dir = Split-Path $Exe -Parent

if ($Summarize) {
    $log = Get-Item (Join-Path "$dir\logs" (Split-Path $Summarize -Leaf))
} else {
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\runlock.ps1'
$lockName = "run_and_triage $(Split-Path $dir -Leaf) $($ExtraArgs -join ' ')"
$lockName = $lockName.Substring(0, [Math]::Min(200, $lockName.Length))
# Fixer (review findings 1 and 5): SHARED only when this run has its own user data folder (otherwise ReXGlue uses the
# default Documents\wwe2k14 profile, Jarrod's real save, which concurrent runs must never share) and the exe takes its
# log number once (older exes write each other's captures / CSVs). Otherwise EXCLUSIVE, the pre-shared behaviour.
$wantMode = if ($Exclusive) { 'Exclusive' } else { 'Shared' }
$whyExclusive = $null
$exeLogsNum = Test-RunLockExeSharedSafe $Exe   # the exe logs "[wwe2k14] run log number: NNN (pid P)"
if (-not $Exclusive) {
    if (($ExtraArgs -join ' ') -notmatch '--user_data_root=') { $whyExclusive = 'no --user_data_root in -ExtraArgs (it would use the default Documents\wwe2k14 profile)' }
    elseif (-not $exeLogsNum) { $whyExclusive = "this exe does not log 'run log number' (built before the per-run log number; concurrent runs would write each other's files)" }
    if ($whyExclusive) { $wantMode = 'Exclusive' }
}
try { $lock = Enter-RunLock -Name $lockName -Mode $wantMode }
catch {
    # Exclusive asked for inside a shared hold (runlock.ps1 throws rather than wait on its own parent), or a timeout.
    $pre = if ($whyExclusive) { "cannot run shared ($whyExclusive): " } else { '' }
    "ABORT: $pre$($_.Exception.Message)"
    exit 1
}
if ($whyExclusive -and $lock.Mode -eq 'Exclusive' -and -not $lock.Reentrant) { "[runlock] running EXCLUSIVE, not shared: $whyExclusive" }
$log = $null
try {
if ($lock.Mode -eq 'Exclusive') {
    $running = @(Get-Process wwe2k14 -ErrorAction SilentlyContinue).Count
    if ($running -gt 0) { "ABORT: $running wwe2k14 instance(s) already running; close them first."; exit 1 }
}
"[runlock] run mode: $($lock.Mode.ToLowerInvariant())$(if ($lock.Slot) { " slot $($lock.Slot)" })"
# Session 8 (task 2): which pinned SDK build (tools\sdk_pins.json) this run loads; read inside the lock, so no build
# can restage the DLLs before the game loads them. Printed after the run and kept in logs\sdk_pin_runs.log.
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\sdk_pins.ps1'
$pinLine = Get-SdkPinLine -ExeDir $dir

$logDir = Join-Path $dir 'logs'
function Get-NewRunLogs($Before) {
    @(Get-ChildItem (Join-Path $logDir 'wwe2k14_*.log') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^wwe2k14_\d+\.log$' -and $Before -notcontains $_.Name } | Sort-Object CreationTimeUtc)
}
# "[wwe2k14] run log number: NNN (pid P)" -> {Num, Pid}, or $null. Reads the log's head sharing read/write/delete,
# so it works on a log the game is still writing (the line is the log's first; 1 MB is plenty).
function Get-RunLogRecord($File) {
    if (-not $File) { return $null }
    $text = $null
    try {
        $fs = [System.IO.File]::Open($File.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
        try { $buf = [byte[]]::new(1MB); $n = $fs.Read($buf, 0, $buf.Length); $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, [Math]::Max(0, $n)) }
        finally { $fs.Dispose() }
    } catch { return $null }
    $m = [regex]::Match($text, '\[wwe2k14\] run log number: (\d+) \(pid (\d+)\)')
    if ($m.Success) { [pscustomobject]@{ Num = [int]$m.Groups[1].Value; Pid = [int]$m.Groups[2].Value } } else { $null }
}
$launch = Enter-RunLockLaunch -Name $lockName
$serialized = [bool]$launch
try {
    $before = @(Get-ChildItem (Join-Path $logDir 'wwe2k14_*.log') -File -ErrorAction SilentlyContinue | ForEach-Object Name)
    $t0 = Get-Date
    $p = if ($ExtraArgs.Count) { Start-Process -FilePath $Exe -ArgumentList $ExtraArgs -WorkingDirectory $dir -PassThru }
         else { Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru }
    # Unattended runs only: if this script is killed, Windows kills the game too (it would otherwise outlive the lock).
    # Interactive launches (no -KillAfterWait) deliberately leave the game running.
    if ($KillAfterWait) { $null = Add-RunLockKillOnExit $p }
    # This run's log: the first new wwe2k14_NNN.log inside this launch window (nobody else can start meanwhile).
    $until = $t0.AddSeconds(90)
    while ($true) {
        $new = Get-NewRunLogs $before
        if ($new.Count) { $log = $new[0]; break }
        if ($p.HasExited) { $new = Get-NewRunLogs $before; if ($new.Count) { $log = $new[0] }; break }
        if ((Get-Date) -gt $until) { break }
        Start-Sleep -Milliseconds 100
    }
    $seen = ((Get-Date) - $t0).TotalSeconds
    # Fixer (review finding 4): an exe that logs its run log number scans for the newest log once, in
    # OnPostInitLogging, right after ReXGlue created the file; keep the window closed until that line is in the log,
    # so the next instance's log can't exist yet when this one scans (the line is the log's first, flushed at once).
    $headRec = $null
    if ($log -and $exeLogsNum) {
        $untilRec = (Get-Date).AddSeconds(30)
        while (-not ($headRec = Get-RunLogRecord $log) -and -not $p.HasExited -and (Get-Date) -lt $untilRec) { Start-Sleep -Milliseconds 50 }
        if (-not $headRec) { $headRec = Get-RunLogRecord $log }
        if (-not $headRec) { "[runlock] warning: no 'run log number' line in $($log.Name) {0:N1} s after launch; closing the launch window anyway" -f ((Get-Date) - $t0).TotalSeconds }
    }
    $tLine = Get-Date
    # Keep the window >= 3.5 s from the launch: no two runs start within 3 s (triage.py matches a crash event to a run
    # by its process start time, -1..3 s before the log's first line). Fixer (review finding 3): also when the game
    # has already exited (a crash at startup is exactly when triage matters); and >= 1.5 s after this log's first line
    # showed up, so a slow-starting run's next launch can't land within 1 s after that line either.
    $gap = [Math]::Max(3.5 - ((Get-Date) - $t0).TotalSeconds, $(if ($headRec) { 1.5 - ((Get-Date) - $tLine).TotalSeconds } else { 0 }))
    if ($gap -gt 0) { Start-Sleep -Milliseconds ([int]($gap * 1000)) }
} finally { Exit-RunLockLaunch $launch }
if ($log) { "[runlock] log of pid $($p.Id): $($log.Name) (appeared {0:N1} s after launch{1})" -f $seen, $(if ($serialized) { ', inside its launch window' } else { ', launch window NOT serialized' }) }
else { "[runlock] no log appeared within the launch window ({0:N0} s) for pid $($p.Id)" -f $seen }
$left = [Math]::Max(0, [int]($WaitSeconds * 1000 - ((Get-Date) - $t0).TotalMilliseconds))
if ($p.WaitForExit($left)) {
    "EXITED after {0:N1}s, exit code {1} (0x{2:X8})" -f ((Get-Date) - $t0).TotalSeconds, $p.ExitCode, $p.ExitCode
} else {
    $p.Refresh()
    "STILL RUNNING after ${WaitSeconds}s: PID $($p.Id), CPU $([math]::Round($p.TotalProcessorTime.TotalSeconds,1))s, RAM $([math]::Round($p.WorkingSet64/1MB)) MB, threads $($p.Threads.Count), title '$($p.MainWindowTitle)', responding $($p.Responding)"
    if ($KillAfterWait) {
        $p.Kill(); $null = $p.WaitForExit(15000)
        "KILLED after ${WaitSeconds}s (-KillAfterWait)"
    }
}
# Cross-check: builds from Session 8 task 0 on log "[wwe2k14] run log number: NNN (pid P)" at startup. P must be the
# pid we launched, and NNN (the number the app names its captures, frames/timeline CSVs and coverage folder by) must
# be the log file's own number (fixer, review finding 4: only the pid was compared before).
function Get-RunLogPid($File) { $r = Get-RunLogRecord $File; if ($r) { $r.Pid } else { $null } }
if ($log) {
    $lp = Get-RunLogPid $log
    if ($null -ne $lp -and $lp -ne $p.Id) {
        $mine = Get-NewRunLogs $before | Where-Object { (Get-RunLogPid $_) -eq $p.Id } | Select-Object -First 1
        "[runlock] warning: $($log.Name) records pid $lp, not $($p.Id); " + $(if ($mine) { "using $($mine.Name) (pid match)" } else { 'no new log records this pid' })
        $log = $mine
    } elseif ($null -ne $lp) { "[runlock] log pid check: $($log.Name) records pid $lp, matches" }
} else {
    $log = Get-NewRunLogs $before | Where-Object { (Get-RunLogPid $_) -eq $p.Id } | Select-Object -First 1
    if ($log) { "[runlock] log found after the run by pid: $($log.Name)" }
}
if ($log) {
    $rr = Get-RunLogRecord $log
    $fileNum = [int]($log.Name -replace '^wwe2k14_(\d+)\.log$', '$1')
    if ($rr -and $rr.Num -ne $fileNum) {
        "[runlock] WARNING run log number MISMATCH: $($log.Name) records run log number $($rr.Num): this run's captures and frames/timeline CSVs are named $($rr.Num), not $fileNum (another run's files may have been overwritten)"
    } elseif ($rr) { "[runlock] log number check: $($log.Name) records run log number $($rr.Num), matches" }
    elseif ($exeLogsNum) { "[runlock] warning: $($log.Name) has no 'run log number' line although this exe logs one" }
}
if (-not $log -and $lock.Mode -eq 'Exclusive') {
    # Exclusive: no other game ran meanwhile, so a log that showed up late is ours (the old rule).
    Start-Sleep -Milliseconds 500
    $log = Get-NewRunLogs $before | Select-Object -Last 1
}
} finally { Exit-RunLock $lock }

if (-not $log) { "no new log file"; exit 0 }
}
$lines = Get-Content $log.FullName
"=== log: $($log.Name), $($lines.Count) lines ==="
$pinLedger = Join-Path $log.DirectoryName 'sdk_pin_runs.log'
if ($Summarize) {
    $rec = if (Test-Path -LiteralPath $pinLedger) { Select-String -LiteralPath $pinLedger -SimpleMatch "$($log.BaseName)  " | Select-Object -Last 1 }
    if ($rec) { $rec.Line -replace '^\S+\s+\S+\s+', '' }
    else { . 'C:\Users\Jarrod\wwe2k14-recomp\tools\sdk_pins.ps1'; (Get-SdkPinLine -ExeDir $dir) + ' (staged now; this run is not in sdk_pin_runs.log)' }
} else {
    $pinLine
    for ($k = 0; $k -lt 20; $k++) {   # several runs may finish at once
        try { Add-Content -LiteralPath $pinLedger -Value ('{0}  {1}  {2}' -f $log.BaseName, (Get-Date).ToString('s'), $pinLine) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 50 }
    }
}
"--- our [wwe2k14] lines ---"
$lines | Select-String '\[wwe2k14\]' |
    Where-Object { $_.Line -notmatch '\[wwe2k14\]\[(scan|mem)\] (candidate|lone|MmAllocate)|\[wwe2k14\]\[(file|frames)\]' } |
    ForEach-Object { $_.Line -replace '^\[[^\]]+\]\s*', '' }

$opens = @($lines | Select-String '\[wwe2k14\]\[file\]')
$failed = @($opens | Where-Object { $_.Line -notmatch '-> 0x00000000' })
"--- file opens: $($opens.Count) total, $($failed.Count) failed (unique failures below; last 10 opens after) ---"
$failed | ForEach-Object { $_.Line -replace '^.*\[wwe2k14\]\[file\]\s*', '' } | Group-Object | ForEach-Object { "{0,5}x  {1}" -f $_.Count, $_.Name }
$opens | Select-Object -Last 10 | ForEach-Object { $_.Line -replace '^\[[^\]]+\]\s*\[\w+\]\s*\[\w+\]\s*', '' }

$csv = Join-Path $log.DirectoryName ($log.Name -replace '^wwe2k14_(\d+)\.log$', 'frames_$1.csv')
if (Test-Path $csv) {
    $ms = @(Import-Csv $csv | ForEach-Object { [double]$_.frame_ms })
    if ($ms.Count) {
        $sorted = $ms | Sort-Object
        $total = ($ms | Measure-Object -Sum).Sum
        "--- guest frames ({0}): {1} frames in {2:N0} s = {3:N1} fps avg; median {4:N1} ms, p95 {5:N1} ms, worst {6:N0} ms; frames >= 100 ms: {7}, >= 250 ms: {8} ---" -f `
            (Split-Path $csv -Leaf), $ms.Count, ($total / 1000), ($ms.Count / ($total / 1000)), $sorted[[int]($ms.Count * 0.5)],
            $sorted[[math]::Min($ms.Count - 1, [int]($ms.Count * 0.95))], $sorted[-1], @($ms | Where-Object { $_ -ge 100 }).Count, @($ms | Where-Object { $_ -ge 250 }).Count
    }
}
"--- warnings/errors/critical (deduplicated, top 30) ---"
$lines | Select-String '\[(warning|error|critical)\]' |
    ForEach-Object { $_.Line -replace '^\[[^\]]+\]\s*', '' -replace '\[t\d+\]\s*', '' } |
    Group-Object | Sort-Object Count -Descending | Select-Object -First 30 |
    ForEach-Object { "{0,6}x  {1}" -f $_.Count, $_.Name.Substring(0, [Math]::Min(220, $_.Name.Length)) }
"--- last 25 lines ---"
$lines | Select-Object -Last 25 | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) }
