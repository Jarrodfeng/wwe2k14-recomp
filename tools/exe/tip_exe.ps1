# S109 WARM2: the exe of the session tip, for run_list.ps1 -Exe tip | tip-stream (docs\research\s109\WARM_plan.md, docs\systems\worktree_game_build.md "The tip exe").
#   pwsh -NoProfile -File C:\Users\Jarrod\wwe2k14-recomp\tools\exe\tip_exe.ps1 [-Preset win-amd64-release]     (foreground only: what run_list.ps1 -Exe tip calls)
# The tip = the main checkout's HEAD. The pointer out\exe\tip.json (stream-keeping: tip-stream.json) names the exe of the newest tip state; it is CURRENT when
# tools\bench\exe_vs_head.py in HEAD mode finds no code path changed since the exe's stamp (docs / tools commits never stale it, never build). Not current:
# tools\exe\lane_exe.ps1 -Name tip-<c9> (stream: tipstream-<c9>) -Commit HEAD builds it in slot 1, then the pointer moves (written beside, renamed). Old tip
# folders stay (each holds its own runs' logs; the previous one is the "before" exe). Then, for the check exe only, the frame rig (out\tools\frame_rig.exe,
# tools\scene\build_rig.ps1 in the main checkout) is rebuilt when src / tools\scene differ from its stamp's commit, unless the main checkout has uncommitted
# edits there (said, not built). HEAD moving meanwhile = one more pass. No runner handle: two callers of the same tip-<c9> meet in lane_exe's BUILD hold, and
# the second finds the folder built (lane_exe.ps1 "B2" re-judge after the hold; the folder is published by rename).
# It waits for the admission queue's own bound (180 s) and lane_exe's lock wait; the last line is TIP_EXE <path> on success.
# Every event: a line in out\exe\tip.log. Exit: 0 ok, 2 bad arguments, 4 BUILD lock not free, 5 build failed, 6 refused, 75 not admitted.
# Test hooks: -Main (a fake checkout), -LaneExe / -RigBuild (stubs), -NoRig.
param([string]$Preset = 'win-amd64-release-off', [int]$Jobs = 10,
      [string]$Main = 'C:\Users\Jarrod\wwe2k14-recomp', [string]$LaneExe = '', [string]$RigBuild = '', [switch]$NoRig)
