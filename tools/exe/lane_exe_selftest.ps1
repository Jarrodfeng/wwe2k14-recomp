# S106 C4: proof of tools\exe\lane_exe.ps1 without a build, a lock or a game: each refusal is PLANTED once (it must be refused) and its
# clean twin (the unplanted arm) must pass. Scratch lives under <worktree>\out\c4\selftest (gitignored), never in the main checkout.
#   pwsh -NoProfile -File tools\exe\lane_exe_selftest.ps1        -> exit 0 only when every row is PASS
param([string]$Main = 'C:\Users\Jarrod\wwe2k14-recomp')
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$myBuild = (Resolve-Path (Join-Path $PSScriptRoot '..\build.ps1')).Path
$tree = (Resolve-Path (Join-Path $here '..\..')).Path.TrimEnd('\')
. "$here\lane_exe_lib.ps1"
$scratch = Join-Path $tree 'out\c4\selftest'
$script:fails = 0; $script:rows = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    $script:rows++
    if ($Ok) { "PASS  $Name" } else { $script:fails++; "FAIL  $Name  $Detail" }
}
function Remove-Scratch([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    # a junction is removed as a link first (never follow it), then the folder
    foreach ($r in (Get-ChildItem -LiteralPath $Dir -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)) { [System.IO.Directory]::Delete($r.FullName) }
    Remove-Item -LiteralPath $Dir -Recurse -Force -ErrorAction SilentlyContinue
}
function New-FakeMain([string]$Dir, [switch]$NoStreamline) {
    [void][System.IO.Directory]::CreateDirectory("$Dir\.git")
    foreach ($i in (Get-LaneExeInputs)) {
        if ($NoStreamline -and $i.Name -eq 'streamline') { continue }
        $p = Join-Path $Dir $i.Source
        if ($i.Kind -eq 'file') { [void][System.IO.Directory]::CreateDirectory((Split-Path $p -Parent)); Set-Content -LiteralPath $p -Value 'x' }
        else { [void][System.IO.Directory]::CreateDirectory($p); Set-Content -LiteralPath "$p\a.h" -Value 'x' }
    }
}
Remove-Scratch $scratch
[void][System.IO.Directory]::CreateDirectory($scratch)

# 1. links in a slot: a junction and a hard link are found; a clean slot is clean; the tool itself never makes one.
$slot = "$scratch\slot"; [void][System.IO.Directory]::CreateDirectory("$slot\generated"); [void][System.IO.Directory]::CreateDirectory("$slot\thirdparty"); [void][System.IO.Directory]::CreateDirectory("$scratch\target")
Set-Content -LiteralPath "$slot\generated\f.txt" -Value 'x'
Check 'links: a clean slot is link-free' ((@(Test-LaneSlotLinks $slot)).Count -eq 0)
$null = New-Item -ItemType Junction -Path "$slot\thirdparty\jn" -Target "$scratch\target"
$r = @(Test-LaneSlotLinks $slot)
Check 'links: a junction in the slot is refused' ($r.Count -eq 1 -and $r[0] -like 'reparse point:*') ($r -join '; ')
[System.IO.Directory]::Delete("$slot\thirdparty\jn")
$null = New-Item -ItemType HardLink -Path "$slot\generated\hl.txt" -Target "$slot\generated\f.txt"
$r = @(Test-LaneSlotLinks $slot)
Check 'links: a hard link in generated\ is refused' ($r.Count -ge 1 -and ($r -join ' ') -like '*hard link:*') ($r -join '; ')
$txt = (Get-Content -LiteralPath "$here\lane_exe.ps1" -Raw) + (Get-Content -LiteralPath "$here\lane_exe_lib.ps1" -Raw)
Check 'links: lane_exe.ps1 / lib never ask for a junction, symlink or hard link' ($txt -notmatch '(?i)-ItemType\s+(Junction|HardLink|SymbolicLink)|mklink|fsutil\s+hardlink')

# 2. streamline missing in the source: refused (positive twin: all inputs present = empty list); the script refuses with exit 6 and names it.
New-FakeMain "$scratch\main_ok"
Check 'sources: a complete main has no missing input' ((@(Test-LaneExeSources "$scratch\main_ok")).Count -eq 0)
New-FakeMain "$scratch\main_nosl" -NoStreamline
$m = @(Test-LaneExeSources "$scratch\main_nosl")
Check 'sources: missing thirdparty\streamline is named' ($m.Count -eq 1 -and $m[0] -like 'streamline:*') ($m -join '; ')
$out = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Main "$scratch\main_nosl" -Lane selftest -Commit HEAD 2>&1 | Out-String
Check 'sources: lane_exe.ps1 -DryRun refuses it (exit 6, names streamline)' ($LASTEXITCODE -eq 6 -and $out -match 'REFUSED' -and $out -match 'streamline') "exit $LASTEXITCODE"

# 3. a lane folder whose exe differs (never replaced in place); the same folder with a matching sha is reused; an absent one is new.
$opt = [ordered]@{ WWE2K14_FOO = 'ON' }
$fol = "$scratch\exe\lane-x-abc"; [void][System.IO.Directory]::CreateDirectory($fol)
[System.IO.File]::WriteAllBytes("$fol\wwe2k14.exe", [byte[]](65, 65, 65))
$good = [ordered]@{ commit = 'c1'; preset = 'p1'; options = $opt; sha256 = (Get-LaneFileSha "$fol\wwe2k14.exe") }
($good | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath "$fol\exe_info.json"
Check 'folder: same commit, preset, options and exe = Reuse' ((Get-LaneFolderState $fol 'c1' 'p1' $opt).State -eq 'Reuse')
Check 'folder: absent = New' ((Get-LaneFolderState "$scratch\exe\nope" 'c1' 'p1' $opt).State -eq 'New')
[System.IO.File]::WriteAllBytes("$fol\wwe2k14.exe", [byte[]](66, 66, 66))
$s = Get-LaneFolderState $fol 'c1' 'p1' $opt
Check 'folder: an exe that differs from exe_info.json is refused' ($s.State -eq 'Refuse' -and $s.Why -like '*differs*') $s.Why
[System.IO.File]::WriteAllBytes("$fol\wwe2k14.exe", [byte[]](65, 65, 65))
Check 'folder: another commit is refused' ((Get-LaneFolderState $fol 'c2' 'p1' $opt).State -eq 'Refuse')
Check 'folder: another preset is refused' ((Get-LaneFolderState $fol 'c1' 'p2' $opt).State -eq 'Refuse')
Check 'folder: other WWE2K14_* options are refused' ((Get-LaneFolderState $fol 'c1' 'p1' ([ordered]@{ WWE2K14_FOO = 'OFF' })).State -eq 'Refuse')
Remove-Item -LiteralPath "$fol\exe_info.json"
Check 'folder: files but no exe_info.json is refused' ((Get-LaneFolderState $fol 'c1' 'p1' $opt).State -eq 'Refuse')

# 4. build.ps1 -LockMode Build on the main checkout: refused before anything (a nonexistent preset makes any slip fail too, with another message).
$o = & pwsh -NoProfile -File "$here\..\build.ps1" -Root $Main -LockMode Build -Preset selftest-no-such-preset 2>&1 | Out-String
Check 'build.ps1: -LockMode Build with -Root = the main checkout is refused (exit 6)' ($LASTEXITCODE -eq 6 -and $o -match 'LockMode Build on the main checkout') "exit $LASTEXITCODE : $($o.Trim())"
$o = & pwsh -NoProfile -File "$here\..\build.ps1" -Root $tree -LockMode Build -Preset selftest-no-such-preset 2>&1 | Out-String
Check 'build.ps1: the same flag in a worktree passes that check (refused later, by the missing tree)' ($LASTEXITCODE -eq 6 -and $o -notmatch 'LockMode Build on the main checkout' -and $o -match 'lacks') "exit $LASTEXITCODE : $($o.Trim())"

# 5. a generated copy one file short, one byte off, and an exact copy.
$g = "$scratch\gen_src"; $d = "$scratch\gen_dst"
[void][System.IO.Directory]::CreateDirectory("$g\sub"); 1..5 | ForEach-Object { Set-Content -LiteralPath "$g\sub\f$_.cpp" -Value ('x' * $_) }
Copy-Item -LiteralPath $g -Destination $d -Recurse
Check 'generated: an exact copy passes' ($null -eq (Test-LaneGeneratedCopy $g $d))
Remove-Item -LiteralPath "$d\sub\f3.cpp"
$w = Test-LaneGeneratedCopy $g $d
Check 'generated: a copy one file short is refused' ($w -like '*differs*files 4 vs 5*') "$w"
Copy-Item -LiteralPath "$g\sub\f3.cpp" -Destination "$d\sub\f3.cpp"; Add-Content -LiteralPath "$d\sub\f3.cpp" -Value 'more'
$w = Test-LaneGeneratedCopy $g $d
Check 'generated: a copy one file longer (torn / other bytes) is refused' ($w -like '*differs*bytes*') "$w"
Check 'generated: a missing copy is refused' ($null -ne (Test-LaneGeneratedCopy $g "$scratch\nope"))

# 6. the plan for a real commit on the real main checkout (read-only: no lock, no write).
$out = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -Main $Main -BuildScript $myBuild 2>&1 | Out-String
Check 'dry run: a real commit prints a plan and exits 0' ($LASTEXITCODE -eq 0 -and $out -match 'plan ok' -and $out -match 'build.ps1 -Root <slot>' -and $out -match '-LockMode Build') "exit $LASTEXITCODE : $out"
Check 'dry run: the plan names the folder, the slots and the copies' ($out -match 'out\\exe\\lane-selftest-' -and $out -match 'slot 3:' -and $out -match 'thirdparty\\streamline')
$out | ForEach-Object { $_ -split "`r?`n" } | Where-Object { $_ } | Select-Object -First 16 | ForEach-Object { "    | $_" }

# ---- S106 C4F: the fix round (review_C4.md B1 B2 F1 F2 F3 F4 F5 F6 F7 F8). Every row is written against the lib functions; a clean twin follows each planted arm.
function New-FakeRepo([string]$Dir) {
    [void][System.IO.Directory]::CreateDirectory($Dir)
    & git -C $Dir init -q 2>&1 | Out-Null
    Set-Content -LiteralPath "$Dir\tracked.txt" -Value 'v1'; & git -C $Dir add tracked.txt 2>&1 | Out-Null
    & git -C $Dir -c user.name=t -c user.email=t@t commit -q -m c1 2>&1 | Out-Null; $c1 = "$(& git -C $Dir rev-parse HEAD)".Trim()
    Set-Content -LiteralPath "$Dir\tracked.txt" -Value 'v2'; & git -C $Dir add tracked.txt 2>&1 | Out-Null
    & git -C $Dir -c user.name=t -c user.email=t@t commit -q -m c2 2>&1 | Out-Null; $c2 = "$(& git -C $Dir rev-parse HEAD)".Trim()
    [void][System.IO.Directory]::CreateDirectory("$Dir\out\wt")
    return [pscustomobject]@{ C1 = $c1; C2 = $c2 }
}
function Run-Row([scriptblock]$B) { try { & $B } catch { Check "row threw" $false $_.Exception.Message } }

# B1: two claimants on one slot. A holds the claim in a child process (blocked on stdin); B (a second child) must be refused; kill A, then B succeeds.
$cs = "$scratch\claim_child.ps1"
Set-Content -LiteralPath $cs -Value @'
param([string]$Lib, [string]$Owner, [string]$Name, [switch]$Hold)
. $Lib
$fs = Open-LaneSlotClaim $Owner $Name
if ($fs) { 'CLAIMED'; [Console]::Out.Flush(); if ($Hold) { [void][Console]::In.ReadLine() } } else { 'BUSY' }
'@
function Start-Child([string]$Name, [switch]$Hold) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo; $psi.FileName = 'pwsh'; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true
    foreach ($a in @('-NoProfile', '-File', $cs, '-Lib', "$here\lane_exe_lib.ps1", '-Owner', "$scratch\slot1.owner", '-Name', $Name)) { [void]$psi.ArgumentList.Add($a) }
    if ($Hold) { [void]$psi.ArgumentList.Add('-Hold') }
    return [System.Diagnostics.Process]::Start($psi)
}
Run-Row {
    $a = Start-Child 'A' -Hold; $la = $a.StandardOutput.ReadLine()
    $b = Start-Child 'B'; $lb = $b.StandardOutput.ReadLine(); $b.WaitForExit()
    Check 'claim: A claims a free slot' ($la -eq 'CLAIMED') "got '$la'"
    Check 'claim: B is refused while A holds the slot (two claimants never share one)' ($lb -eq 'BUSY') "got '$lb'"
    $a.Kill(); $a.WaitForExit()
    $b2 = Start-Child 'B2'; $lb2 = $b2.StandardOutput.ReadLine(); $b2.WaitForExit()
    Check 'claim: after A is killed the OS frees the slot and B2 claims it (no dead-owner delete)' ($lb2 -eq 'CLAIMED') "got '$lb2'"
}
Run-Row {
    $fs = Open-LaneSlotClaim "$scratch\slot2.owner" 'x'
    $held = Test-LaneSlotBusy "$scratch\slot2.owner"; $fs.Dispose()
    Check 'claim: the probe sees a held slot busy and a released one free' ($held -and -not (Test-LaneSlotBusy "$scratch\slot2.owner"))
    Check 'claim: a slot with no owner file is free (a probe writes nothing)' (-not (Test-LaneSlotBusy "$scratch\never.owner") -and -not (Test-Path "$scratch\never.owner"))
}
# F1 (killed wrapper): the owner names the build child; a live child keeps the slot busy even when nobody holds the handle.
Run-Row {
    $dead = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -Wait -NoNewWindow; $deadPid = $dead.Id
    [System.IO.File]::WriteAllText("$scratch\slot3.owner", "pid=$deadPid`r`nname=x`r`nchild=$PID`r`n")
    $x = Open-LaneSlotClaim "$scratch\slot3.owner" 'y'
    Check 'claim: a live build child recorded in the owner keeps the slot busy' ($null -eq $x -and (Test-LaneSlotBusy "$scratch\slot3.owner"))
    [System.IO.File]::WriteAllText("$scratch\slot3.owner", "pid=$deadPid`r`nname=x`r`nchild=$deadPid`r`n")
    $x = Open-LaneSlotClaim "$scratch\slot3.owner" 'y'
    Check 'claim: a dead build child does not' ($null -ne $x); if ($x) { $x.Dispose() }
}
# F8: the selector. Free slots are claimed in order; every slot held = $null at once with WaitSeconds 0 (the caller polls until its lock deadline).
Run-Row {
    $sl = @(1..2 | ForEach-Object { [pscustomobject]@{ N = $_; Dir = "$scratch\nos$_"; Owner = "$scratch\sel$_.owner" } })
    $g1 = Get-LaneSlotClaim $sl 'p' 'c' 'n' 0 0 1
    $g2 = Get-LaneSlotClaim $sl 'p' 'c' 'n' 0 0 1
    $g3 = Get-LaneSlotClaim $sl 'p' 'c' 'n' 0 0 1
    Check 'select: two claims get slots 1 and 2, a third finds none' ($g1.Slot.N -eq 1 -and $g2.Slot.N -eq 2 -and $null -eq $g3)
    $g1.Stream.Dispose(); $g2.Stream.Dispose()
    $g4 = Get-LaneSlotClaim $sl 'p' 'c' 'n' 2 0 1
    Check 'select: -Slot 2 only ever gets slot 2' ($g4.Slot.N -eq 2); if ($g4) { $g4.Stream.Dispose() }
}

# B2: a published folder is never removed or written into.
function Get-FolderHashes([string]$D) { (Get-ChildItem -LiteralPath $D -Recurse -File -Force | Sort-Object FullName | ForEach-Object { "$($_.FullName.Substring($D.Length)):$((Get-FileHash $_.FullName).Hash)" }) -join '|' }
function New-Staging([string]$Dir, [string]$Commit, [string]$Body) {
    [void][System.IO.Directory]::CreateDirectory("$Dir\stamp"); [System.IO.File]::WriteAllText("$Dir\wwe2k14.exe", $Body); Set-Content -LiteralPath "$Dir\stamp\s.h" -Value $Body
    ([ordered]@{ commit = $Commit; preset = 'p1'; options = $opt; sha256 = (Get-LaneFileSha "$Dir\wwe2k14.exe") } | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath "$Dir\exe_info.json"
}
Run-Row {
    $pub = "$scratch\pub\lane-x-abc"; New-Staging "$scratch\pub\.t1" 'c1' 'first'
    $r1 = Publish-LaneFolder "$scratch\pub\.t1" $pub 'c1' 'p1' $opt
    Check 'publish: a new name is published (the staging folder becomes it)' ($r1.Result -eq 'Published' -and (Test-Path "$pub\wwe2k14.exe") -and -not (Test-Path "$scratch\pub\.t1")) "$($r1.Result) $($r1.Why)"
    $h = Get-FolderHashes $pub
    New-Staging "$scratch\pub\.t2" 'c1' 'second, other bytes'
    $r2 = Publish-LaneFolder "$scratch\pub\.t2" $pub 'c1' 'p1' $opt
    Check 'publish: the same name again, same commit, other staging = Reuse; the first stays byte-identical, nothing written into it' ($r2.Result -eq 'Reuse' -and (Get-FolderHashes $pub) -eq $h -and (Test-Path "$scratch\pub\.t2\wwe2k14.exe")) "$($r2.Result) $($r2.Why)"
    New-Staging "$scratch\pub\.t3" 'c2' 'third'
    $r3 = Publish-LaneFolder "$scratch\pub\.t3" $pub 'c2' 'p1' $opt
    Check 'publish: the same name for another commit is refused; the first stays byte-identical' ($r3.Result -eq 'Refuse' -and (Get-FolderHashes $pub) -eq $h) "$($r3.Result) $($r3.Why)"
    [void][System.IO.Directory]::CreateDirectory("$scratch\pub\emptyone"); New-Staging "$scratch\pub\.t4" 'c1' 'fourth'
    $r4 = Publish-LaneFolder "$scratch\pub\.t4" "$scratch\pub\emptyone" 'c1' 'p1' $opt
    Check 'publish: an existing EMPTY folder is replaced (non-recursive delete, then rename)' ($r4.Result -eq 'Published' -and (Test-Path "$scratch\pub\emptyone\wwe2k14.exe")) "$($r4.Result) $($r4.Why)"
    $src = [System.IO.File]::ReadAllText("$here\lane_exe.ps1")
    Check 'publish: lane_exe.ps1 never Remove-Items the final folder, stages per pid, and re-checks the folder state after the lock' ($src -notmatch 'Remove-Item[^\r\n]*\$folder\b' -and $src -match '\.\$Name\.\$PID\.tmp' -and $src.IndexOf('Get-LaneFolderState', $src.IndexOf('Enter-RunLock')) -gt 0)
}

# F2: a junction at the slot root, one at out\wt, a slot of another repo, a clone: all refused BEFORE any checkout / clean / copy (the planted target is unchanged).
Run-Row {
    $fm = "$scratch\fm1"; $c = New-FakeRepo $fm
    $victim = "$fm\out\wt\victim"; & git -C $fm worktree add --detach $victim $c.C1 2>&1 | Out-Null
    Set-Content -LiteralPath "$victim\tracked.txt" -Value 'DIRTY'; Set-Content -LiteralPath "$victim\untracked.txt" -Value 'keep'
    $null = New-Item -ItemType Junction -Path "$fm\out\wt\exe-slot1" -Target $victim
    $r = Initialize-LaneSlot $fm "$fm\out\wt\exe-slot1" $c.C2
    Check 'slot: a junction at the slot root is refused' (-not $r.Ok -and $r.Why -like '*reparse*') "$($r.Ok) $($r.Why)"
    Check 'slot: ... and the planted target is untouched (no checkout --force, no clean -fd)' ((Get-Content "$victim\tracked.txt") -eq 'DIRTY' -and (Test-Path "$victim\untracked.txt") -and "$(& git -C $victim rev-parse HEAD)".Trim() -eq $c.C1)
    $o2 = "$scratch\fm_other"; $c2 = New-FakeRepo $o2
    & git -C $o2 worktree add --detach "$fm\out\wt\exe-slot2" $c2.C1 2>&1 | Out-Null
    Set-Content -LiteralPath "$fm\out\wt\exe-slot2\untracked.txt" -Value 'keep'
    $r = Initialize-LaneSlot $fm "$fm\out\wt\exe-slot2" $c.C2
    Check 'slot: a worktree of ANOTHER repo (not registered with this main) is refused' (-not $r.Ok -and $r.Why -like '*main*') "$($r.Ok) $($r.Why)"
    Check 'slot: ... and its files are untouched' ((Test-Path "$fm\out\wt\exe-slot2\untracked.txt") -and "$(& git -C "$fm\out\wt\exe-slot2" rev-parse HEAD)".Trim() -eq $c2.C1)
    $cl = "$fm\out\wt\exe-slot3"; [void][System.IO.Directory]::CreateDirectory($cl); & git -C $cl init -q 2>&1 | Out-Null; Set-Content -LiteralPath "$cl\untracked.txt" -Value 'keep'
    $r = Initialize-LaneSlot $fm $cl $c.C2
    Check 'slot: a clone (.git is a folder) in the slot path is refused and untouched' (-not $r.Ok -and (Test-Path "$cl\untracked.txt")) "$($r.Ok) $($r.Why)"
    Check 'slot: Test-LaneSlotRegistered: a registered slot yes, an unregistered path no' ((Test-LaneSlotRegistered $fm $victim) -and -not (Test-LaneSlotRegistered $fm "$fm\out\wt\nothing"))
    $fm2 = "$scratch\fm2"; [void](New-FakeRepo $fm2); [System.IO.Directory]::Delete("$fm2\out\wt"); [void][System.IO.Directory]::CreateDirectory("$scratch\wt_target")
    $null = New-Item -ItemType Junction -Path "$fm2\out\wt" -Target "$scratch\wt_target"
    $r = Initialize-LaneSlot $fm2 "$fm2\out\wt\exe-slot1" $c.C1
    Check 'slot: a junction at out\wt is refused and nothing is created in its target' (-not $r.Ok -and $r.Why -like '*reparse*' -and -not (Get-ChildItem -LiteralPath "$scratch\wt_target" -Force | Select-Object -First 1)) "$($r.Ok) $($r.Why)"
    $fm3 = "$scratch\fm3"; $c3 = New-FakeRepo $fm3
    $s3 = "$fm3\out\wt\exe-slot1"
    $r = Initialize-LaneSlot $fm3 $s3 $c3.C1
    Check 'slot: clean twin: a new slot is added (cold) at the commit' ($r.Ok -and $r.Cold -and "$(& git -C $s3 rev-parse HEAD)".Trim() -eq $c3.C1) "$($r.Ok) $($r.Why)"
    $r = Initialize-LaneSlot $fm3 $s3 $c3.C2
    Check 'slot: clean twin: a registered slot is moved to another commit (warm)' ($r.Ok -and -not $r.Cold -and "$(& git -C $s3 rev-parse HEAD)".Trim() -eq $c3.C2) "$($r.Ok) $($r.Why)"
    Remove-Item -LiteralPath $s3 -Recurse -Force
    $r = Initialize-LaneSlot $fm3 $s3 $c3.C1
    Check 'slot: F7: a slot folder deleted without prune is re-added' ($r.Ok -and "$(& git -C $s3 rev-parse HEAD)".Trim() -eq $c3.C1) "$($r.Ok) $($r.Why)"
}

# F1 (generated copy): a partial copy at the final path, with no completion evidence, is copied again; a leftover .copying is dropped; an exact copy is left alone.
Run-Row {
    $g = "$scratch\gen2_src"; $d = "$scratch\gen2\default"
    [void][System.IO.Directory]::CreateDirectory("$g\sub"); 1..5 | ForEach-Object { Set-Content -LiteralPath "$g\sub\f$_.cpp" -Value ('x' * $_) }
    [void][System.IO.Directory]::CreateDirectory("$d\sub"); 1..2 | ForEach-Object { Copy-Item "$g\sub\f$_.cpp" "$d\sub\" }
    [void][System.IO.Directory]::CreateDirectory("$d.copying"); Set-Content -LiteralPath "$d.copying\junk" -Value 'x'
    $r = Sync-LaneGenerated $g $d
    Check 'generated: a partial copy at the final path is copied again (the old Files -gt 0 accepted it)' ($r.Copied -and -not $r.Error -and $null -eq (Test-LaneGeneratedCopy $g $d) -and -not (Test-Path "$d.copying")) "$($r | Out-String)"
    Check 'generated: the copy is marked seeded (marker beside the folder, outside it)' ((Test-Path "$d.seeded" -PathType Leaf) -and -not (Test-Path "$d\default.seeded") -and -not (Test-Path "$d.seeded" -PathType Container))
    $r = Sync-LaneGenerated $g $d
    Check 'generated: clean twin: a seeded exact copy is left alone' (-not $r.Copied -and $r.Kept -and -not $r.Error)
    # EXEB-A: after the slot's own codegen the bytes never equal main's; the seeded folder is KEPT (the old byte compare re-copied it every build).
    Add-Content -LiteralPath "$d\sub\f1.cpp" -Value 'the slot codegen wrote longer paths'; Set-Content -LiteralPath "$d\codegen.d" -Value 'slot paths'
    $mine = (Get-FileHash "$d\sub\f1.cpp").Hash
    $r = Sync-LaneGenerated $g $d
    Check 'generated: EXEB-A: a seeded folder whose bytes differ from main is KEPT, not re-copied' (-not $r.Copied -and $r.Kept -and -not $r.Error -and (Get-FileHash "$d\sub\f1.cpp").Hash -eq $mine -and (Test-Path "$d\codegen.d")) "$($r | Out-String)"
    # no marker (a new or pre-marker slot) = copied again and marked, even when the folder is a full copy.
    Remove-Item -LiteralPath "$d.seeded" -Force
    $r = Sync-LaneGenerated $g $d
    Check 'generated: EXEB-A: no marker = the folder is copied again from main and marked' ($r.Copied -and -not $r.Kept -and (Test-Path "$d.seeded") -and $null -eq (Test-LaneGeneratedCopy $g $d) -and -not (Test-Path "$d\codegen.d")) "$($r | Out-String)"
    # a leftover .copying beside a seeded folder is dropped and the folder is still kept.
    [void][System.IO.Directory]::CreateDirectory("$d.copying"); Set-Content -LiteralPath "$d.copying\junk" -Value 'x'
    $r = Sync-LaneGenerated $g $d
    Check 'generated: EXEB-A: a leftover .copying is dropped; the seeded folder is kept' (-not $r.Copied -and $r.Kept -and -not (Test-Path "$d.copying"))
    # a missing folder with a stale marker is seeded again.
    Remove-Item -LiteralPath $d -Recurse -Force
    $r = Sync-LaneGenerated $g $d
    Check 'generated: EXEB-A: a stale marker with no folder = seeded again' ($r.Copied -and (Test-Path "$d.seeded") -and $null -eq (Test-LaneGeneratedCopy $g $d))
    $src = [System.IO.File]::ReadAllText("$here\lane_exe.ps1")
    Check 'generated: EXEB-A: lane_exe logs "generated: seeded from main" or "generated: kept" on every build' ($src -match 'generated: seeded from main' -and $src -match 'generated: kept')
}

# F3 / F4: options come from a cache file (the slot's own) and the symbols recipe is pinned OFF whatever main says.
Run-Row {
    $cf = "$scratch\CMakeCache.txt"
    Set-Content -LiteralPath $cf -Value @('WWE2K14_EXE_SYMBOLS:BOOL=ON', 'WWE2K14_EXE_SYMBOLS_GUEST_LINES:BOOL=$gl', 'WWE2K14_KEEP_STREAM_RUN:BOOL=ON', 'WWE2K14_VERSION_SUFFIX:STRING=', 'WWE2K14_GUEST_ISA:STRING=avx2')
    $sk = @(); $o = Get-LaneCacheOptions $cf ([ref]$sk); $st = Get-LaneCacheStrings $cf
    Check 'options: a cache file gives its BOOL options and the non-empty STRING / FILEPATH ones' ($o['WWE2K14_EXE_SYMBOLS'] -eq 'ON' -and $o['WWE2K14_KEEP_STREAM_RUN'] -eq 'ON' -and $st.Count -eq 1 -and $st['WWE2K14_GUEST_ISA'] -eq 'avx2' -and $sk.Count -eq 1) "$($o | Out-String) $($st | Out-String)"
    $w = Get-LaneWantedOptions $o
    Check 'options: the wanted recipe pins EXE_SYMBOLS and GUEST_LINES OFF (main said ON / $gl) and keeps the rest' ($w['WWE2K14_EXE_SYMBOLS'] -eq 'OFF' -and $w['WWE2K14_EXE_SYMBOLS_GUEST_LINES'] -eq 'OFF' -and $w['WWE2K14_KEEP_STREAM_RUN'] -eq 'ON')
    $ws = Get-LaneWantedOptions ([ordered]@{ WWE2K14_KEEP_STREAM = 'OFF'; WWE2K14_KEEP_STREAM_RUN = 'ON' }) 'win-amd64-release'
    $wo = Get-LaneWantedOptions ([ordered]@{ WWE2K14_KEEP_STREAM = 'ON'; WWE2K14_KEEP_STREAM_RUN = 'OFF' }) 'win-amd64-release-off'
    Check 'S111 options: a preset pins its own KEEP_STREAM identity over main cache (the play folder is stream-free: tip-stream stays ON, the -off preset stays OFF)' ($ws['WWE2K14_KEEP_STREAM'] -eq 'ON' -and $ws['WWE2K14_KEEP_STREAM_RUN'] -eq 'OFF' -and $wo['WWE2K14_KEEP_STREAM'] -eq 'OFF' -and $wo['WWE2K14_KEEP_STREAM_RUN'] -eq 'ON' -and $w['WWE2K14_KEEP_STREAM_RUN'] -eq 'ON')
    $diff = @(Get-LaneOptionDiff $o $w)
    Check 'options: a cache that differs from the wanted recipe names the differing keys (the slot is re-configured); an equal one names none' ($diff.Count -eq 2 -and @(Get-LaneOptionDiff $w $w).Count -eq 0) ($diff -join ',')
}

# F5 / F6: the session branch is a parameter; a name that could climb or hit out\exe itself is refused.
Run-Row {
    Check 'name: . .. -x .hidden and empty are refused; a lane name is accepted' (-not (Test-LaneName '.') -and -not (Test-LaneName '..') -and -not (Test-LaneName '-x') -and -not (Test-LaneName '.hidden') -and -not (Test-LaneName '') -and (Test-LaneName 'lane-base-abcdef123') -and (Test-LaneName 'a.b_c'))
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Name . -Commit s106 -Main $Main 2>&1 | Out-String
    Check 'name: lane_exe.ps1 -Name . is refused with exit 2' ($LASTEXITCODE -eq 2 -and $o -match 'REFUSED') "exit $LASTEXITCODE $o"
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Name selftest-x -Commit s106 -Main $Main -BuildScript $myBuild 2>&1 | Out-String
    Check 'name: clean twin: -Name selftest-x passes' ($LASTEXITCODE -eq 0 -and $o -match 'plan ok') "exit $LASTEXITCODE"
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -SessionBranch s106 -Main $Main -BuildScript $myBuild 2>&1 | Out-String
    Check 'branch: -SessionBranch is accepted and named; the plan shows the options source and the symbols pinned OFF' ($LASTEXITCODE -eq 0 -and $o -match 'merge-base with s106' -and $o -match 'WWE2K14_EXE_SYMBOLS=OFF' -and $o -match 'WWE2K14_EXE_SYMBOLS_GUEST_LINES=OFF' -and $o -match 'options source') "exit $LASTEXITCODE $o"
}

# ---- S106 C4F round 2 (review_round2.md: B1 F1 F2 F3 F4 + minor)
# B1: a main build.ps1 without -LockMode is refused (exit 6), also in a dry run; this branch's build.ps1 (it has it) passes.
Run-Row {
    $nb = "$scratch\nolock_build.ps1"; Set-Content -LiteralPath $nb -Value 'param([string]$Root, [string]$Preset, [string]$Run)'
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -Main $Main -BuildScript $nb 2>&1 | Out-String
    Check 'B1: a main build.ps1 without -LockMode is refused in a dry run (exit 6, names -LockMode)' ($LASTEXITCODE -eq 6 -and $o -match 'REFUSED' -and $o -match 'LockMode' -and $o -notmatch 'plan ok') "exit $LASTEXITCODE $o"
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -Main $Main -BuildScript $myBuild 2>&1 | Out-String
    Check 'B1: clean twin: a build.ps1 with -LockMode passes' ($LASTEXITCODE -eq 0 -and $o -match 'plan ok') "exit $LASTEXITCODE"
    Check 'B1: Test-LaneBuildHasLockMode says no / yes' (-not (Test-LaneBuildHasLockMode $nb) -and (Test-LaneBuildHasLockMode $myBuild))
}
# F1: a build child that outlives the timeout is killed with its whole tree; a quick one is not touched. The stub sleeps once and starts a grandchild.
Run-Row {
    $stub = "$scratch\stub_build.ps1"
    Set-Content -LiteralPath $stub -Value @'
param([int]$Seconds = 10, [string]$Marker)
$g = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', "Start-Sleep -Seconds $Seconds" -PassThru -NoNewWindow
Set-Content -LiteralPath $Marker -Value $g.Id
Start-Sleep -Seconds $Seconds
'@
    $mk = "$scratch\stub.marker"
    $bp = Start-Process pwsh -ArgumentList '-NoProfile', '-File', "`"$stub`"", '-Seconds', '10', '-Marker', "`"$mk`"" -NoNewWindow -PassThru
    $null = $bp.Handle
    for ($i = 0; $i -lt 100 -and -not (Test-Path -LiteralPath $mk); $i++) { Start-Sleep -Milliseconds 100 }
    $gpid = [int]("$(Get-Content -LiteralPath $mk -ErrorAction SilentlyContinue)".Trim())
    $r = Wait-LaneBuild $bp 0.01
    Start-Sleep -Milliseconds 500
    Check 'F1: a build that outlives the timeout is reported timed out and killed with its tree (child and grandchild gone)' ($r.TimedOut -and $bp.HasExited -and $gpid -gt 0 -and -not (Get-Process -Id $gpid -ErrorAction SilentlyContinue)) "$($r | Out-String) grandchild $gpid"
    $q = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'exit 3' -NoNewWindow -PassThru; $null = $q.Handle
    $r2 = Wait-LaneBuild $q 1
    Check 'F1: clean twin: a quick build is not timed out and its exit code comes back' (-not $r2.TimedOut -and $r2.ExitCode -eq 3) "$($r2 | Out-String)"
    $src = [System.IO.File]::ReadAllText("$here\lane_exe.ps1")
    Check 'F1: lane_exe.ps1 waits through Wait-LaneBuild, has -BuildTimeoutMinutes, and kills a live build child in finally' ($src -match 'Wait-LaneBuild' -and $src -match 'BuildTimeoutMinutes' -and $src -match '(?s)finally \{.*taskkill')
}
# F2: after the build the owner holds only pid= (a recycled child pid can never keep the slot busy).
Run-Row {
    $ow = "$scratch\slot4.owner"; $fs = Open-LaneSlotClaim $ow 'z'
    Set-LaneSlotChild $fs 'z' $PID
    $fs.Position = 0; $buf = New-Object byte[] 200; $n = $fs.Read($buf, 0, 200); $t1 = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    Clear-LaneSlotChild $fs
    $fs.Position = 0; $buf = New-Object byte[] 200; $n = $fs.Read($buf, 0, 200); $t2 = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    Check 'F2: the owner names the child while it builds, and after Clear-LaneSlotChild holds only pid=' ($t1 -match 'child=' -and $t2 -match '^pid=\d+\s*$' -and $t2 -notmatch 'child') "during '$t1' after '$t2'"
    $fs.Dispose()
    Check 'F2: a child recorded with another start time (a recycled pid) does not keep the slot busy' ((Test-LaneChildAlive "pid=1`r`nchild=$PID`r`nchildstart=1`r`n") -eq $false -and (Test-LaneChildAlive "pid=1`r`nchild=$PID`r`n") -eq $true)
    $src = [System.IO.File]::ReadAllText("$here\lane_exe.ps1")
    Check 'F2: lane_exe.ps1 clears the child before it disposes the claim' ($src -match '(?s)Clear-LaneSlotChild.*Dispose\(\)')
}
# F3: -Preset is a folder name under out\build: a path or a command separator is refused (exit 2) before anything.
Run-Row {
    foreach ($bad in @('..\..\x', 'a;b', '..', 'x y', 'a/b')) {
        $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -Main $Main -Preset $bad -BuildScript $myBuild 2>&1 | Out-String
        Check "F3: -Preset '$bad' is refused with exit 2" ($LASTEXITCODE -eq 2 -and $o -match 'REFUSED') "exit $LASTEXITCODE $o"
    }
    $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit s106 -Main $Main -Preset win-amd64-release-off -BuildScript $myBuild 2>&1 | Out-String
    Check 'F3: clean twin: win-amd64-release-off passes' ($LASTEXITCODE -eq 0 -and $o -match 'plan ok') "exit $LASTEXITCODE"
}
# F4: a wanted option the commit does not declare is stored UNINITIALIZED by the -D: recorded absent, not a difference.
Run-Row {
    $cf = "$scratch\CMakeCache_f4.txt"
    Set-Content -LiteralPath $cf -Value @('WWE2K14_EXE_SYMBOLS:BOOL=OFF', 'WWE2K14_EXE_SYMBOLS_GUEST_LINES:UNINITIALIZED=OFF', 'WWE2K14_NEWER_OPT:UNINITIALIZED=ON')
    $have = Get-LaneCacheOptions $cf ([ref]@())
    $want = [ordered]@{ WWE2K14_EXE_SYMBOLS = 'OFF'; WWE2K14_EXE_SYMBOLS_GUEST_LINES = 'OFF'; WWE2K14_NEWER_OPT = 'ON' }
    $un = @(Get-LaneCacheUninitialized $cf)
    Check 'F4: the UNINITIALIZED keys of a cache are listed' ($un.Count -eq 2 -and $un -contains 'WWE2K14_NEWER_OPT' -and $un -contains 'WWE2K14_EXE_SYMBOLS_GUEST_LINES') ($un -join ',')
    $d0 = @(Get-LaneOptionDiff $have $want); $d1 = @(Get-LaneOptionDiff $have $want $un)
    Check 'F4: such a key is ignored by the diff (not refused); without the list it differs' ($d1.Count -eq 0 -and $d0.Count -eq 2) "$($d0 -join ',') / $($d1 -join ',')"
    $want2 = [ordered]@{ WWE2K14_EXE_SYMBOLS = 'ON'; WWE2K14_NEWER_OPT = 'ON' }
    Check 'F4: clean twin: a real BOOL mismatch still differs when another key is absent' (((@(Get-LaneOptionDiff $have $want2 $un)) -join ',') -eq 'WWE2K14_EXE_SYMBOLS')
}
# minor: commits older than 704968263 are refused (exit 6); the session merge-base is named merge_base_session.
Run-Row {
    $oldc = "$(& git -C $tree rev-parse 704968263~1 2>$null)".Trim()
    if ($oldc) {
        $o = & pwsh -NoProfile -File "$here\lane_exe.ps1" -DryRun -Lane selftest -Commit $oldc -Main $Main -BuildScript $myBuild 2>&1 | Out-String
        Check 'old commit: one older than 704968263 is refused (exit 6)' ($LASTEXITCODE -eq 6 -and $o -match '704968263') "exit $LASTEXITCODE $o"
    } else { Check 'old commit: 704968263~1 resolves in this tree' $false 'not found' }
    $src = [System.IO.File]::ReadAllText("$here\lane_exe.ps1")
    Check 'name: exe_info.json says merge_base_session, not merge_base_s106' ($src -match 'merge_base_session' -and $src -notmatch 'merge_base_s106')
}

Remove-Scratch $scratch
"lane_exe_selftest: $($script:rows - $script:fails) / $($script:rows) rows passed"
if ($script:fails) { exit 1 }
exit 0
