# S106 lane C5: "alone" for the run lock (docs\research\s106\C5.md, docs\harness\runlock.md "Alone"). Dot-sourced by tools\runlock.ps1; it only defines functions.
#
# `-Alone` = EXCLUSIVE (no other game, no run-lock build) + a PAUSE FILE (<run lock dir>\alone.lock: byte 0 locked by this process, a JSON record
# {pid, pid_start, token, name, since, until} from byte 1) that the compile queue (tools\admit: admit.exe, admit_py.py, Admit.ps1) honours: while the pause
# is held admit grants nothing, the forced path included, and paused time does not count toward a caller's timeout. The holder then DRAINS: it waits (at most
# -AloneDrainSeconds, default 300) until nothing else runs: no wwe2k14 process, no run-lock build slot, no live admit slot, no compiler process, none of them
# in the holder's own process tree. At the limit it releases everything and refuses (exit 3, the recipe's BLOCKED-ALONE). While the run lasts a sampler (a
# runspace thread of this process: it dies with the process) looks every 2 s and notes anything it sees; the end line says `held` or `broken`.
# Crash: the OS frees the pause when the holder dies; the EXCLUSIVE record goes stale and the next taker takes it over. Nothing needs cleaning.
#
# Lines (printed by the callers, history.tsv rows `alone` / `alone_end` / `alone_refused` carry the same text):
#   alone: yes (games 0, compiles 0, since 19:50:06; waited 78 s for games and run-lock holders, 0 s for compiles)
#   alone: held (games 0, compiles 0, 19:50:06-19:51:11, 33 samples)
#   alone: broken at 19:50:40 (clang++.exe pid 1234, seen 19:50:40-19:50:52): readout not judged
#   alone: NO after 300 s (games 0; compiles 6: admit cc.tool.0 pid 10836 for pid 12772 'hold for pid 12772', clang++.exe x3): not run

function Get-RunLockAloneFilePath {
    if ($env:WWE_ALONE_FILE) { return $env:WWE_ALONE_FILE }
    return (Join-Path $script:RunLockDir 'alone.lock')
}

