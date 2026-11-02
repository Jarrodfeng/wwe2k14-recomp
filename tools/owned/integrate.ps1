# One-command integration of renderer builders' branches (Session 82, harness builder).
#   & .\tools\owned\integrate.ps1 -Branches s82-p0,s82-p2 [-Worktree thirdparty\sdk-wt\s82-int]
#   & .\tools\owned\integrate.ps1 -Branches s82-p0,s82-p2 -DryRun          predicts conflicts + pre-flights, changes nothing
#   & .\tools\owned\integrate.ps1 -NoMerge                                 after resolving a conflict by hand: build + check
# Steps (stops at the first failing one and prints where):
#   1. merge each branch into the integration worktree (default thirdparty\sdk-wt\s82-int, branch s82-owned):
#      git merge --no-ff -X diff-algorithm=patience. A CONFLICT STOPS HERE: the conflicted files are printed, the merge is
#      left in progress for you to resolve (never auto-resolved; "git -C <wt> merge --abort" undoes it). A branch already
#      contained in the worktree's branch is skipped. The worktree must have no uncommitted tracked changes.
#   2. fast_build.ps1 -Name int (incremental, build class of the run lock) -> out\harness\bisect\exe\live_int\.
#   3. preflight.ps1: refuses before any game starts when the exe folder / replay / scenario / game-data flags' targets are
#      missing, a game runs from the folder, or RAM is short.
#   4. owned_check.ps1 -Exe <exe> -OnFlags '--gpu_native_owned_frame=true' -Validation: 720p + 1440p, on vs off, the quick match
#      route with the census (4 runs; the ON arms with Vulkan validation) -> the verdicts, the scoreboard, truth work per frame.
#   5. the quick entrance replay on vs off in one batch (route short\s82_ent_shots.txt, --wwe2k14_replay_play=s82_lb_quick_ent,
#      720p NATIVE settings, run_list -Parallel 2) -> its verdict.
#   6. SUMMARY: verdicts, scoreboard, truth work per frame, build times, game runs used. Full log: out\owned_check\integrate_<time>.log.
# Exit codes: 0 ok (a WARN verdict is printed in the summary and stays 0), 2-6 stopped before any game (bad worktree, merge
# problem, conflict 4, build failed 5, pre-flight failed 6), 7 owned_check or the entrance replay FAILED or gave no verdict.
# Options: -NoValidation (owned_check without Vulkan validation), -NoEntrance, -NoGames (merge + build + pre-flight only),
# -Name (fast_build name, default int), -OnFlags (default '--gpu_native_owned_frame=true').
# Call it in-process from PowerShell or with pwsh -File (-Branches takes a comma/space separated list as one string).
param([string]$Branches = '', [string]$Worktree = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\s82-int', [string]$Name = 'int',
      [string]$OnFlags = '--gpu_native_owned_frame=true', [switch]$NoMerge, [switch]$DryRun, [switch]$NoValidation,
      [switch]$NoEntrance, [switch]$NoGames)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$SdkSrc = "$R\thirdparty\rexglue-sdk-src"
Set-Location $R
if (-not [System.IO.Path]::IsPathRooted($Worktree)) { $Worktree = Join-Path $R $Worktree }
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
$list = @($Branches -split '[,\s]+' | Where-Object { $_ })
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$logFile = "$R\out\owned_check\integrate_$stamp.log"
$null = New-Item -ItemType Directory -Force -Path "$R\out\owned_check"
function Say([string]$m) { Write-Host $m; Add-Content -LiteralPath $logFile -Value $m }
function Stop-Here([string]$why, [int]$code) { Say "INTEGRATE STOPPED: $why"; Say "log: $logFile"; exit $code }
$t0 = Get-Date
$runsUsed = 0
$summary = [System.Collections.Generic.List[string]]::new()
Say "integrate ${stamp}: worktree $Worktree, branches [$($list -join ', ')]$(if ($DryRun) { ', DRY RUN' })"

# --- the worktree
if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { Stop-Here "$Worktree is not a git worktree" 2 }
$wtBranch = (git -C $Worktree rev-parse --abbrev-ref HEAD).Trim()
$mergeHead = (git -C $Worktree rev-parse --git-path MERGE_HEAD).Trim()
if (-not [System.IO.Path]::IsPathRooted($mergeHead)) { $mergeHead = Join-Path $Worktree $mergeHead }
if ((Test-Path -LiteralPath $mergeHead) -and -not $NoMerge) { Stop-Here "a merge is already in progress in ${Worktree}: resolve and commit it (then rerun with -NoMerge), or git -C $Worktree merge --abort" 3 }
$dirty = @(git -C $Worktree status --porcelain --untracked-files=no)
if ($dirty.Count -and -not $NoMerge -and -not $DryRun) { Stop-Here "$Worktree has uncommitted tracked changes ($($dirty.Count)): commit or stash them first" 3 }
Say "integration branch: $wtBranch at $((git -C $Worktree rev-parse --short HEAD).Trim())"

