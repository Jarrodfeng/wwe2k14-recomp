# S110 BUILDLOCK (docs\research\s110\BUILDLOCK.md): per-folder reader / writer locks for builds whose outputs no PC game run loads (the Android chain).
# The run lock (tools\runlock.ps1) guards games and the files a game run loads. A build into out\android needs only that no other build writes a folder
# it reads or writes at the same moment, so it holds one lock per folder: <lock dir>\<key>.lock, key = the folder's full path (lower case, every run of
# characters outside [a-z0-9.-] -> _). The lock IS the open handle (Windows share modes, as the run lock's lock.json): a WRITER keeps the file open
# read/write sharing nothing, a READER keeps it open read-only sharing read. Any number of readers coexist, a writer is alone, and a dead holder's
# handle closes by itself (nothing to clean; the file is never deleted).
# All or nothing: Enter-FolderLocks opens every lock it asks for in key order; when one is busy it closes the ones it got and tries again after
# -PollMilliseconds (no hold-and-wait, so two callers can never deadlock on each other). <key>.who (best effort, for the waiting line only) names the
# last taker. CPU and memory are not this file's business: the compile queue (tools\admit\Admit.ps1) rations them.
#   . "$root\tools\harness\folderlock.ps1"
#   $h = Enter-FolderLocks -Read @($sdkInstall) -Write @($build) -Name 'build_apk.ps1' [-TimeoutMinutes 60]   # waits; throws at the limit
#   try { ... } finally { Exit-FolderLocks $h }
# Lock folder: $env:WWE_FOLDERLOCK_DIR (tests), else the main checkout's out\harness\runlock\folders (one place for the checkout and every worktree).
# Proof: pwsh -NoProfile -File tools\harness\folderlock_selftest.ps1

function Get-FolderLockDir {
    if ($env:WWE_FOLDERLOCK_DIR) { return $env:WWE_FOLDERLOCK_DIR }
    return 'C:\Users\Jarrod\wwe2k14-recomp\out\harness\runlock\folders'
}

function Get-FolderLockKey([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/').ToLowerInvariant()
    return ($full -replace '[^a-z0-9.\-]+', '_')
}

function Enter-FolderLocks {
    param([string[]]$Read = @(), [string[]]$Write = @(), [string]$Name = 'build', [double]$TimeoutMinutes = 60, [int]$PollMilliseconds = 500)
    $dir = Get-FolderLockDir
    [void][System.IO.Directory]::CreateDirectory($dir)
    $want = @{}; $shown = @{}
    foreach ($p in @($Read | Where-Object { $_ })) { $k = Get-FolderLockKey $p; if (-not $want.ContainsKey($k)) { $want[$k] = 'Read'; $shown[$k] = $p } }
    foreach ($p in @($Write | Where-Object { $_ })) { $k = Get-FolderLockKey $p; $want[$k] = 'Write'; $shown[$k] = $p }   # a folder in both lists is written
    $keys = @($want.Keys | Sort-Object)
    $t0 = Get-Date
    $deadline = $t0.AddMinutes($TimeoutMinutes)
    $said = ''
    while ($true) {
        $got = [System.Collections.Generic.List[System.IO.FileStream]]::new()
        $busy = $null
        foreach ($k in $keys) {
            $f = Join-Path $dir "$k.lock"
            try {
                if ($want[$k] -eq 'Write') { $got.Add([System.IO.File]::Open($f, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)) }
                else { $got.Add([System.IO.File]::Open($f, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)) }
            } catch [System.IO.IOException] { $busy = $k; break }
        }
        if (-not $busy) {
            $now = Get-Date
            foreach ($k in $keys) {
                try { [System.IO.File]::WriteAllText((Join-Path $dir "$k.who"), ("{0} '{1}' pid {2} since {3:yyyy-MM-dd HH:mm:ss}" -f $want[$k].ToLowerInvariant(), $Name, $PID, $now)) } catch { }
            }
            $waited = ($now - $t0).TotalSeconds
            if ($said) { Write-Host ("[folderlock] '{0}' took its {1} folder lock(s) after {2:N0} s" -f $Name, $keys.Count, $waited) }
            return [pscustomobject]@{ Name = $Name; Keys = $keys; Modes = $want; Streams = $got; WaitedSeconds = $waited }
        }
        foreach ($s in $got) { $s.Dispose() }
        $who = ''
        try { $who = [System.IO.File]::ReadAllText((Join-Path $dir "$busy.who")).Trim() } catch { }
        $line = "[folderlock] '$Name' waits for $($want[$busy].ToLowerInvariant()) access to $($shown[$busy])$(if ($who) { " (last taker: $who)" })"
        if ($line -ne $said) { Write-Host $line; $said = $line }
        if ((Get-Date) -ge $deadline) { throw "folder lock: '$Name' could not get $($want[$busy].ToLowerInvariant()) access to $($shown[$busy]) within $TimeoutMinutes min$(if ($who) { " (last taker: $who)" })" }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
}

function Exit-FolderLocks($Handle) {
    if (-not $Handle) { return }
    foreach ($s in @($Handle.Streams)) { try { $s.Dispose() } catch { } }
    $Handle.Streams = @()
}