# The busy test, one scan. Self-contained on purpose (it runs inside the sampler's runspace too): everything comes in through $Cfg
# (HolderPids, GameNames, CompilerNames, BuildFiles, AdmitDirs). -> @{ Games; Compiles; Items = @(@{ Kind; Text; Key }) }; Items empty = alone.
function Get-RunLockAloneBusy {
    param([hashtable]$Cfg)
    $items = New-Object System.Collections.Generic.List[object]
    $holders = @($Cfg.HolderPids | ForEach-Object { [int]$_ })      # this alone run's own process: its DESCENDANTS are "own"
    $ancestors = @($Cfg.AncestorPids | ForEach-Object { [int]$_ })  # and its ancestor chain (a build tool that started the run); never its siblings (S106 C5F note 3)
    $ownCache = $Cfg.OwnCache                                        # "pid|start ticks" -> $true for what was judged own (S106 C5F note 9: no process-table read for it again)
    $ctx = @{ parent = $null; failed = $false }
    $own = {
        param([int]$Id)
        if ($ancestors -contains $Id -or $holders -contains $Id) { return $true }
        $key = $null
        try { $pr = Get-Process -Id $Id -ErrorAction Stop; $key = "$Id|$($pr.StartTime.Ticks)" } catch { return $false }   # gone (or unreadable): nothing to judge
        if ($ownCache -and $ownCache.ContainsKey($key)) { return $true }
        if ($null -eq $ctx.parent) {
            $ctx.parent = @{}
            try {
                foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, CreationDate -ErrorAction Stop)) {
                    $ctx.parent[[int]$p.ProcessId] = @{ pp = [int]$p.ParentProcessId; born = $(if ($p.CreationDate) { [datetime]$p.CreationDate } else { [datetime]::MinValue }) }
                }
            } catch { $ctx.failed = $true }
        }
        if ($ctx.failed) { return $false }   # the process table cannot be read: nothing is "ours" (conservative)
        $cur = $Id
        for ($k = 0; $k -lt 16; $k++) {
            if (-not $ctx.parent.ContainsKey($cur)) { break }
            $e = $ctx.parent[$cur]
            $par = $e.pp
            # S106 C5F note 10: a parent created AFTER its child is a reused pid, not the parent
            if ($ctx.parent.ContainsKey($par) -and $ctx.parent[$par].born -ge $e.born) { break }
            if ($holders -contains $par) { if ($ownCache) { $ownCache[$key] = $true }; return $true }
            $cur = $par
        }
        return $false
    }
    $games = 0
    foreach ($p in @(Get-Process -Name $Cfg.GameNames -ErrorAction SilentlyContinue)) {
        if (& $own $p.Id) { continue }
        $games++
        $items.Add(@{ Kind = 'game'; Text = "$($p.ProcessName).exe pid $($p.Id)"; Key = "game:$($p.Id)" })
    }
    foreach ($p in @(Get-Process -Name $Cfg.CompilerNames -ErrorAction SilentlyContinue)) {
        if (& $own $p.Id) { continue }
        $items.Add(@{ Kind = 'cc'; Text = "$($p.ProcessName).exe pid $($p.Id)"; Key = "cc:$($p.Id)"; Name = "$($p.ProcessName).exe" })
    }
    # a live run-lock build slot (a holder pid alive and not the same process reused)
    foreach ($bf in @($Cfg.BuildFiles)) {
        $rec = $null
        try {
            $fs = [System.IO.File]::Open($bf, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
            try { $txt = (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
            if ($txt.Trim()) { $rec = $txt | ConvertFrom-Json }
        } catch { }
        if (-not $rec -or -not $rec.pid) { continue }
        $pr = Get-Process -Id ([int]$rec.pid) -ErrorAction SilentlyContinue
        if (-not $pr) { continue }
        if ($rec.pid_start) { try { if ([int64]$pr.StartTime.ToUniversalTime().Ticks -ne [int64]$rec.pid_start) { continue } } catch { } }
        if (& $own ([int]$rec.pid)) { continue }
        $items.Add(@{ Kind = 'build'; Text = "run-lock build slot '$($rec.name)' pid $($rec.pid)"; Key = "build:$($rec.pid)" })
    }
    # a live admit slot: byte 0 locked (FileStream.Lock = the same Win32 lock admit.exe and python's msvcrt take); the record from byte 1
    foreach ($ad in @($Cfg.AdmitDirs)) {
        if (-not $ad -or -not [System.IO.Directory]::Exists($ad)) { continue }
        foreach ($f in [System.IO.Directory]::GetFiles($ad)) {
            $leaf = [System.IO.Path]::GetFileName($f)
            if ($leaf -notmatch '^[A-Za-z0-9_.-]+\.\d{1,3}$') { continue }
            $live = $false; $raw = ''
            $fs = $null
            try {
                $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite, 1)
                try { $fs.Lock(0, 1); $fs.Unlock(0, 1) }
                catch [System.IO.IOException] {
                    $live = $true
                    try { $fs.Position = 1; $buf = New-Object byte[] 4096; $n = $fs.Read($buf, 0, 4096); if ($n -gt 0) { $raw = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n) } } catch { }
                }
            } catch [System.IO.FileNotFoundException] { } catch [System.IO.DirectoryNotFoundException] { } catch { $live = $true }   # opened without sharing: someone has it
            finally { if ($fs) { $fs.Dispose() } }
            if (-not $live) { continue }
            $rec = $null
            if ($raw) { try { $rec = $raw | ConvertFrom-Json } catch { } }
            if ($rec) {
                $mine = $false
                foreach ($id in @($rec.pid, $rec.parent, $rec.for_pid)) { if ($id -and (& $own ([int]$id))) { $mine = $true } }
                if ($mine) { continue }
            }
            $what = "admit $leaf"
            if ($rec) {
                $what += " pid $($rec.pid)"
                if ($rec.for_pid) { $what += " for pid $($rec.for_pid)" }
                $lab = if ($rec.prog) { $rec.prog } elseif ($rec.check) { $rec.check } else { '' }
                if ($lab) { $what += " '$lab'" }
            }
            $items.Add(@{ Kind = 'admit'; Text = $what; Key = "admit:$leaf" })
        }
    }
    $cc = 0
    foreach ($i in $items) { if ($i.Kind -ne 'game') { $cc++ } }
    return @{ Games = $games; Compiles = $cc; Items = $items.ToArray() }
}

