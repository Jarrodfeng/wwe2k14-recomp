# S106 C4F: fixes from docs\research\s106\review_C4.md (dot-sourced by lane_exe_lib.ps1; plain functions, nothing here builds or locks).

# F6: a lane / folder name starts with a letter or digit (so '.', '..', '-x' and '.hidden' can never reach out\exe itself or climb).
function Test-LaneName([string]$Name) { return [bool]($Name -match '^[A-Za-z0-9][A-Za-z0-9_.-]{0,59}$') }

# B1: the slot claim is an OPEN HANDLE on <slot>.owner (read-share only), held for the whole build. The OS frees it when the holder dies, so a dead
# holder is never "deleted"; two claimants can never hold it together. F1: the owner also names the build child (child=N); a live child (a killed
# wrapper's build keeps running) keeps the slot busy. Returns the open FileStream (keep it; Dispose when done) or $null (busy).
function Test-LaneChildAlive([string]$Text) {
    if ($Text -match 'child=(\d+)') {
        $p = Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -match '^(pwsh|powershell|cmake|ninja|cl|clang.*)$') {
            # S106 C4F2 F2: the owner also holds the child's start time; a recycled pid has another one.
            if ($Text -match 'childstart=(\d+)') {
                $t = 0L; try { $t = $p.StartTime.ToUniversalTime().Ticks } catch { return $true }
                if ([Math]::Abs($t - [long]$Matches[1]) -gt 20000000) { return $false }
            }
            return $true
        }
    }
    return $false
}
function Open-LaneSlotClaim([string]$Owner, [string]$Name) {
    [void][System.IO.Directory]::CreateDirectory((Split-Path $Owner -Parent))
    try { $fs = [System.IO.File]::Open($Owner, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read) } catch { return $null }
    $old = ''
    try { $buf = New-Object byte[] ([int][Math]::Min($fs.Length, 4096)); [void]$fs.Read($buf, 0, $buf.Length); $old = [System.Text.Encoding]::ASCII.GetString($buf) } catch { }
    if (Test-LaneChildAlive $old) { $fs.Dispose(); return $null }
    $fs.SetLength(0); $fs.Position = 0
    $b = [System.Text.Encoding]::ASCII.GetBytes("pid=$PID`r`nname=$Name`r`n"); $fs.Write($b, 0, $b.Length); $fs.Flush()
    return $fs
}
# F1: record the build child's pid in the owner (rewrites the owner through the held handle).
function Set-LaneSlotChild($Stream, [string]$Name, [int]$ChildPid) {
    $Stream.SetLength(0); $Stream.Position = 0
    $st = 0L; try { $st = (Get-Process -Id $ChildPid -ErrorAction Stop).StartTime.ToUniversalTime().Ticks } catch { }
    $b = [System.Text.Encoding]::ASCII.GetBytes("pid=$PID`r`nname=$Name`r`nchild=$ChildPid`r`n$(if ($st) { "childstart=$st`r`n" })"); $Stream.Write($b, 0, $b.Length); $Stream.Flush()
}
# S106 C4F2 F2: after the build the owner holds only pid= (no child= left to be matched against a recycled pid).
function Clear-LaneSlotChild($Stream) {
    $Stream.SetLength(0); $Stream.Position = 0
    $b = [System.Text.Encoding]::ASCII.GetBytes("pid=$PID`r`n"); $Stream.Write($b, 0, $b.Length); $Stream.Flush()
}
# S106 C4F2 B1: does this build.ps1 take -LockMode? (A plain param() script drops unknown named arguments into $args, so the build would ask for EXCLUSIVE inside our BUILD hold.)
function Test-LaneBuildHasLockMode([string]$Script) {
    try { return [bool](Get-Command $Script -CommandType ExternalScript -ErrorAction Stop).Parameters.ContainsKey('LockMode') } catch { return $false }
}
# S106 C4F2 F1: wait for the build child for at most $Minutes; on timeout kill it with its whole tree. Returns TimedOut / ExitCode.
function Wait-LaneBuild($Proc, [double]$Minutes) {
    $ms = [int][Math]::Min([Math]::Max($Minutes * 60000, 1), 2000000000)
    if ($Proc.WaitForExit($ms)) { $Proc.WaitForExit(); return [pscustomobject]@{ TimedOut = $false; ExitCode = $Proc.ExitCode } }
    & taskkill /T /F /PID $Proc.Id 2>&1 | Out-Null
    [void]$Proc.WaitForExit(15000)
    return [pscustomobject]@{ TimedOut = $true; ExitCode = -1 }
}
# Busy = a live holder has the handle, or the owner names a live build child. Writes nothing (an absent owner = free, never created).
function Test-LaneSlotBusy([string]$Owner) {
    if (-not (Test-Path -LiteralPath $Owner)) { return $false }
    try { $fs = [System.IO.File]::Open($Owner, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read) } catch { return $true }
    try { $buf = New-Object byte[] ([int][Math]::Min($fs.Length, 4096)); [void]$fs.Read($buf, 0, $buf.Length); $old = [System.Text.Encoding]::ASCII.GetString($buf) } finally { $fs.Dispose() }
    return (Test-LaneChildAlive $old)
}
# F8: claim a slot: prefer one already on this commit, then a configured one; poll (PollSeconds) until WaitSeconds when none is free. Slots: objects with N, Dir, Owner.
function Get-LaneSlotClaim($Slots, [string]$Preset, [string]$Full, [string]$Name, [int]$Only = 0, [double]$WaitSeconds = 0, [double]$PollSeconds = 10) {
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ($true) {
        $scored = @($Slots | Where-Object { $Only -eq 0 -or $_.N -eq $Only } | ForEach-Object {
            $h = ''; if (Test-Path -LiteralPath "$($_.Dir)\.git" -PathType Leaf) { $h = "$(& git -C $_.Dir rev-parse HEAD 2>$null)".Trim() }
            $k = if ($h -eq $Full) { 0 } elseif (Test-Path -LiteralPath "$($_.Dir)\out\build\$Preset\CMakeCache.txt") { 1 } else { 2 }
            [pscustomobject]@{ S = $_; K = $k; N = $_.N } } | Sort-Object K, N)
        foreach ($c in $scored) { $fs = Open-LaneSlotClaim $c.S.Owner $Name; if ($fs) { return [pscustomobject]@{ Slot = $c.S; Stream = $fs } } }
        if ((Get-Date) -ge $deadline) { return $null }
        Start-Sleep -Seconds $PollSeconds
    }
}