# --- 1. merge (or predict)
if ($NoMerge) { Say 'merge: skipped (-NoMerge)' }
else {
    foreach ($b in $list) {
        git -C $SdkSrc rev-parse --verify --quiet "refs/heads/$b" *> $null
        if ($LASTEXITCODE) { Stop-Here "no branch '$b' in the SDK repo" 2 }
    }
    if ($DryRun) {
        foreach ($b in $list) {
            git -C $Worktree merge-base --is-ancestor $b HEAD 2>$null
            if ($LASTEXITCODE -eq 0) { Say "dry run: $b is already in $wtBranch"; continue }
            $o = @(git -C $Worktree merge-tree --write-tree --name-only HEAD $b 2>&1)
            if ($LASTEXITCODE -eq 0) { Say "dry run: $b merges into $wtBranch cleanly" }
            else { Say "dry run: $b vs $wtBranch CONFLICTS:"; $o | Select-Object -Skip 1 | Where-Object { $_ -and $_ -notmatch '^(Auto-merging|CONFLICT)' } | ForEach-Object { Say "    $_" } }
        }
        for ($i = 0; $i -lt $list.Count; $i++) { for ($j = $i + 1; $j -lt $list.Count; $j++) {
            $o = @(git -C $Worktree merge-tree --write-tree --name-only $list[$i] $list[$j] 2>&1)
            if ($LASTEXITCODE -eq 0) { Say "dry run: $($list[$i]) and $($list[$j]) merge cleanly" }
            else { Say "dry run: $($list[$i]) vs $($list[$j]) CONFLICTS:"; $o | Select-Object -Skip 1 | Where-Object { $_ -and $_ -notmatch '^(Auto-merging|CONFLICT)' } | ForEach-Object { Say "    $_" } }
        } }
    } else {
        foreach ($b in $list) {
            git -C $Worktree merge-base --is-ancestor $b HEAD 2>$null
            if ($LASTEXITCODE -eq 0) { Say "merge: $b is already in $wtBranch (skipped)"; $summary.Add("merged: $b already in"); continue }
            $msg = "Integrate $b into $wtBranch`n`nCo-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
            $o = @(git -C $Worktree merge --no-ff -X diff-algorithm=patience -m $msg $b 2>&1)
            if ($LASTEXITCODE) {
                $conf = @(git -C $Worktree diff --name-only --diff-filter=U)
                Say "merge of $b CONFLICTED. Conflicted files ($($conf.Count)):"
                $conf | ForEach-Object { Say "    $_" }
                Say "The merge is left in progress in $Worktree. Resolve (never auto-resolved), 'git add' and commit, then: integrate.ps1 -NoMerge. Or: git -C $Worktree merge --abort"
                Stop-Here "conflict merging $b" 4
            }
            Say "merge: $b -> $wtBranch ok ($((git -C $Worktree rev-parse --short HEAD).Trim()))"
            $summary.Add("merged: $b")
        }
    }
}
if ($DryRun) {
    $exeGuess = "$R\out\harness\bisect\exe\live_$Name\wwe2k14.exe"
    if (Test-Path -LiteralPath $exeGuess) { & "$R\tools\owned\preflight.ps1" -Exe $exeGuess -Entrance 2>&1 | ForEach-Object { Say "$_" } }
    else { Say "dry run: $exeGuess does not exist yet (the first fast_build creates it); pre-flight skipped" }
    Say 'dry run: nothing merged, built or launched'
    exit 0
}

# --- 2. fast build
$tb = Get-Date
$bo = @(& pwsh -NoProfile -File "$R\tools\owned\fast_build.ps1" -Worktree $Worktree -Name $Name 2>&1)
$bo | ForEach-Object { Add-Content -LiteralPath $logFile -Value "$_" }
$bo | Where-Object { "$_" -match '^(fast_build |build |configure|\[runlock\] acquired|.*FAILED|.*failed)' } | ForEach-Object { Write-Host "$_" }
$exeLine = @($bo | Where-Object { "$_" -match '^FAST_BUILD_EXE ' }) | Select-Object -Last 1
if (-not $exeLine) { Stop-Here 'the fast build failed (see above / the log)' 5 }
$exe = ("$exeLine" -replace '^FAST_BUILD_EXE ', '').Trim()
$buildLine = @($bo | Where-Object { "$_" -match '^fast_build ' }) | Select-Object -Last 1
Say "build: $buildLine"
$summary.Add("build: $buildLine")

