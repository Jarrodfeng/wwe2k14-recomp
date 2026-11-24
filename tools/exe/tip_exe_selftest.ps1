# S109 WARM: proof of tools\exe\tip_exe.ps1 without a build, a lock, a game or the real admission queue (docs\research\s109\WARM_plan.md "Proof").
#   pwsh -NoProfile -File tools\exe\tip_exe_selftest.ps1        -> exit 0 only when every row is PASS
# A fake main checkout (git init under <this tree>\out\tip_selftest), a stub lane_exe (writes a folder whose exe holds the stamp hash, as a real build does), a stub
# rig build, and admit.exe replaced by tools\admit\wiring_stub_admit.py through WWE_ADMIT_EXE. Each planted twin runs a COPY of the runner with one rule broken
# and must FAIL the row its rule guards.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$tree = (Resolve-Path (Join-Path $here '..\..')).Path
$scratch = Join-Path $tree 'out\tip_selftest'
$fails = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { "PASS $Name" } else { "FAIL $Name$(if ($Detail) { "  -- $Detail" })"; $script:fails++ }
}
if (Test-Path $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
[void][IO.Directory]::CreateDirectory($scratch)
. "$here\tip_exe_lib.ps1"

# ---- stubs
$stubLane = Join-Path $scratch 'stub_lane_exe.ps1'
Set-Content -LiteralPath $stubLane -Value @'
param([string]$Name, [string]$Commit, [string]$Preset, [string]$Jobs, [string]$Main, [string]$LockWaitMinutes = '')
Add-Content -LiteralPath $env:STUB_LANE_LOG -Value "$Name $Commit $Preset jobs=$Jobs lockwait=$LockWaitMinutes"
if ($env:STUB_LANE_ENV) {   # WARM review R2: what a git hook's environment leaves the build, and what it does to lane_exe's git in a slot worktree
    & git -C $env:STUB_LANE_WT checkout -q --detach --force $Commit 2>$null; $gc = $LASTEXITCODE
    Add-Content -LiteralPath $env:STUB_LANE_ENV -Value "gif=[$env:GIT_INDEX_FILE] date=[$env:GIT_AUTHOR_DATE] slot_checkout=$gc"
}
if ($env:STUB_LANE_COMMIT -and -not (Test-Path -LiteralPath $env:STUB_LANE_COMMIT)) {   # HEAD moves while the FIRST build runs (the flag file marks it done)
    Set-Content -LiteralPath $env:STUB_LANE_COMMIT -Value done
    Add-Content -LiteralPath (Join-Path $Main 'src\a.cpp') -Value "// moved $(Get-Random)"
    & git -C $Main commit -q -am 'src moved during the build' | Out-Null
}
$code = if ($env:STUB_LANE_EXIT) { [int]$env:STUB_LANE_EXIT } else { 0 }
if ($code -ne 0) { "lane_exe: stub exit $code"; exit $code }
$c12 = $Commit.Substring(0, 12)
$d = Join-Path $Main "out\exe\$Name"
[void][IO.Directory]::CreateDirectory("$d\stamp")
[IO.File]::WriteAllBytes("$d\wwe2k14.exe", [Text.Encoding]::ASCII.GetBytes("MZ....$c12`0...."))
Set-Content -LiteralPath "$d\stamp\wwe2k14_build_stamp.h" -Value "#define WWE2K14_BUILD_GIT `"$c12`"`n#define WWE2K14_BUILD_DIRTY 0"
"LANE_EXE $d\wwe2k14.exe"
exit 0
'@
$stubRig = Join-Path $scratch 'stub_build_rig.ps1'
Set-Content -LiteralPath $stubRig -Value @'
Add-Content -LiteralPath $env:STUB_RIG_LOG -Value 'rig'
if ($env:STUB_RIG_EXIT) { '[admit] cc.tool build of 16: REFUSED after 3 s (this script stops: nothing was compiled)'; exit [int]$env:STUB_RIG_EXIT }
$m = $env:STUB_MAIN
[void][IO.Directory]::CreateDirectory("$m\out\tools")
Set-Content -LiteralPath "$m\out\tools\frame_rig.exe" -Value 'rig'
if ($env:STUB_RIG_COMMIT -and -not (Test-Path -LiteralPath $env:STUB_RIG_COMMIT)) {   # WARM review R4: a merge lands while the FIRST rig build compiles
    Set-Content -LiteralPath $env:STUB_RIG_COMMIT -Value done
    Add-Content -LiteralPath (Join-Path $m 'src\a.cpp') -Value "// merged during the rig build $(Get-Random)"
    & git -C $m commit -q -am 'src merged during the rig build' | Out-Null
}
Set-Content -LiteralPath "$m\out\tools\frame_rig.stamp" -Value "git $("$(& git -C $m rev-parse --short=12 HEAD)".Trim()) dirty 0"
exit 0
'@
$stubCheck = Join-Path $scratch 'stub_check.py'   # stands in for game_syntax.py and check.py: argv logged, exit / output from STUB_SYNTAX / STUB_HANDBACK
Set-Content -LiteralPath $stubCheck -Value @'
import os, sys
which = 'syntax' if '--build' in sys.argv else 'handback'
with open(os.environ['STUB_CHECK_LOG'], 'a') as f:
    f.write(which + ' ' + ' '.join(sys.argv[1:]) + '\n')
mode = os.environ.get('STUB_' + which.upper(), 'pass')
if mode == 'fail':
    print('FAIL L5 rf_props.cpp: an allowlist line is missing')
    sys.exit(1)
if mode == 'refused':
    print('[admit] syntax.worker: REFUSED after 180 s (exit 75)')
    sys.exit(75)
print('PASS')
'@
$admitCmd = Join-Path $scratch 'stub_admit.cmd'
$py = (Get-Command py).Source
Set-Content -LiteralPath $admitCmd -Value "@`"$py`" -I `"$tree\tools\admit\wiring_stub_admit.py`" %*" -Encoding ascii

function New-FakeMain([string]$Dir) {
    [void][IO.Directory]::CreateDirectory($Dir)
    & git -C $Dir init -q -b s109 2>&1 | Out-Null
    & git -C $Dir config user.email a@b; & git -C $Dir config user.name a
    foreach ($f in 'src\a.cpp', 'docs\n.md', 'tools\x.py', 'tools\scene\frame_rig.cpp') { [void][IO.Directory]::CreateDirectory((Split-Path (Join-Path $Dir $f) -Parent)); Set-Content -LiteralPath (Join-Path $Dir $f) -Value 'v1' }
    Set-Content -LiteralPath (Join-Path $Dir '.gitignore') -Value 'out/'
    & git -C $Dir add -A; & git -C $Dir commit -q -m one | Out-Null
    & git -C $Dir branch main | Out-Null   # the session's start point (the tip check's base when there is no last green yet)
}
function Commit-In([string]$Dir, [string]$File, [string]$Msg) {
    Add-Content -LiteralPath (Join-Path $Dir $File) -Value "// $Msg $(Get-Random)"
    & git -C $Dir commit -q -am $Msg | Out-Null
}
function Get-Head([string]$Dir) { "$(& git -C $Dir rev-parse HEAD)".Trim() }

# One run of a runner (the real one, or a planted copy) on fake main $M. -> @{ Code; Out; Lane (new lane calls); Rig (new rig calls); Secs }
function Invoke-Tip([string]$Runner, [string]$M, [string[]]$More = @(), [hashtable]$Env = @{}) {
    $env:WWE_ADMIT_EXE = $admitCmd; $env:STUB_LOG = Join-Path $scratch 'admit.log'; $env:STUB_MODE = 'grant 10'
    $env:STUB_LANE_LOG = Join-Path $M 'lane.log'; $env:STUB_RIG_LOG = Join-Path $M 'rig.log'; $env:STUB_MAIN = $M
    $env:STUB_LANE_EXIT = ''; $env:STUB_RIG_EXIT = ''; $env:STUB_LANE_COMMIT = ''; $env:STUB_CHECK_LOG = Join-Path $M 'check.log'; $env:STUB_SYNTAX = ''; $env:STUB_HANDBACK = ''
    foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
    $l0 = @(Get-Content -LiteralPath $env:STUB_LANE_LOG -ErrorAction SilentlyContinue).Count
    $r0 = @(Get-Content -LiteralPath $env:STUB_RIG_LOG -ErrorAction SilentlyContinue).Count
    $k0 = @(Get-Content -LiteralPath $env:STUB_CHECK_LOG -ErrorAction SilentlyContinue).Count
    $t = [Diagnostics.Stopwatch]::StartNew()
    $o = & pwsh -NoProfile -File $Runner -Main $M -LaneExe $stubLane -RigBuild $stubRig -SyntaxCheck $stubCheck -HandbackCheck $stubCheck @More 2>&1 | Out-String
    $c = $LASTEXITCODE
    $lane = @(Get-Content -LiteralPath $env:STUB_LANE_LOG -ErrorAction SilentlyContinue)
    $rig = @(Get-Content -LiteralPath $env:STUB_RIG_LOG -ErrorAction SilentlyContinue)
    $chk = @(Get-Content -LiteralPath $env:STUB_CHECK_LOG -ErrorAction SilentlyContinue)
    foreach ($k in 'WWE_ADMIT_EXE', 'STUB_LOG', 'STUB_MODE', 'STUB_LANE_LOG', 'STUB_RIG_LOG', 'STUB_MAIN', 'STUB_LANE_EXIT', 'STUB_RIG_EXIT', 'STUB_LANE_COMMIT', 'STUB_CHECK_LOG', 'STUB_SYNTAX', 'STUB_HANDBACK') { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    return @{ Code = $c; Out = $o; Lane = @($lane | Select-Object -Skip $l0); Rig = $rig.Count - $r0; Check = @($chk | Select-Object -Skip $k0); Secs = $t.Elapsed.TotalSeconds }
}
function Get-Ptr([string]$M, [string]$File = 'tip.json') { Read-TipPointer (Join-Path $M "out\exe\$File") }

# A planted copy: the runner, its lib, the judge and the admit helper copied into a scratch tree, one rule replaced. -> the copy's tip_exe.ps1
function New-Plant([string]$Name, [string]$File, [string]$From, [string]$To) {
    $p = Join-Path $scratch "plant_$Name"
    foreach ($s in 'tools\exe\tip_exe.ps1', 'tools\exe\tip_exe_lib.ps1', 'tools\bench\exe_vs_head.py', 'tools\admit\Admit.ps1', 'tools\admit\admit.json', 'tools\admit\tool_build_parallel.json') {
        [void][IO.Directory]::CreateDirectory((Split-Path (Join-Path $p $s) -Parent)); Copy-Item -LiteralPath (Join-Path $tree $s) -Destination (Join-Path $p $s)
    }
    $f = Join-Path $p $File
    $t = [IO.File]::ReadAllText($f)
    if (-not $t.Contains($From)) { throw "plant ${Name}: '$From' not found in $File" }
    [IO.File]::WriteAllText($f, $t.Replace($From, $To))
    return (Join-Path $p 'tools\exe\tip_exe.ps1')
}

$tip = Join-Path $here 'tip_exe.ps1'

# ---- 1. first run: no pointer -> one build, the pointer, TIP_EXE, the rig
$M = Join-Path $scratch 'main'; New-FakeMain $M
$r = Invoke-Tip $tip $M
$p = Get-Ptr $M
Check 'first run: one build, exit 0, TIP_EXE = the pointer exe' ($r.Code -eq 0 -and $r.Lane.Count -eq 1 -and $p -and $r.Out -cmatch "TIP_EXE $([regex]::Escape($p.exe))") "$($r.Code) lane=$($r.Lane.Count) $($r.Out)"
Check '  ... the folder is tip-<c9 of HEAD>, the pointer names HEAD' ($p -and (Split-Path $p.folder -Leaf) -eq "tip-$((Get-Head $M).Substring(0, 9))" -and $p.commit -eq (Get-Head $M))
Check '  ... the build was admitted and asked for 10 jobs (granted 10)' ($r.Lane.Count -eq 1 -and $r.Lane[0] -match 'jobs=10 lockwait=$')
Check '  ... the rig was built (none before)' ($r.Rig -eq 1)
$first = $p.exe

# ---- 2. docs-only and tools-only commits: no build, no rig; the same exe
Commit-In $M 'docs\n.md' 'docs'
$r = Invoke-Tip $tip $M
Check 'docs-only commit: no build, no rig, the same exe' ($r.Code -eq 0 -and $r.Lane.Count -eq 0 -and $r.Rig -eq 0 -and $r.Out -cmatch "TIP_EXE $([regex]::Escape($first))") "$($r.Out)"
Commit-In $M 'tools\x.py' 'tools'
$r = Invoke-Tip $tip $M
Check 'tools-only commit: no build, no rig' ($r.Code -eq 0 -and $r.Lane.Count -eq 0 -and $r.Rig -eq 0) "$($r.Out)"
$pl = New-Plant 'commit_judge' 'tools\exe\tip_exe_lib.ps1' '--exe $Exe --repo $Main 2>&1' '--exe $Exe --repo $Main --want (Get-TipHead $Main) 2>&1'
$r = Invoke-Tip $pl $M
Check 'PLANTED TWIN (judge by commit identity): a tools-only commit builds again (the row above must FAIL on it)' ($r.Lane.Count -eq 1) "$($r.Out)"

# ---- 3. tools\scene commit: rig rebuilt, no exe build; src commit: a new folder, the old one kept
Commit-In $M 'tools\scene\frame_rig.cpp' 'rig harness'
$r = Invoke-Tip $tip $M
Check 'tools\scene commit: the rig is rebuilt, the exe is not' ($r.Code -eq 0 -and $r.Lane.Count -eq 0 -and $r.Rig -eq 1) "$($r.Out)"
Commit-In $M 'docs\n.md' 'docs again'
$pl = New-Plant 'rig_all_paths' 'tools\exe\tip_exe_lib.ps1' 'diff --name-only $c HEAD -- src tools/scene' 'diff --name-only $c HEAD'
$r = Invoke-Tip $pl $M
Check 'PLANTED TWIN (rig judged by every path): a docs commit rebuilds the rig (the docs rows must FAIL on it)' ($r.Rig -eq 1) "$($r.Out)"
Commit-In $M 'src\a.cpp' 'src fix'
$r = Invoke-Tip $tip $M
$p = Get-Ptr $M
Check 'src commit: a new folder, the pointer moved, the old folder kept, the rig rebuilt' ($r.Code -eq 0 -and $r.Lane.Count -eq 1 -and $p.exe -ne $first -and (Test-Path $first) -and $p.commit -eq (Get-Head $M) -and $r.Rig -eq 1) "$($r.Out)"
$second = $p.exe

# ---- 4. not admitted, lock not free, build failed (foreground only: the runner is told, nothing waits)
Commit-In $M 'src\a.cpp' 'src 3'
$r = Invoke-Tip $tip $M @() @{ STUB_MODE = 'refused' }
Check 'foreground, not admitted: exit 75 (the caller is told), nothing built, pointer unchanged, no TIP_EXE' ($r.Code -eq 75 -and $r.Lane.Count -eq 0 -and $r.Out -cnotmatch 'TIP_EXE ' -and (Get-Ptr $M).exe -eq $second) "$($r.Code) $($r.Out)"
Check '  ... tip.log says it' ([IO.File]::ReadAllText((Join-Path $M 'out\exe\tip.log')) -match 'REFUSED: not admitted')
$r = Invoke-Tip $tip $M @() @{ STUB_LANE_EXIT = '4' }
Check 'foreground, lock not free: exit 4, lane_exe waits its own default' ($r.Code -eq 4 -and $r.Lane[0] -match 'lockwait=$') "$($r.Code) $($r.Lane)"
$r = Invoke-Tip $tip $M @() @{ STUB_LANE_EXIT = '5' }
Check 'foreground, build failed: exit 5' ($r.Code -eq 5) "$($r.Code)"

# WARM review R2: a caller that is a git hook passes git's hook environment (GIT_INDEX_FILE=.git/index relative, GIT_AUTHOR_DATE frozen; measured on
# git 2.52); none of it may reach the build (lane_exe's git runs in a slot worktree).
Commit-In $M 'src\a.cpp' 'src 4b'
$ewt = Join-Path $scratch 'envwt'
& git -C $M worktree add -q --detach $ewt 2>&1 | Out-Null
$envLog = Join-Path $scratch 'lane_env.log'
$r = Invoke-Tip $tip $M @() @{ GIT_INDEX_FILE = '.git/index'; GIT_AUTHOR_DATE = '@1 +0000'; STUB_LANE_ENV = $envLog; STUB_LANE_WT = $ewt }
foreach ($k in 'GIT_INDEX_FILE', 'GIT_AUTHOR_DATE', 'STUB_LANE_ENV', 'STUB_LANE_WT') { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$el = "$(Get-Content -LiteralPath $envLog -ErrorAction SilentlyContinue)"
Check 'hook environment: the build sees no GIT_INDEX_FILE / GIT_AUTHOR_DATE, and git in a slot worktree works' ($r.Lane.Count -eq 1 -and $el -ceq 'gif=[] date=[] slot_checkout=0') "$el | $($r.Out)"

# ---- 6. HEAD moves during a build: one more pass in the same run
Commit-In $M 'src\a.cpp' 'src 5'
$r = Invoke-Tip $tip $M @() @{ STUB_LANE_COMMIT = (Join-Path $scratch 'moved.flag') }
Check 'HEAD moved by a src commit during the build: a second build, the pointer names the new HEAD' ($r.Code -eq 0 -and $r.Lane.Count -eq 2 -and (Get-Ptr $M).commit -eq (Get-Head $M)) "$($r.Lane.Count) $($r.Out)"
Commit-In $M 'src\a.cpp' 'src 5b'
$r = Invoke-Tip $tip $M @() @{ STUB_RIG_COMMIT = (Join-Path $scratch 'rigmoved.flag') }
Remove-Item env:STUB_RIG_COMMIT -ErrorAction SilentlyContinue
Check 'WARM review R4: a src commit during the rig build: that rig is not kept as current (built again on the next pass, stamp = the new HEAD)' ($r.Code -eq 0 -and $r.Rig -eq 2 -and $r.Lane.Count -eq 2 -and "$(Get-Content (Join-Path $M 'out\tools\frame_rig.stamp'))" -match "^git $((Get-Head $M).Substring(0, 12)) ") "rig=$($r.Rig) lane=$($r.Lane.Count) $($r.Out)"
Commit-In $M 'src\a.cpp' 'src 5c'
$pl = New-Plant 'rig_mixed_kept' 'tools\exe\tip_exe.ps1' '((Get-TipHead $Main) -ne $Target -or -not (Test-TipSrcClean $Main))' '$false'
$r = Invoke-Tip $pl $M @() @{ STUB_RIG_COMMIT = (Join-Path $scratch 'rigmoved2.flag') }
Remove-Item env:STUB_RIG_COMMIT -ErrorAction SilentlyContinue
Check 'PLANTED TWIN (the rig judged by its end-of-build stamp only): the mixed rig is kept, one rig build (the R4 row must FAIL on it)' ($r.Rig -eq 1) "rig=$($r.Rig) $($r.Out)"

# ---- 7. the rig is left when src has uncommitted edits
Commit-In $M 'src\a.cpp' 'src 6'
Set-Content -LiteralPath (Join-Path $M 'src\new.h') -Value 'untracked'
$r = Invoke-Tip $tip $M
Check 'uncommitted file under src: the exe is built, the rig is left and it says so' ($r.Code -eq 0 -and $r.Lane.Count -eq 1 -and $r.Rig -eq 0 -and $r.Out -match 'rig left') "$($r.Out)"
Remove-Item -LiteralPath (Join-Path $M 'src\new.h')
$r = Invoke-Tip $tip $M @() @{ STUB_RIG_EXIT = '1' }
Check 'rig refused by the queue: the exe answer stands (TIP_EXE, exit 0), the rig REFUSED, said' ($r.Code -eq 0 -and $r.Out -cmatch 'TIP_EXE ' -and $r.Out -match 'rig REFUSED') "$($r.Code) $($r.Out)"

# ---- 8. the stream-keeping exe: its own pointer and folders, no rig
$r = Invoke-Tip $tip $M @('-Preset', 'win-amd64-release')
$ps = Get-Ptr $M 'tip-stream.json'
Check 'stream preset: tip-stream.json, folder tipstream-<c9>, preset passed, no rig' ($r.Code -eq 0 -and $ps -and (Split-Path $ps.folder -Leaf) -like 'tipstream-*' -and $r.Lane[0] -match 'win-amd64-release jobs' -and $r.Rig -eq 0) "$($r.Out)"
$r = Invoke-Tip $tip $M @('-Preset', 'win-amd64-coverage')
Check 'another preset: exit 2' ($r.Code -eq 2)

# ---- 9. the pointer is written beside and renamed: a failed rename keeps the old file and leaves no .tmp
$pp = Join-Path $scratch 'ptr.json'
Write-TipJson $pp @{ exe = 'A' }
$lk = [IO.File]::Open($pp, 'Open', 'Read', 'None')
$threw = $false
try { Write-TipJson $pp @{ exe = 'B' } } catch { $threw = $true } finally { $lk.Dispose() }
Check 'pointer write on a held file: throws, the old pointer stays, no .tmp left' ($threw -and (Read-TipPointer $pp).exe -eq 'A' -and -not (Get-ChildItem $scratch -Filter 'ptr.json.*.tmp'))

# ---- 11. not the main checkout
$wt = Join-Path $scratch 'wt'
& git -C $M worktree add -q --detach $wt 2>&1 | Out-Null
$r = Invoke-Tip $tip $wt
Check 'a worktree (.git is a file): REFUSED, exit 2' ($r.Code -eq 2 -and $r.Out -match 'not the main checkout') "$($r.Out)"
# (the fake worktree goes with the scratch folder at the next start: it is registered only in the fake repo's own .git)


""
if ($fails) { "tip_exe_selftest: $fails FAIL"; exit 1 }
'tip_exe_selftest: all PASS'
exit 0