# B2: publish a staged folder. Never removes or writes into an existing non-empty folder: an empty one is deleted non-recursively, then the staging folder is
# RENAMED in ([IO.Directory]::Move fails when the target exists). On a failed move the folder is re-judged: Reuse (same commit / preset / options) or Refuse.
# Result: Published | Reuse | Refuse. The staging folder is left for the caller to drop unless Published.
function Publish-LaneFolder([string]$Temp, [string]$Folder, [string]$Commit, [string]$Preset, $Options) {
    $st = Get-LaneFolderState $Folder $Commit $Preset $Options
    if ($st.State -eq 'Reuse') { return [pscustomobject]@{ Result = 'Reuse'; Why = $st.Why } }
    if ($st.State -eq 'Refuse') { return [pscustomobject]@{ Result = 'Refuse'; Why = $st.Why } }
    try {
        if (Test-Path -LiteralPath $Folder) { [System.IO.Directory]::Delete($Folder, $false) }   # only an empty folder can be here; fails (never recursive) when someone filled it
        [System.IO.Directory]::Move($Temp, $Folder)
        return [pscustomobject]@{ Result = 'Published'; Why = '' }
    } catch {
        $again = Get-LaneFolderState $Folder $Commit $Preset $Options
        if ($again.State -eq 'Reuse') { return [pscustomobject]@{ Result = 'Reuse'; Why = "another builder published it first: $($again.Why)" } }
        return [pscustomobject]@{ Result = 'Refuse'; Why = "could not publish $Folder ($($_.Exception.Message)); it now reads: $($again.State) $($again.Why)" }
    }
}