$ErrorActionPreference = 'Continue'
# WARM review R2: a caller that is a git hook holds git's hook environment (GIT_INDEX_FILE=.git/index, a RELATIVE path); lane_exe's git in its slot worktree
# failed with it. Dropping the inherited GIT_* is harmless for any caller and stays.
Get-ChildItem env: | Where-Object { $_.Name -like 'GIT_*' -and $_.Name -ne 'GIT_EXEC_PATH' } | ForEach-Object { Remove-Item -LiteralPath "env:$($_.Name)" }
. "$PSScriptRoot\tip_exe_lib.ps1"
$Main = [IO.Path]::GetFullPath($Main).TrimEnd('\')
if (-not $LaneExe) { $LaneExe = Join-Path $PSScriptRoot 'lane_exe.ps1' }
if (-not $RigBuild) { $RigBuild = Join-Path $Main 'tools\scene\build_rig.ps1' }
$judge = Join-Path $PSScriptRoot '..\bench\exe_vs_head.py'
$names = Get-TipNames $Preset
if (-not $names) { 'tip_exe: REFUSED: -Preset is win-amd64-release-off (the check exe) or win-amd64-release (the stream-keeping exe)'; exit 2 }
if (-not (Test-Path -LiteralPath (Join-Path $Main '.git') -PathType Container)) { "tip_exe: REFUSED: $Main is not the main checkout (its .git is not a folder)"; exit 2 }
$exeRoot = Join-Path $Main 'out\exe'
[void][IO.Directory]::CreateDirectory($exeRoot)
function Say([string]$m) { Write-Host "tip_exe: $m"; Write-TipLog $exeRoot "$($names.Prefix): $m" }

# The exe of HEAD: current or built. -> @{ Ok; Exe; Code; Why }
function Update-TipExe([string]$Target) {
    $ptrPath = Join-Path $exeRoot $names.Pointer
    $ptr = Read-TipPointer $ptrPath
    if ($ptr -and (Test-Path -LiteralPath "$($ptr.exe)")) {
        $j = Test-TipExeCurrent $Main "$($ptr.exe)" $judge
        if ($j.Code -eq 0) { return @{ Ok = $true; Exe = "$($ptr.exe)"; Code = 0; Why = "current ($($j.Text))" } }
        $why = "behind: $($j.Text)"
    } else { $why = if ($ptr) { "the pointer's exe $($ptr.exe) is gone" } else { "no $($names.Pointer)" } }
    $c9 = $Target.Substring(0, 9)
    $name = "$($names.Prefix)-$c9"
    Say "building $name ($why)"
    . (Join-Path $PSScriptRoot '..\admit\Admit.ps1')
    try { $n = Request-BuildParallel -Want $Jobs -Kind cc.game_src -TimeoutS 180 }
    catch { return @{ Ok = $false; Code = 75; Why = "not admitted: $($_.Exception.Message)" } }
    try {
        $a = @('-NoProfile', '-File', $LaneExe, '-Name', $name, '-Commit', $Target, '-Preset', $Preset, '-Jobs', "$n", '-Main', $Main)
        $lines = @(& pwsh @a 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE
    } finally { Release-BuildParallel }
    try { [IO.File]::AppendAllLines((Join-Path $exeRoot 'tip_build.log'), [string[]](@("== $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) $name exit $code") + $lines)) } catch { }
    $last = $lines | Where-Object { $_ -match '^LANE_EXE (.+)$' } | Select-Object -Last 1
    if ($code -eq 0 -and $last -and ($last -match '^LANE_EXE (.+)$') -and (Test-Path -LiteralPath $Matches[1].Trim())) {
        $exe = $Matches[1].Trim()
        Write-TipJson $ptrPath ([ordered]@{ commit = $Target; c9 = $c9; folder = (Split-Path $exe -Parent); exe = $exe; preset = $Preset; built_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') })
        return @{ Ok = $true; Exe = $exe; Code = 0; Why = "built $name" }
    }
    $tail = ($lines | Select-Object -Last 2) -join ' | '
    if ($code -eq 4) { return @{ Ok = $false; Code = 4; Why = "the BUILD lock was not free: $tail" } }
    return @{ Ok = $false; Code = $(if ($code -eq 6 -or $code -eq 2) { 6 } else { 5 }); Why = "lane_exe exit $code (full output: out\exe\tip_build.log): $tail" }
}

# The rig follows the check exe. Its failure never fails the exe.
function Update-TipRig([string]$Target) {
    if (-not (Test-TipSrcClean $Main)) {
        Say 'rig left as it is: the main checkout has uncommitted edits under src / tools\scene (a rig built now would draw code no commit holds)'
        return
    }
    $need = Get-TipRigNeed $Main
    if (-not $need.Need) { return }
    Say "building the rig ($($need.Why))"
    $lines = @(& pwsh -NoProfile -File $RigBuild 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE
    try { [IO.File]::AppendAllLines((Join-Path $exeRoot 'tip_build.log'), [string[]](@("== $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) rig exit $code") + $lines)) } catch { }
    # WARM review R4: build_rig compiles the main checkout's files and stamps the HEAD it finds at its END; a merge during the build gives objects of two commits
    # stamped with the later one, which the next pass would call current. Such a rig is stamped unknown, so the next pass (HEAD moved) builds it again.
    if ($code -eq 0 -and ((Get-TipHead $Main) -ne $Target -or -not (Test-TipSrcClean $Main))) {
        try { [IO.File]::WriteAllText((Join-Path $Main 'out\tools\frame_rig.stamp'), "git unknown dirty 1 (HEAD or src moved during the build from $Target)`n") } catch { }
        Say 'rig built while HEAD or src moved: stamped unknown (rebuilt on the next pass)'
        return
    }
    if ($code -eq 0) { Say 'rig built'; return }
    $refused = $lines | Where-Object { $_ -match 'REFUSED' } | Select-Object -First 1
    Say "rig $(if ($refused) { 'REFUSED' } else { 'FAILED' }) (exit $code): $(if ($refused) { $refused } else { ($lines | Select-Object -Last 1) })"
}

$result = $null
for ($pass = 0; $pass -lt 5; $pass++) {
    $target = Get-TipHead $Main
    if (-not $target) { Say "REFUSED: git rev-parse HEAD failed in $Main"; exit 6 }
    $result = Update-TipExe $target
    Say "$(if ($result.Ok) { "exe $($result.Why): $($result.Exe)" } else { "REFUSED: $($result.Why)" })"
    if (-not $result.Ok) { break }
    if ($names.Rig -and -not $NoRig) { Update-TipRig $target }
    if ((Get-TipHead $Main) -eq $target) { break }
}
if ($result -and $result.Ok) { "TIP_EXE $($result.Exe)"; exit 0 }
if ($result) { exit $result.Code }
exit 4