# --- 3. pre-flight
$pf = @(& "$R\tools\owned\preflight.ps1" -Exe $exe -Entrance 2>&1)
$pf | ForEach-Object { Say "$_" }
if ($LASTEXITCODE -ne 0) { Stop-Here 'pre-flight failed: no game was launched' 6 }
if ($NoGames) { Say 'no games (-NoGames): stopping after build + pre-flight'; exit 0 }

# --- 4. owned_check (+ validation on the ON arms)
Say ''
Say '== owned_check (720p + 1440p, on vs off, quick match route with the census)'
$ocArgs = @{ Exe = $exe; OnFlags = $OnFlags }
if (-not $NoValidation) { $ocArgs.Validation = $true }
$oc = @(& "$R\tools\native\owned_check.ps1" @ocArgs 2>&1)
$ocCode = $LASTEXITCODE
$oc | ForEach-Object { Say "$_" }
$runsUsed += @($oc | Where-Object { "$_" -match ' -> wwe2k14_\d+' }).Count
$ocVerdict = @($oc | Where-Object { "$_" -match '^OWNED VERDICT' }) | Select-Object -Last 1
$summary.Add("owned_check: $(if ($ocVerdict) { $ocVerdict } else { "no verdict (exit $ocCode)" })")
# (Review fix: exit 7 when a verdict is FAIL or missing; WARN stays exit 0 but is printed in the summary.)
$failed = @()
if (-not $ocVerdict -or "$ocVerdict" -match 'FAIL') { $failed += 'owned_check' }
foreach ($l in $oc) { if ("$l" -match '^== run |truth work per frame|owned \d+ of \d+ passes|^  -> |validation:') { $summary.Add("  $l") } }

# --- 5. the quick entrance replay, on vs off in one batch
if (-not $NoEntrance) {
    Say ''
    Say '== quick entrance replay (s82_lb_quick_ent, 720p NATIVE settings), on vs off in one batch'
    $set = "$R\out\owned_check\settings_native720.json"
    if (-not (Test-Path $set)) { Set-Content -NoNewline -Path $set -Value '{"renderer": 2, "frame_rate": 1, "resolution": 0}' }
    $ent = 'tools\input_scripts\short\s82_ent_shots.txt'
    $common = "--wwe2k14_replay_play=s82_lb_quick_ent --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false --game_data_root=$R\game_data\extracted --wwe2k14_replay_dir=$R\out\replays --wwe2k14_scenario_dir=$R\out\scenarios"
    $extras = @("--wwe2k14_settings_file=$set $OnFlags", "--wwe2k14_settings_file=$set --gpu_native_own_steps=0")
    $eo = @(& "$R\tools\bench\run_list.ps1" -Scripts @($ent, $ent) -ScriptExtras $extras -Parallel 2 -Extra $common -Exe $exe 2>&1)
    $eo | Where-Object { "$_" -match ' -> wwe2k14_\d+' } | ForEach-Object { Say "$_" }
    $eruns = @($eo | ForEach-Object { if ("$_" -match ' -> wwe2k14_(\d+)') { $Matches[1] } })
    $runsUsed += $eruns.Count
    if ($eruns.Count -ne 2) { Say "entrance replay: expected 2 runs, got $($eruns.Count)"; $summary.Add('entrance replay: NO VERDICT (runs missing)'); $failed += 'entrance replay (no verdict)' }
    else {
        $ev = @(& py -I "$R\tools\native\owned_verdict.py" --dir (Split-Path -Parent $exe) @eruns 2>&1)
        $ev | ForEach-Object { Say "$_" }
        $entVerdict = @($ev | Where-Object { "$_" -match '^OWNED VERDICT' }) | Select-Object -Last 1
        $summary.Add("entrance replay: $(if ($entVerdict) { $entVerdict } else { 'no verdict' })")
        if (-not $entVerdict -or "$entVerdict" -match 'FAIL') { $failed += 'entrance replay' }
        foreach ($l in $ev) { if ("$l" -match '^== run |^  shots vs|owned-frame census|^  -> |  sheet:') { $summary.Add("  $l") } }
    }
}

# --- 6. summary
Say ''
Say '================ INTEGRATE SUMMARY ================'
$summary | ForEach-Object { Say "$_" }
Say ("game runs used: {0} (owned_check {1}{2}); total {3:N0} s; exe {4}" -f $runsUsed, 4, $(if ($NoEntrance) { '' } else { ' + entrance replay 2' }), ((Get-Date) - $t0).TotalSeconds, $exe)
Say "full log: $logFile"
$warned = @($summary | Where-Object { "$_" -match 'VERDICT WARN' })
if ($warned.Count) { Say "WARN: $($warned -join '; ') (exit code stays 0)" }
if ($failed.Count) { Say "INTEGRATE FAILED: $($failed -join ', ') FAIL or without a verdict (exit 7)"; exit 7 }
exit 0