# "clang++.exe x3, admit cc.tool.0 pid 10836 ..." (compilers of one name grouped)
function Format-RunLockAloneItems($Items) {
    $parts = @()
    $cc = @{}
    foreach ($i in @($Items)) {
        if ($i.Kind -eq 'cc') { $cc[$i.Name] = 1 + [int]$cc[$i.Name]; $cc["$($i.Name)#pid"] = $i.Text; continue }
        $parts += $i.Text
    }
    foreach ($k in ($cc.Keys | Where-Object { $_ -notlike '*#pid' } | Sort-Object)) { $parts += $(if ($cc[$k] -gt 1) { "$k x$($cc[$k])" } else { "$($cc["$k#pid"])" }) }
    return ($parts -join ', ')
}

# This process's ancestor chain (a build tool that started the alone run is "own" too), parent creation time before the child's (a reused pid ends the chain).
function Get-RunLockAloneAncestors([int]$Id) {
    $out = @()
    try {
        $tab = @{}
        foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, CreationDate -ErrorAction Stop)) {
            $tab[[int]$p.ProcessId] = @{ pp = [int]$p.ParentProcessId; born = $(if ($p.CreationDate) { [datetime]$p.CreationDate } else { [datetime]::MinValue }) }
        }
        $cur = $Id
        for ($k = 0; $k -lt 16; $k++) {
            if (-not $tab.ContainsKey($cur)) { break }
            $e = $tab[$cur]
            if (-not $tab.ContainsKey($e.pp) -or $tab[$e.pp].born -ge $e.born) { break }
            $out += $e.pp
            $cur = $e.pp
        }
    } catch { }
    return @($out)
}

function New-RunLockAloneCfg($HolderPids) {
    $admit = if ($env:WWE_ADMIT_DIR) { $env:WWE_ADMIT_DIR } else { Join-Path (Split-Path $script:RunLockDir -Parent) 'admit' }
    @{ HolderPids = @($HolderPids); AncestorPids = @(Get-RunLockAloneAncestors $PID); OwnCache = [hashtable]::Synchronized(@{}); AdmitDir0 = $admit; GameNames = @($script:RunLockGameNames); CompilerNames = @($script:RunLockCompilerNames)
       BuildFiles = @(1..$script:RunLockMaxBuild | ForEach-Object { Get-RunLockBuildFile $_ }); AdmitDirs = @($admit) }
}

# The sampler: a runspace thread of THIS process (it ends with it). Every $IntervalMs it scans and keeps what it saw in the synchronized $State.
$script:RunLockAloneWatchSource = @'
param($State, $Cfg, $FnText, $Tsv, $IntervalMs)
. ([scriptblock]::Create($FnText))
$Test = if ("$($env:WWE_ADMIT_TEST)" -eq '1') { "$($env:WWE_ALONE_TEST)" } else { '' }   # S106 C5FF N3: the test hooks act only under WWE_ADMIT_TEST=1
if ($Test -eq 'tsvbad') { $Tsv = Join-Path $Tsv 'no_such_folder\x.tsv' }   # S106 C5FF N4 test hook: every tsv write fails
while (-not $State.Stop) {
    if ($Test -eq 'die' -and $State.ThreadSamples -ge 1) { break }   # S106 C5F B2 test hook: the sampler thread ends on its own after one sample
    try {
        if ($Test -eq 'throws') { throw 'planted: every scan throws' }   # S106 C5F B2 test hook
        if ($Test -eq 'throw_once' -and -not $State.OnceDone) { $State.OnceDone = $true; throw 'planted: the first scan throws' }   # S106 C5FF N4 test hook
        $b = Get-RunLockAloneBusy -Cfg $Cfg
        $now = Get-Date
        $State.Samples = [int]$State.Samples + 1
        $State.ThreadSamples = [int]$State.ThreadSamples + 1   # S106 C5F B2: samples taken BY THE THREAD (the final look on the caller's thread is not one)
        $State.Last = $now
        if ($b.Items.Count) {
            if (-not $State.Broken) { $State.Broken = @{ At = $now; Items = @{}; Last = $now } }
            $State.Broken.Last = $now
            foreach ($i in $b.Items) { $State.Broken.Items[$i.Key] = $i }
        }
        $State.ScanErrRun = 0   # S106 C5FF N4: a good scan ends a run of failures
        try {   # the tsv is a record for humans: a write error is counted apart and never makes the watch unverified (the sightings are in $State.Broken)
            $line = ("{0:HH:mm:ss.fff}`t{1}`t{2}`t{3}" -f $now, $b.Games, $b.Compiles, (($b.Items | ForEach-Object { $_.Text }) -join '; ')) + "`n"
            [System.IO.File]::AppendAllText($Tsv, $line)
        } catch { $State.TsvFails = [int]$State.TsvFails + 1; $State.TsvErr = $_.Exception.Message }
    } catch {
        # S106 C5FF N4: counted, not latched: one transient scan error (a process that vanished mid-scan) is noise; 3 in a row is a blind watch (Format-RunLockAloneLine)
        $State.ScanFails = [int]$State.ScanFails + 1
        $State.ScanErrRun = [int]$State.ScanErrRun + 1
        if ([int]$State.ScanErrRun -gt [int]$State.ScanErrMax) { $State.ScanErrMax = [int]$State.ScanErrRun }
        $State.ScanErr = $_.Exception.Message
    }
    for ($k = 0; $k -lt ($IntervalMs / 50) -and -not $State.Stop; $k++) { Start-Sleep -Milliseconds 50 }
}
'@

# S106 C5FF N3: the planted-fault hooks (WWE_ALONE_TEST) are honoured only with WWE_ADMIT_TEST=1, as admit.exe gates its plants on its test mode
function Get-RunLockAloneTestHook { if ("$($env:WWE_ADMIT_TEST)" -eq '1') { return "$($env:WWE_ALONE_TEST)" } return '' }

function Test-RunLockAloneHeldBy([string]$Token) {
    # the pause file is locked and its record names this token (an ancestor already took the pause)
    $f = Get-RunLockAloneFilePath
    if (-not $Token -or -not [System.IO.File]::Exists($f)) { return $false }
    $fs = $null
    try {
        $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete, 1)
        try { $fs.Lock(0, 1); $fs.Unlock(0, 1); return $false } catch [System.IO.IOException] { }
        $fs.Position = 1; $buf = New-Object byte[] 4096; $n = $fs.Read($buf, 0, 4096)
        if ($n -le 0) { return $false }
        $rec = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n) | ConvertFrom-Json
        return ("$($rec.token)" -eq $Token)
    } catch { return $false } finally { if ($fs) { $fs.Dispose() } }
}