# F2: is $Sl one of $Main's registered worktrees (git worktree list --porcelain)?
function ConvertTo-LaneNorm([string]$P) { return ([System.IO.Path]::GetFullPath($P) -replace '/', '\').TrimEnd('\').ToLowerInvariant() }
function Test-LaneSlotRegistered([string]$Main, [string]$Sl) {
    $want = ConvertTo-LaneNorm $Sl
    foreach ($l in @(& git -C $Main worktree list --porcelain 2>$null)) { if ("$l" -match '^worktree (.+)$' -and (ConvertTo-LaneNorm $Matches[1]) -eq $want) { return $true } }
    return $false
}
# F2 + F7: everything that can touch a slot, in a safe order. Nothing is checked out / cleaned / added until the slot passed: no reparse point at the slot root
# or at out\wt, the slot is a worktree whose common dir is $Main\.git and is registered, and the tree holds no link. A missing slot is pruned then added (F7).
# Returns Ok, Why, Cold.
function Initialize-LaneSlot([string]$Main, [string]$Sl, [string]$Full) {
    $r = [pscustomobject]@{ Ok = $false; Why = ''; Cold = $false }
    foreach ($p in @("$Main\out\wt", $Sl)) {
        if ((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { $r.Why = "reparse point at $p (a slot path is never a junction or symlink)"; return $r }
    }
    if (Test-Path -LiteralPath $Sl) {
        if (-not (Test-Path -LiteralPath "$Sl\.git" -PathType Leaf)) { $r.Why = "$Sl exists but is not a git worktree"; return $r }
        $cd = "$(& git -C $Sl rev-parse --git-common-dir 2>$null)".Trim()
        $cdFull = if ($cd -and [System.IO.Path]::IsPathRooted($cd)) { $cd } elseif ($cd) { Join-Path $Sl $cd } else { '' }
        if (-not $cdFull -or (ConvertTo-LaneNorm $cdFull) -ne (ConvertTo-LaneNorm "$Main\.git")) { $r.Why = "$Sl is not a worktree of the main checkout $Main (common dir '$cd')"; return $r }
        if (-not (Test-LaneSlotRegistered $Main $Sl)) { $r.Why = "$Sl is not a registered worktree of $Main"; return $r }
        $links = @(Test-LaneSlotLinks $Sl)
        if ($links.Count) { $r.Why = "the slot holds $($links.Count) link(s) (first: $($links[0])): a slot is link-free"; return $r }
        & git -C $Sl checkout --detach --force $Full *>&1 | Select-Object -Last 3
        & git -C $Sl clean -fdq *>&1 | Select-Object -Last 3
    } else {
        & git -C $Main worktree prune 2>&1 | Select-Object -Last 3
        [void][System.IO.Directory]::CreateDirectory("$Main\out\wt")
        & git -C $Main worktree add --detach $Sl $Full *>&1 | Select-Object -Last 3
        $r.Cold = $true
    }
    if ("$(& git -C $Sl rev-parse HEAD 2>$null)".Trim() -ne $Full) { $r.Why = "checkout of $Full in $Sl failed"; return $r }
    $r.Ok = $true
    return $r
}

# F1 + EXEB-A: generated\default is copied ONCE to <dst>.copying, verified (count + bytes), renamed, then marked (<dst>.seeded). A folder without the marker is dropped
# and copied again (a killed copy never leaves something that looks complete); one with it is left to the slot's own codegen. Returns Copied, Kept, Why, Error.
function Sync-LaneGenerated([string]$Src, [string]$Dst) {
    $r = [pscustomobject]@{ Copied = $false; Kept = $false; Why = ''; Error = '' }
    $tmp = "$Dst.copying"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    $mark = "$Dst.seeded"
    # S108 EXEB-A: seeded ONCE. A folder with its marker is the slot's own from then on (its codegen rewrites codegen.d / the stamp, so a byte compare with main
    # was always false and the full codegen reran every build). The marker sits beside the folder, outside what codegen and the count / bytes test see.
    if ((Test-Path -LiteralPath $Dst -PathType Container) -and (Test-Path -LiteralPath $mark -PathType Leaf)) { $r.Kept = $true; $r.Why = "the slot's own codegen owns it"; return $r }
    $r.Why = if (Test-Path -LiteralPath $Dst) { 'no seed marker: a folder of unknown origin' } else { 'no folder yet (new or cleaned slot)' }
    if (Test-Path -LiteralPath $mark) { Remove-Item -LiteralPath $mark -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $Dst) { Remove-Item -LiteralPath $Dst -Recurse -Force -ErrorAction SilentlyContinue }
    [void][System.IO.Directory]::CreateDirectory($tmp)
    & robocopy $Src $tmp /E /XJ /NFL /NDL /NJH /NJS /NP /MT:8 /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; $r.Error = "robocopy of generated failed (exit $LASTEXITCODE)"; return $r }
    $bad = Test-LaneGeneratedCopy $Src $tmp
    if ($bad) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; $r.Error = "$bad (copy removed)"; return $r }
    [System.IO.Directory]::Move($tmp, $Dst)
    $n = Get-LaneTreeSize $Dst
    [System.IO.File]::WriteAllText($mark, "seeded from main: files $($n.Files) bytes $($n.Bytes) at $((Get-Date).ToString('s'))`r`n")
    $r.Copied = $true
    return $r
}

# F3 / F4: WWE2K14_* options from ANY CMakeCache.txt (the slot's own, or main's). BOOL ON / OFF go to the map; other BOOL values to $Skipped.
function Get-LaneCacheOptions([string]$Cache, [ref]$Skipped) {
    $o = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Cache)) { return $o }
    foreach ($l in [System.IO.File]::ReadLines($Cache)) {
        if ($l -match '^(WWE2K14_[A-Za-z0-9_]+):BOOL=(.*)$') {
            if ($Matches[2] -in 'ON', 'OFF') { $o[$Matches[1]] = $Matches[2] } elseif ($Skipped) { $Skipped.Value += "$($Matches[1])=$($Matches[2])" }
        }
    }
    return $o
}
# The NON-EMPTY WWE2K14_* STRING / FILEPATH options (GUEST_ISA, OURS_ISA, GUEST_PGO_USE, VERSION_SUFFIX): recorded in exe_info; main's must be empty or the build refuses.
function Get-LaneCacheStrings([string]$Cache) {
    $o = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Cache)) { return $o }
    foreach ($l in [System.IO.File]::ReadLines($Cache)) { if ($l -match '^(WWE2K14_[A-Za-z0-9_]+):(STRING|FILEPATH|PATH)=(.+)$') { $o[$Matches[1]] = $Matches[3] } }
    return $o
}
# F4: one symbols recipe for every lane exe (the main session's decision, S106 11:4x): symbols OFF, guest lines OFF, whatever main's cache says.
# S111: and the preset's own identity, never main's cache (main's out\build\win-amd64-release is the stream-FREE player build since 2026-11-26; tip_exe's tip-stream exe is built from that preset and must keep the stream).
function Get-LaneWantedOptions($MainOptions, [string]$Preset = '') {
    $w = [ordered]@{}
    foreach ($k in $MainOptions.Keys) { $w[$k] = $MainOptions[$k] }
    $w['WWE2K14_EXE_SYMBOLS'] = 'OFF'; $w['WWE2K14_EXE_SYMBOLS_GUEST_LINES'] = 'OFF'
    if ($Preset -eq 'win-amd64-release') { $w['WWE2K14_KEEP_STREAM'] = 'ON'; $w['WWE2K14_KEEP_STREAM_RUN'] = 'OFF' }
    elseif ($Preset -eq 'win-amd64-release-off') { $w['WWE2K14_KEEP_STREAM'] = 'OFF'; $w['WWE2K14_KEEP_STREAM_RUN'] = 'ON' }
    return $w
}
# The wanted keys whose value in $Have differs or is absent.
function Get-LaneOptionDiff($Have, $Want, $Ignore = @()) {
    foreach ($k in $Want.Keys) { if ($k -in $Ignore) { continue }; if (-not $Have.Contains($k) -or "$($Have[$k])" -ne "$($Want[$k])") { $k } }
}
# S106 C4F2 F4: keys the cache holds as :UNINITIALIZED (a -D of an option the commit's CMakeLists does not declare). Such a wanted key is absent in that commit, not a difference.
function Get-LaneCacheUninitialized([string]$Cache) {
    if (-not (Test-Path -LiteralPath $Cache)) { return }
    foreach ($l in [System.IO.File]::ReadLines($Cache)) { if ($l -match '^(WWE2K14_[A-Za-z0-9_]+):UNINITIALIZED=') { $Matches[1] } }
}