# Takes the pause, drains, starts the sampler. Returns @{ Ok; Line } ; on Ok the handle carries .Alone (state for Exit-RunLockAlone and the lines).
function Enter-RunLockAlone {
    param($Handle, [double]$DrainSeconds = 300, [double]$RunMinutes = 15, [double]$QueueWaited = 0, [string]$Token = '')
    $t0 = Get-Date
    if (-not $Token) { $Token = "$($Handle.Token)" }   # (the run-lock token: history rows only)
    $name = "$($Handle.Name)"
    if ($name.Length -gt 200) { $name = $name.Substring(0, 200) }   # S106 C5F note 1: the record stays far under admit's 8 KB read and our 4 KB read
    $aloneTok = [guid]::NewGuid().ToString('N')   # S106 C5F note 3: a DEDICATED token (env WWE_ALONE_TOKEN, honoured by admit.exe / admit_py / Admit.ps1): only this process's tree carries it
    $prevAloneTok = $env:WWE_ALONE_TOKEN
    $f = Get-RunLockAloneFilePath
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($f))
    $fs = $null
    for ($k = 0; $k -lt 60 -and -not $fs; $k++) {   # (admit's scans lock byte 0 for an instant: a few tries)
        try {
            $try = [System.IO.FileStream]::new($f, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete, 1)
            try { $try.Lock(0, 1); $fs = $try } catch { $try.Dispose(); Start-Sleep -Milliseconds 50 }
        } catch { Start-Sleep -Milliseconds 50 }
    }
    if (-not $fs) {
        $line = "alone: NO after 0 s (could not take the pause file ${f}: another process holds it): not run"
        Write-RunLockHistory -Kind 'alone_refused' -Mode 'exclusive' -Name $name -Games 0 -Token $Token -Note $line
        return @{ Ok = $false; Line = $line }
    }
    $nowS = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $untilS = $nowS + [long][Math]::Min(1800, $DrainSeconds + $RunMinutes * 60)
    $rec = [ordered]@{ pid = $PID; pid_start = (Get-RunLockProcStart $PID); token = $aloneTok; name = $name; since = $nowS; until = $untilS }
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($rec | ConvertTo-Json -Compress))
        $fs.SetLength(1)
        $fs.Position = 1
        $fs.Write($bytes, 0, $bytes.Length)
        $fs.Flush($true)
    } catch {
        # S106 C5F note 1: admit reads a locked file without a parseable record as NO pause, so a record that did not get written is no pause: refuse
        $m = $_.Exception.Message
        try { $fs.SetLength(0) } catch { }
        $fs.Dispose()
        $line = "alone: NO after 0 s (could not write the pause record to ${f}: $m): not run"
        Write-RunLockHistory -Kind 'alone_refused' -Mode 'exclusive' -Name $name -Games 0 -Token $Token -Note $line
        return @{ Ok = $false; Line = $line }
    }
    $env:WWE_ALONE_TOKEN = $aloneTok
    $aok = $false   # S106 C5FF N2: set once the function returns on its own (success, or a refusal that already cleaned up)
    # S106 main (C5FF re-review note b): the finally below must never reach a CALLER's $state / $ps / $rs (PowerShell's dynamic scope) when
    # the drain throws before they are assigned
    $state = $null; $ps = $null; $rs = $null; $async = $null
    try {
    # S106 C5F note 4: one pass through admit's machine lock after the pause is taken: a grant that was in flight when the pause began has finished (its slot is
    # on disk for the drain to see); every later grant looks at the pause inside that lock and is refused
    $barrierOk = $true
    if ((Get-RunLockAloneTestHook) -ne 'nobarrier') { $barrierOk = Invoke-RunLockAloneAdmitBarrier }
    $holders = @($PID)
    $cfg = New-RunLockAloneCfg $holders
    $said = $false; $lastSay = $t0
    $b = Get-RunLockAloneBusy -Cfg $cfg
    while ($b.Items.Count) {
        $el = ((Get-Date) - $t0).TotalSeconds
        if ($el -ge $DrainSeconds) {
            $n = $b.Compiles
            $line = "alone: NO after $([int][Math]::Round($el)) s (games $($b.Games); compiles ${n}: $(Format-RunLockAloneItems ($b.Items | Where-Object { $_.Kind -ne 'game' }))" +
                $(if ($b.Games) { "; games: $(Format-RunLockAloneItems ($b.Items | Where-Object { $_.Kind -eq 'game' }))" } else { '' }) + '): not run'
            try { $fs.SetLength(0) } catch { }
            $fs.Dispose()
            $env:WWE_ALONE_TOKEN = $prevAloneTok
            $aok = $true
            Write-RunLockHistory -Kind 'alone_refused' -Mode 'exclusive' -Name $name -Waited $el -Games $b.Games -Token $Token -Note $line
            return @{ Ok = $false; Line = $line }
        }
        if (-not $said -or ((Get-Date) - $lastSay).TotalSeconds -ge 30) {
            if (-not $said) { $said = $true }
            $lastSay = Get-Date
            Write-RunLockMsg "[runlock] alone: waiting for the PC to be free (games $($b.Games), compiles $($b.Compiles)): $(Format-RunLockAloneItems $b.Items)"
        }
        Start-Sleep -Seconds 1
        $b = Get-RunLockAloneBusy -Cfg $cfg
    }
    $drained = ((Get-Date) - $t0).TotalSeconds
    $since = Get-Date
    $tsv = Join-Path $script:RunLockDir ("alone_{0}.tsv" -f $Token.Substring(0, [Math]::Min(8, $Token.Length)))
    $state = [hashtable]::Synchronized(@{ Stop = $false; Samples = 0; ThreadSamples = 0; Broken = $null; Last = $null; Err = ''; ScanFails = 0; ScanErrRun = 0; ScanErrMax = 0; ScanErr = ''; TsvFails = 0; TsvErr = ''; OnceDone = $false })
    $ps = $null; $rs = $null; $async = $null
    try {
        if ((Get-RunLockAloneTestHook) -eq 'nostart') { throw 'planted: the sampler cannot start' }   # S106 C5F B2 test hook
        $rs = [runspacefactory]::CreateRunspace()
        $rs.Open()
        $ps = [powershell]::Create()
        $ps.Runspace = $rs
        $fnText = 'function Get-RunLockAloneBusy {' + "`n" + ${function:Get-RunLockAloneBusy}.ToString() + "`n}"
        [void]$ps.AddScript($script:RunLockAloneWatchSource).AddArgument($state).AddArgument($cfg).AddArgument($fnText).AddArgument($tsv).AddArgument(2000)
        $async = $ps.BeginInvoke()
    } catch { Write-RunLockMsg "[runlock] warning: the alone sampler did not start ($($_.Exception.Message)): the end line says so" ; $state.Err = "sampler did not start: $($_.Exception.Message)" }
    # S106 C5F note 5: does the INSTALLED admit.exe honour the pause? (an older exe does not: it grants compiles during the run, unseen until the sampler notices)
    $honour = Get-RunLockAloneAdmitHonours
    $untilTxt = [DateTimeOffset]::FromUnixTimeSeconds($untilS).ToLocalTime().ToString('HH:mm:ss')
    $yes = "alone: yes (games 0, compiles 0, since $($since.ToString('HH:mm:ss')), pause until $untilTxt; waited $([int][Math]::Round($QueueWaited)) s for games and run-lock holders, $([int][Math]::Round($drained)) s for compiles; $honour$(if (-not $barrierOk) { "; the barrier timed out: admit's machine lock stayed busy for 5 s, so a grant in flight may not be on disk yet (the sampler is the watch)" } else { '' }))"
    $Handle | Add-Member -NotePropertyName Alone -NotePropertyValue @{ Stream = $fs; State = $state; Ps = $ps; Rs = $rs; Async = $async; Since = $since; Tsv = $tsv; Cfg = $cfg
        YesLine = $yes; Token = $Token; Name = $name; PrevAloneToken = $prevAloneTok } -Force
    Write-RunLockHistory -Kind 'alone' -Mode 'exclusive' -Name $name -Waited ($QueueWaited + $drained) -Games 0 -Token $Token -Note $yes
    $aok = $true
    return @{ Ok = $true; Line = $yes }
    } finally {
        # S106 C5F note 2 / C5FF N2: anything that leaves before the function returns on its own (a throw in the drain, a Ctrl+C: a stopped pipeline runs `finally`, never `catch`)
        # gives the pause back; the caller (Enter-RunLock) gives the exclusive lock back the same way
        if (-not $aok) {
            try { $state.Stop = $true } catch { }
            try { if ($ps) { $ps.Dispose() } } catch { }
            try { if ($rs) { $rs.Dispose() } } catch { }
            try { $fs.SetLength(0) } catch { }
            try { $fs.Dispose() } catch { }
            $env:WWE_ALONE_TOKEN = $prevAloneTok
        }
    }
}

# The installed admit.exe: out\tools\admit\admit.exe of the main checkout ($env:WWE_ADMIT_EXE for tests). `admit capabilities` names alone_pause; an older exe answers its usage.
function Get-RunLockAloneAdmitHonours {
    try {
        $exe = if ($env:WWE_ADMIT_EXE) { $env:WWE_ADMIT_EXE } else { Join-Path (Split-Path (Split-Path $script:RunLockDir -Parent) -Parent) 'tools\admit\admit.exe' }
        if (-not (Test-Path -LiteralPath $exe)) { return 'no admit.exe installed: compiles are not queued, the sampler is the only watch' }
        $psi = [System.Diagnostics.ProcessStartInfo]::new($exe)
        $psi.ArgumentList.Add('capabilities')
        $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $so = $p.StandardOutput.ReadToEndAsync()
        if (-not $p.WaitForExit(10000)) { try { $p.Kill() } catch { }; return 'admit.exe did not answer `capabilities` in 10 s: pause not confirmed (rebuild: tools\admit\build_admit.ps1)' }
        if ($p.ExitCode -eq 0 -and $so.Result -match 'alone_pause') { return 'admit.exe honours the pause' }
        return 'admit.exe does NOT honour the pause (an older build): compiles are granted during the run; rebuild it with tools\admit\build_admit.ps1'
    } catch { return "admit.exe not checked ($($_.Exception.Message))" }
}

# One pass through admit's machine lock (<admit dir>\admit.lock, byte 0, the lock admit.exe takes around a grant), bounded to 5 s, never throws.
function Invoke-RunLockAloneAdmitBarrier {   # S106 C5FF N5: returns $false when it gave up after 5 s (a grant may still be in flight), $true otherwise
    try {
        $ad = if ($env:WWE_ADMIT_DIR) { $env:WWE_ADMIT_DIR } else { Join-Path (Split-Path $script:RunLockDir -Parent) 'admit' }
        $lf = Join-Path $ad 'admit.lock'
        if (-not [System.IO.File]::Exists($lf)) { return $true }
        $fs = [System.IO.FileStream]::new($lf, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete, 1)
        try {
            $t = Get-Date
            while (((Get-Date) - $t).TotalSeconds -lt 5) {
                try { $fs.Lock(0, 1); $fs.Unlock(0, 1); return $true } catch [System.IO.IOException] { Start-Sleep -Milliseconds 20 }
            }
            return $false
        } finally { $fs.Dispose() }
    } catch { return $false }   # S106 main (C5FF re-review note c): a barrier that could not run is NOT a pass: the yes line names it
    return $true
}

# The sample line so far (`held` / `unverified` / `broken`), from the sampler's state. $Final = the run is over.
# S106 C5F B2: `held` only when the sampler is proven alive: no error, at least one sample taken by the THREAD, the last one within 3 intervals (6 s). Anything else
# is `unverified` (the PC was not watched): the readout is not judged, like `broken`.
function Format-RunLockAloneLine($Handle, [switch]$Final) {
    $a = $Handle.Alone
    if (-not $a) { return '' }
    $st = $a.State
    $span = "$($a.Since.ToString('HH:mm:ss'))-$((Get-Date).ToString('HH:mm:ss'))"
    if ($st.Broken) {
        $b = $st.Broken
        return "alone: broken at $($b.At.ToString('HH:mm:ss')) ($(Format-RunLockAloneItems @($b.Items.Values)), seen $($b.At.ToString('HH:mm:ss'))-$($b.Last.ToString('HH:mm:ss'))): readout not judged"
    }
    $why = @()
    if ($st.Err) { $why += "sampler: $($st.Err)" }
    # S106 C5FF N4: a run of 3 failed scans is a blind watch; one transient failure is not (the count is shown whenever the line is unverified anyway)
    if ([int]$st.ScanErrMax -ge 3) { $why += "sampler: $([int]$st.ScanErrMax) scans in a row failed (last: $($st.ScanErr))" }
    if ([int]$st.ThreadSamples -lt 1) { $why += $(if ([int]$st.ScanFails) { "the sampler thread has taken no sample ($([int]$st.ScanFails) scan error(s), last: $($st.ScanErr))" } else { 'the sampler thread has taken no sample' }) }
    elseif ($st.Last -and ((Get-Date) - [datetime]$st.Last).TotalSeconds -gt 6) { $why += "the sampler's last sample was $([int]((Get-Date) - [datetime]$st.Last).TotalSeconds) s ago (it died?)" }
    elseif ($a.Async -and $a.Async.IsCompleted -and -not $st.Stop) { $why += 'the sampler thread ended' }
    if ($why.Count) { return "alone: unverified ($($why -join '; '); $span): readout not judged" }
    $tail = if ($Final) { '' } else { ' so far' }
    return "alone: held (games 0, compiles 0, $span, $([int]$st.ThreadSamples) samples)$tail"
}

# One last sample, the end line, the history row, the pause closed. Never throws.
function Exit-RunLockAlone {
    param($Handle)
    try {
        $a = $Handle.Alone
        if (-not $a) { return }
        # a run shorter than the sampler's first scan: give the thread (at most 3 s) the chance to take its first sample, so a healthy short run is not `unverified`
        $w0 = Get-Date
        while ([int]$a.State.ThreadSamples -lt 1 -and -not $a.State.Err -and [int]$a.State.ScanErrMax -lt 3 -and $a.Async -and -not $a.Async.IsCompleted -and ((Get-Date) - $w0).TotalSeconds -lt 3) { Start-Sleep -Milliseconds 50 }
        $a.State.Stop = $true
        try { if ($a.Ps) { [void]$a.Ps.BeginStop($null, $null) } } catch { }
        try {
            $b = Get-RunLockAloneBusy -Cfg $a.Cfg   # one last look, on this thread
            if ($b.Items.Count) {
                $now = Get-Date
                if (-not $a.State.Broken) { $a.State.Broken = @{ At = $now; Items = @{}; Last = $now } }
                $a.State.Broken.Last = $now
                foreach ($i in $b.Items) { $a.State.Broken.Items[$i.Key] = $i }
            }
            $a.State.Samples = [int]$a.State.Samples + 1
        } catch { }
        $line = Format-RunLockAloneLine $Handle -Final
        $Handle.Alone = $null
        Write-RunLockMsg $line
        Write-RunLockHistory -Kind 'alone_end' -Mode 'exclusive' -Name $a.Name -Games 0 -Token $a.Token -Note $line
        try { $a.Stream.SetLength(0) } catch { }
        try { $a.Stream.Dispose() } catch { }
        $env:WWE_ALONE_TOKEN = $a.PrevAloneToken   # S106 C5F note 3: the dedicated token goes with the run
        try { if ($a.Ps) { $a.Ps.Dispose() } } catch { }
        try { if ($a.Rs) { $a.Rs.Dispose() } } catch { }
        if (-not $a.State.Broken) { Remove-RunLockFileQuiet $a.Tsv }
    } catch { }
}
