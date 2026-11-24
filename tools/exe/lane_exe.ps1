# S106 C4: an exe per commit, built from a commit into its own folder under the main checkout's out\exe\ (docs\research\s106\C4.md, docs\systems\worktree_game_build.md).
#   pwsh -NoProfile -File C:\Users\Jarrod\wwe2k14-recomp\tools\exe\lane_exe.ps1 -Lane <name> -Commit <sha> [-Preset win-amd64-release-off] [-Jobs 10] [-Pdb]
#   pwsh -NoProfile -File ...\lane_exe.ps1 -Name <folder> -Commit <sha>        (-Name <c9 of the commit>; -Lane is then optional)
#   (S109 WARM: -Warm is gone. The tip's exe is kept by tools\exe\tip_exe.ps1, which calls -Name tip-<c9> and keeps slot 1 warm as a by-product.)
#   ... -DryRun                                                                   (print the plan, take no lock, write nothing, build nothing)
# What it does: holds a BUILD-class run lock (max 3 at once, beside untimed shared game runs, never beside an exclusive hold), claims a free slot
# <main>\out\wt\exe-slot1..3 (a persistent, detached, LINK-FREE git worktree; <slot>.owner = pid=N while it builds), checks the commit out, gives it by
# COPY what a worktree lacks (generated\default, the xex, stb / xxHash, the streamline headers, the FidelityFX dll), configures once, calls
#   $env:REXSDK=<main>\thirdparty\rexglue-sdk-vulkan; <main>\tools\build.ps1 -Root <slot> -Preset <Preset> -Run <Name> -LockMode Build -Jobs <Jobs>
# and stages <main>\out\exe\<Name>\ (never replaced in place; a half-written folder is never visible: it is built beside and renamed).
# It never builds in a builder's worktree or in out\build, never makes a junction / symlink / hard link, never starts a game.
# The folder holds wwe2k14.exe, .map, wwe2k14_probes.dll, stamp\wwe2k14_build_stamp.h (exe_vs_head reads it beside the exe), the pinned rex*.dll,
# amd_fidelityfx_vk.dll, shader_list.txt (when out\build\<Preset> has one), exe_info.json, and .pdb files only with -Pdb.
# S106 C4F: the options recipe is pinned: -DWWE2K14_EXE_SYMBOLS=OFF -DWWE2K14_EXE_SYMBOLS_GUEST_LINES=OFF always (a check exe); the other BOOL options come from main's cache,
# are compared with the SLOT's own CMakeCache (a slot that differs is re-configured inside the hold) and the slot's values are recorded in exe_info.json.
# Commits older than 704968263 (2026-11-03) cannot re-enter the BUILD hold (their build.ps1 has no -LockMode), so they cannot be built here.
# Running it: run_list.ps1 -Exe tip (the tip's exe: no other flag), or -Exe <folder>\wwe2k14.exe -ForCommit <commit> (never -AllowStaleExe for an exe folder). No game flag
#   is needed (S109 WARM, docs\research\s109\WARM_plan.md (c)): the exe finds game_data\extracted by walking up from its folder, the scenario dir ../../scenarios is
#   out\scenarios from the exe folder run_and_triage starts it in, run_list adds the replay dir, --audio_mute and --vsync. The settings file too (S109 WARM2, review
#   R1): run_list gives every run its own --wwe2k14_settings_file under out\bench (Add-RunListSettingsFlag) unless the caller or the route's header names one, so no
#   run reads or writes the player's %APPDATA% file (a stream-keeping exe reads it at every start, the GRAPHICS menu reads and writes it in any build).
# Last line on success: LANE_EXE <folder>\wwe2k14.exe. Exit codes: 0 ok, 2 bad arguments, 4 lock timeout, 5 build failed, 6 refused (nothing built:
# missing input, link in the slot, lane folder holds another exe, short generated copy, ...), 7 no free slot, 8 staging failed.
param([string]$Lane = '', [string]$Name = '', [string]$Commit = '', [string]$Preset = 'win-amd64-release-off', [int]$Jobs = 10,
      [int]$Slot = 0, [switch]$Pdb, [switch]$DryRun, [string]$Main = 'C:\Users\Jarrod\wwe2k14-recomp',
      [string]$ShaderList = '', [double]$LockWaitMinutes = 30, [double]$LockMaxMinutes = 90, [string]$SessionBranch = '',
      [double]$BuildTimeoutMinutes = 30, [string]$BuildScript = '')
$ErrorActionPreference = 'Continue'
$Main = [System.IO.Path]::GetFullPath($Main).TrimEnd('\')
if (-not $BuildScript) { $BuildScript = "$Main\tools\build.ps1" }   # a test hook: the build script to ask about / run
. "$PSScriptRoot\lane_exe_lib.ps1"
function Say([string]$m) { "lane_exe: $m" }
function Refuse([string]$m, [int]$code = 6) { "lane_exe: REFUSED, nothing built: $m"; exit $code }
# S106 C4F F5: the session branch (the merge-base shown) is the main checkout's current branch unless given.
if (-not $SessionBranch) { $SessionBranch = "$(& git -C $Main rev-parse --abbrev-ref HEAD 2>$null)".Trim(); if (-not $SessionBranch -or $SessionBranch -eq 'HEAD') { $SessionBranch = 'main' } }

if ($Name -and -not (Test-LaneName $Name)) { Refuse "-Name must start with a letter or digit, then letters, digits, . - or _ (got '$Name')" 2 }
if ($Lane -and $Lane -notmatch '^[A-Za-z0-9_-]{1,40}$') { Refuse "-Lane must be letters, digits, - or _ (got '$Lane')" 2 }
if (-not $Name -and -not $Lane) { Refuse 'give -Lane <name> or -Name <folder>' 2 }
if ($Preset -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$' -or $Preset -match '\.\.') { Refuse "-Preset is a folder name under out\build (letters, digits, . - _; got '$Preset')" 2 }
if ($Slot -lt 0 -or $Slot -gt 3) { Refuse '-Slot is 1..3' 2 }
if ($Jobs -lt 1 -or $Jobs -gt 64) { Refuse '-Jobs is 1..64' 2 }
if (-not $Commit) { Refuse '-Commit is required' 2 }
if (-not (Test-Path -LiteralPath "$Main\.git" -PathType Container)) { Refuse "-Main $Main is not the main checkout (its .git is not a folder)" 2 }

# 1. Inputs the main checkout must hold (the silent trap: no streamline headers = an exe without DLSS).
$miss = @(Test-LaneExeSources $Main)
if ($miss.Count) { foreach ($m in $miss) { "lane_exe: missing $m" }; Refuse "$($miss.Count) required input(s) missing in $Main" }

# S106 C4F2 B1: the main checkout's build.ps1 must take -LockMode (else it is dropped, the build asks for EXCLUSIVE inside our BUILD hold and the lock throws).
if (-not (Test-LaneBuildHasLockMode $BuildScript)) { Refuse "$BuildScript has no -LockMode parameter (main's tools\build.ps1 predates S106 C4: merge it into main first; a build would ask for an exclusive hold inside the BUILD hold)" }

# 2. The commit, the folder, the options.
$full = "$(& git -C $Main rev-parse --verify "$Commit^{commit}" 2>$null)".Trim()   # no Select-Object -First here: it can cut git's pipe and fake a nonzero exit
if (-not $full -or $LASTEXITCODE -ne 0) { Refuse "no such commit in ${Main}: $Commit" 2 }
$c9 = $full.Substring(0, 9)
& git -C $Main merge-base --is-ancestor 704968263 $full 2>$null
if ($LASTEXITCODE -ne 0) { Refuse "commit $c9 is older than 704968263 (its build.ps1 has no -LockMode; it cannot re-enter the BUILD hold)" }
if (-not $Name) { $Name = "lane-$Lane-$c9" }
$exeRoot = Join-Path $Main 'out\exe'
$folder = Join-Path $exeRoot $Name
$skipped = @()
$mainCache = Join-Path $Main "out\build\$Preset\CMakeCache.txt"
$mainOpts = Get-LaneCacheOptions $mainCache ([ref]$skipped)
$options = Get-LaneWantedOptions $mainOpts          # F4: symbols pinned OFF, guest lines OFF
$skipped = @($skipped | Where-Object { $_ -notmatch '^WWE2K14_EXE_SYMBOLS(_GUEST_LINES)?=' })
$mainStrings = Get-LaneCacheStrings $mainCache       # F3: non-empty STRING / FILEPATH options are not carried: refused below
# S108: only options the project DECLARES count. A cache entry no CMakeLists.txt names (e.g. WWE2K14_PY, left in main's cache by a
# -D of tools\scene\keep_stream_config_check.py) cannot change the build, so it is not a reason to refuse.
$declared = (Get-Content -LiteralPath (Join-Path $Main 'CMakeLists.txt') -Raw)
foreach ($k in @($mainStrings.Keys)) { if ($declared -notmatch "\b$([regex]::Escape($k))\b") { Say "main's cache string $k is not declared by CMakeLists.txt: ignored"; $mainStrings.Remove($k) } }
$state = Get-LaneFolderState $folder $full $Preset $options
$mbase = "$(& git -C $Main merge-base $full $SessionBranch 2>$null)".Trim()
$sdk = Join-Path $Main 'thirdparty\rexglue-sdk-vulkan'
if (-not (Test-Path -LiteralPath "$sdk\bin" -PathType Container)) { Refuse "installed SDK missing: $sdk\bin" }
$slots = @(1..3 | ForEach-Object { [pscustomobject]@{ N = $_; Dir = "$Main\out\wt\exe-slot$_"; Owner = "$Main\out\wt\exe-slot$_.owner" } })

if ($DryRun) {
    Say "DRY RUN (no lock, nothing written, nothing built)"
    Say "commit $full (merge-base with ${SessionBranch}: $(if ($mbase) { $mbase.Substring(0, 9) } else { 'none' }))"
    Say "lane '$Lane' name '$Name' preset $Preset jobs $Jobs$(if ($Pdb) { ' +pdb' })"
    Say "folder $folder -> $($state.State)$(if ($state.Why) { " ($($state.Why))" })"
    Say "options wanted (main's cache BOOLs, symbols pinned OFF): $(($options.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')$(if ($skipped.Count) { "   NOT copied (not ON/OFF): $($skipped -join ', ')" })"
    if ($mainStrings.Count) { Say "main's non-empty STRING / FILEPATH options: $(($mainStrings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ') (a real build refuses)" }
    foreach ($s in $slots) {
        $git = Test-Path -LiteralPath "$($s.Dir)\.git" -PathType Leaf
        $sc = "$($s.Dir)\out\build\$Preset\CMakeCache.txt"
        $cfg = Test-Path -LiteralPath $sc
        $dk = if ($cfg) { @(Get-LaneOptionDiff (Get-LaneCacheOptions $sc ([ref]@())) $options @(Get-LaneCacheUninitialized $sc)) } else { @() }
        Say "slot $($s.N): $(if (Test-LaneSlotBusy $s.Owner) { 'BUSY (owner held or build child alive)' } elseif ($git) { "free, worktree exists, $(if ($cfg) { "configured (warm); options source: the slot's own CMakeCache, $(if ($dk.Count) { "differs on $($dk -join ',') (re-configured in the hold)" } else { 'matches the wanted recipe' })" } else { 'not configured (cold)' })" } else { 'free, no worktree yet (cold: git worktree add)' })"
    }
    Say "options source: the slot's own CMakeCache.txt (compared with the wanted recipe above; recorded in exe_info.json)"
    foreach ($i in (Get-LaneExeInputs)) { Say "copy into the slot when missing: $($i.Source) ($($i.Kind), $(if ($i.Required) { 'required' } else { 'optional' }))" }
    Say "configure when the slot's preset cache is missing: cmake --preset $Preset $(($options.GetEnumerator() | ForEach-Object { "-D$($_.Key)=$($_.Value)" }) -join ' ')"
    Say "build: REXSDK=$sdk; $Main\tools\build.ps1 -Root <slot> -Preset $Preset -Run $Name -LockMode Build -Jobs $Jobs"
    Say "stage: $folder (built beside, renamed when complete) + exe_info.json + a row in $exeRoot\lane_exe.tsv"
    if ($state.State -eq 'Refuse') { Refuse $state.Why }
    Say 'plan ok'
    exit 0
}

if ($state.State -eq 'Reuse') { Say "reuse $folder ($($state.Why))"; "LANE_EXE $folder\wwe2k14.exe"; exit 0 }
if ($state.State -eq 'Refuse') { Refuse $state.Why }
if ($mainStrings.Count) { Refuse "main's cache holds non-empty STRING / FILEPATH option(s) a lane exe does not carry: $(($mainStrings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')" }

# 3. The BUILD hold, then everything that touches a slot.
. "$Main\tools\runlock.ps1"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$ph = [ordered]@{ lock = 0; checkout = 0; copy = 0; configure = 0; build = 0; stage = 0 }
$t0 = Get-Date
try { $lock = Enter-RunLock -Name "lane_exe $Name" -Mode Build -TimeoutMinutes $LockWaitMinutes -QueueAware -MaxTotalMinutes $LockMaxMinutes }
catch { Refuse "the BUILD lock was not free within $LockWaitMinutes min ($($_.Exception.Message))" 4 }
$ph.lock = [int]$sw.Elapsed.TotalSeconds; $sw.Restart()
$mySlot = $null; $slotFs = $null; $bp = $null; $result = 'fail'; $temp = ''; $coldWarm = 'warm'; $rc = 0   # $rc: the exit code a refusal inside the try sets before it throws 'lane_exe stop'
try {
    # B2: the folder state was judged before the lock wait (up to 30 min); judge it again now that the hold is ours. A builder that finished meanwhile = Reuse.
    $state = Get-LaneFolderState $folder $full $Preset $options
    if ($state.State -eq 'Reuse') { Say "reuse $folder ($($state.Why); built while this wrapper waited)"; "LANE_EXE $folder\wwe2k14.exe"; $result = 'ok'; throw 'lane_exe stop' }
    if ($state.State -eq 'Refuse') { "lane_exe: REFUSED, nothing built: $($state.Why)"; $rc = 6; throw 'lane_exe stop' }
    # Claim a slot: an OPEN HANDLE on <slot>.owner held to the end (B1: two wrappers never share one; the OS frees it if this wrapper dies). Prefer one already on
    # this commit, then a configured one. F8: poll for a free slot until the lock deadline (inside an inherited hold the 3-build cap does not bound it).
    $claim = Get-LaneSlotClaim $slots $Preset $full $Name $Slot ($LockWaitMinutes * 60) 10
    if (-not $claim) { "lane_exe: no free slot$(if ($Slot) { " (slot $Slot)" }) within $LockWaitMinutes min"; $rc = 7; throw 'lane_exe stop' }
    $mySlot = $claim.Slot; $slotFs = $claim.Stream
    $sl = $mySlot.Dir
    Say "slot $($mySlot.N) ($sl)"
    # F2 + F7: link / registration checks first, then checkout (detached, forced) and clean, or prune + add. Ignored files (generated, out\build) stay.
    $init = Initialize-LaneSlot $Main $sl $full
    if (-not $init.Ok) { "lane_exe: $($init.Why)"; $rc = 6; throw 'lane_exe stop' }
    if ($init.Cold) { $coldWarm = 'cold' }
    $ph.checkout = [int]$sw.Elapsed.TotalSeconds; $sw.Restart()
    # Copies (robocopy / Copy-Item only; no link of any kind). generated\default is copied once, inside the BUILD hold, then checked.
    foreach ($i in (Get-LaneExeInputs)) {
        $src = Join-Path $Main $i.Source; $dst = Join-Path $sl $i.Dest
        if (-not (Test-Path -LiteralPath $src)) { continue }   # an optional input that main lacks (the required ones were checked)
        if ($i.Kind -eq 'file') {
            if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $dst).Length -ne (Get-Item -LiteralPath $src).Length) {
                [void][System.IO.Directory]::CreateDirectory((Split-Path $dst -Parent)); Copy-Item -LiteralPath $src -Destination $dst -Force
            }
        } elseif ($i.Kind -eq 'gen') {
            # F1: copied to <dst>.copying, verified, renamed; a final folder that is not an exact copy is copied again.
            $g = Sync-LaneGenerated $src $dst
            if ($g.Error) { "lane_exe: $($g.Error)"; $rc = 6; throw 'lane_exe stop' }
            if ($g.Copied) { "lane_exe: generated: seeded from main ($($g.Why))" } else { "lane_exe: generated: kept ($($g.Why))" }
        } else {
            $have = Test-Path -LiteralPath $dst -PathType Container
            if ($have) { $a = Get-LaneTreeSize $src; $d = Get-LaneTreeSize $dst; $have = ($a.Files -eq $d.Files -and $a.Bytes -eq $d.Bytes) }
            if (-not $have) {
                [void][System.IO.Directory]::CreateDirectory($dst)
                & robocopy $src $dst /E /XJ /NFL /NDL /NJH /NJS /NP /MT:8 /R:1 /W:1 | Out-Null
                if ($LASTEXITCODE -ge 8) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue; "lane_exe: robocopy of $($i.Name) failed (exit $LASTEXITCODE)"; $rc = 6; throw 'lane_exe stop' }
            }
        }
    }
    $links = @(Test-LaneSlotLinks $sl)
    if ($links.Count) { $links | Select-Object -First 5 | ForEach-Object { "lane_exe: $_" }; "lane_exe: the slot holds $($links.Count) link(s): refused, a slot is link-free"; $rc = 6; throw 'lane_exe stop' }
    $ph.copy = [int]$sw.Elapsed.TotalSeconds; $sw.Restart()
    # Configure once (first use of this preset in the slot).
    $env:REXSDK = $sdk
    $logDir = Join-Path $exeRoot '.logs'; [void][System.IO.Directory]::CreateDirectory($logDir)
    $cache = "$sl\out\build\$Preset\CMakeCache.txt"
    # F3: the options of THIS slot's cache are what gets built: a slot whose cache differs from the wanted recipe (or holds a non-empty STRING / FILEPATH option) is re-configured here, in the hold.
    $needCfg = -not (Test-Path -LiteralPath $cache)
    if ($needCfg) { $coldWarm = 'cold' }
    else {
        $slotDiff = @(Get-LaneOptionDiff (Get-LaneCacheOptions $cache ([ref]@())) $options @(Get-LaneCacheUninitialized $cache))
        $slotStr = Get-LaneCacheStrings $cache
        if ($slotDiff.Count -or $slotStr.Count) { Say "slot cache differs from the wanted recipe ($($slotDiff -join ',')$(if ($slotStr.Count) { " strings: $($slotStr.Keys -join ',')" })): re-configuring"; $needCfg = $true }
    }
    if ($needCfg) {
        $d = (($options.GetEnumerator() | ForEach-Object { "-D$($_.Key)=$($_.Value)" }) + @(Get-LaneCacheStrings $cache | ForEach-Object { $_.Keys | ForEach-Object { "-D$_=" } })) -join ' '
        $cmd = ". '$Main\tools\env.ps1' *> `$null; Set-Location '$sl'; cmake --preset $Preset $d; exit `$LASTEXITCODE"
        & pwsh -NoProfile -Command $cmd *>&1 | Out-File -LiteralPath "$logDir\${Name}_configure.log"
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cache)) { "lane_exe: configure failed (exit $LASTEXITCODE); log $logDir\${Name}_configure.log"; $rc = 5; throw 'lane_exe stop' }
    }
    $slotOptions = Get-LaneCacheOptions $cache ([ref]@()); $slotStrings = Get-LaneCacheStrings $cache
    $absent = @(Get-LaneCacheUninitialized $cache | Where-Object { $options.Contains($_) })   # F4: wanted options this commit does not declare
    if ($absent.Count) { Say "options absent in this commit (recorded, not refused): $($absent -join ', ')" }
    $stillOff = @(Get-LaneOptionDiff $slotOptions $options $absent)
    if ($stillOff.Count -or $slotStrings.Count) { "lane_exe: the slot's cache still differs from the wanted recipe after configure ($($stillOff -join ',') $($slotStrings.Keys -join ',')): refused"; $rc = 6; throw 'lane_exe stop' }
    $ph.configure = [int]$sw.Elapsed.TotalSeconds; $sw.Restart()
    # Build (the child re-enters this BUILD hold through WWE2K14_RUNLOCK_TOKEN). F1: its pid goes into the owner, so a killed wrapper's live build keeps the slot busy.
    $bo = "$logDir\${Name}_build.out"; $be = "$logDir\${Name}_build.err"
    $bargs = @('-NoProfile', '-File', "`"$BuildScript`"", '-Root', "`"$sl`"", '-Preset', $Preset, '-Run', $Name, '-LockMode', 'Build', '-Jobs', "$Jobs")
    $bp = Start-Process pwsh -ArgumentList $bargs -NoNewWindow -PassThru -RedirectStandardOutput $bo -RedirectStandardError $be
    $null = $bp.Handle
    Set-LaneSlotChild $slotFs $Name $bp.Id
    $wb = Wait-LaneBuild $bp $BuildTimeoutMinutes
    $bexit = $wb.ExitCode
    if ($wb.TimedOut) { "lane_exe: the build ran past -BuildTimeoutMinutes $BuildTimeoutMinutes and was killed with its tree" }
    Clear-LaneSlotChild $slotFs   # F2: the child is gone; the owner holds only pid=
    $bl = @(Get-Content -LiteralPath $bo -ErrorAction SilentlyContinue) + @(Get-Content -LiteralPath $be -ErrorAction SilentlyContinue)
    $bl | Out-File -LiteralPath "$logDir\${Name}_build.log"
    Remove-Item -LiteralPath $bo, $be -Force -ErrorAction SilentlyContinue
    $bl | Select-Object -Last 12 | ForEach-Object { "build.ps1: $_" }
    $ph.build = [int]$sw.Elapsed.TotalSeconds; $sw.Restart()
    $bd = "$sl\out\build\$Preset"
    if ($wb.TimedOut -or $bexit -ne 0 -or -not (Test-Path -LiteralPath "$bd\wwe2k14.exe")) {
        # F9: the full build log is <slot>\logs\build_<Name>.log: keep a copy where the message points.
        $full_log = "$sl\logs\build_$Name.log"
        if (Test-Path -LiteralPath $full_log) { Copy-Item -LiteralPath $full_log -Destination "$logDir\${Name}_build_full.log" -Force }
        "lane_exe: build.ps1 exit $bexit; full log $logDir\${Name}_build_full.log (summary $logDir\${Name}_build.log)"; $rc = 5; throw 'lane_exe stop'
    }
    # Stage beside (per pid), publish by rename when complete.
    $temp = Join-Path $exeRoot ".$Name.$PID.tmp"
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    [void][System.IO.Directory]::CreateDirectory("$temp\stamp")
    foreach ($f in @('wwe2k14.exe', 'wwe2k14.map', 'wwe2k14_probes.dll', 'amd_fidelityfx_vk.dll')) {
        if (Test-Path -LiteralPath "$bd\$f") { Copy-Item -LiteralPath "$bd\$f" -Destination "$temp\$f" }
        elseif ($f -eq 'amd_fidelityfx_vk.dll' -and (Test-Path -LiteralPath "$sl\thirdparty\FidelityFX-SDK\ffx-api\bin\$f")) { Copy-Item -LiteralPath "$sl\thirdparty\FidelityFX-SDK\ffx-api\bin\$f" -Destination "$temp\$f" }
        elseif ($f -in 'wwe2k14.exe', 'amd_fidelityfx_vk.dll') { "lane_exe: $bd lacks $f"; $rc = 8; throw 'lane_exe stop' }
    }
    if (Test-Path -LiteralPath "$bd\stamp\wwe2k14_build_stamp.h") { Copy-Item -LiteralPath "$bd\stamp\wwe2k14_build_stamp.h" -Destination "$temp\stamp\" } else { "lane_exe: no stamp\wwe2k14_build_stamp.h in $bd"; $rc = 8; throw 'lane_exe stop' }
    foreach ($f in (Get-ChildItem -LiteralPath $bd -Filter 'rex*.dll' -File)) { Copy-Item -LiteralPath $f.FullName -Destination "$temp\$($f.Name)" }
    if ($Pdb) { foreach ($f in (Get-ChildItem -LiteralPath $bd -Filter '*.pdb' -File)) { Copy-Item -LiteralPath $f.FullName -Destination "$temp\$($f.Name)" } }
    if (-not $ShaderList) { $ShaderList = "$Main\out\build\$Preset\shader_list.txt" }
    if (Test-Path -LiteralPath $ShaderList) { Copy-Item -LiteralPath $ShaderList -Destination "$temp\shader_list.txt" }
    $nin = Join-Path $bd 'build.ninja'
    $sl_on = (Test-Path -LiteralPath $nin) -and [bool](Select-String -LiteralPath $nin -Pattern 'RDR_HAS_STREAMLINE' -SimpleMatch -Quiet)
    $pinLines = @($bl | ForEach-Object { "$_" } | Where-Object { $_ -match '^sdk pin' } | Select-Object -Last 2)
    $info = [ordered]@{ lane = $Lane; name = $Name; commit = $full; c9 = $c9; merge_base_session = "$mbase"; preset = $Preset; jobs = $Jobs
        options = $options; options_absent_in_commit = $absent; options_not_copied = $skipped; options_slot_cache = $slotOptions; options_strings = $slotStrings
        symbols_recipe = 'WWE2K14_EXE_SYMBOLS=OFF WWE2K14_EXE_SYMBOLS_GUEST_LINES=OFF'
        rdr_has_streamline = $sl_on; sdk_pin = $pinLines; slot = $mySlot.N
        sha256 = (Get-LaneFileSha "$temp\wwe2k14.exe"); built_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); pdb = [bool]$Pdb }
    ($info | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath "$temp\exe_info.json" -Encoding UTF8
    # B2: a rename, never a delete of the final folder; another builder that published first = Reuse (ours is dropped), a different folder = refused.
    $pub = Publish-LaneFolder $temp $folder $full $Preset $options
    if ($pub.Result -eq 'Published') { $temp = '' }
    elseif ($pub.Result -eq 'Reuse') { Say "reuse $folder ($($pub.Why)); this build is dropped" }
    else { "lane_exe: REFUSED to publish: $($pub.Why)"; $rc = 6; throw 'lane_exe stop' }
    Say "streamline in this exe: $sl_on; $($pinLines -join ' ')"
    $ph.stage = [int]$sw.Elapsed.TotalSeconds
    $result = 'ok'
    "LANE_EXE $folder\wwe2k14.exe"
} catch {
    if ($_.Exception.Message -ne 'lane_exe stop') { "lane_exe: $($_.Exception.Message)"; if (-not $rc) { $rc = 5 } }
} finally {
    if ($temp -and (Test-Path -LiteralPath $temp)) { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
    if ($bp -and -not $bp.HasExited) { & taskkill /T /F /PID $bp.Id 2>&1 | Out-Null }   # F1: an escaping error never leaves the build tree running without a hold
    if ($slotFs) { try { Clear-LaneSlotChild $slotFs } catch { }; $slotFs.Dispose() }   # the owner file stays (never deleted: the handle is the claim)
    Exit-RunLock $lock
    try {
        [void][System.IO.Directory]::CreateDirectory($exeRoot)
        $tsv = Join-Path $exeRoot 'lane_exe.tsv'
        if (-not (Test-Path -LiteralPath $tsv)) { [System.IO.File]::AppendAllText($tsv, "time`tname`tlane`tcommit`tslot`ttemp`tlock_s`tcheckout_s`tcopy_s`tconfigure_s`tbuild_s`tstage_s`ttotal_s`tresult`n") }
        $row = @((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $Name, $Lane, $c9, $(if ($mySlot) { $mySlot.N } else { '' }), $coldWarm, $ph.lock, $ph.checkout, $ph.copy, $ph.configure, $ph.build, $ph.stage, [int]((Get-Date) - $t0).TotalSeconds, $(if ($result -eq 'ok') { 'ok' } else { "fail($rc)" })) -join "`t"
        [System.IO.File]::AppendAllText($tsv, "$row`n")
    } catch { }
}
if ($result -ne 'ok') { if (-not $rc) { $rc = 5 }; exit $rc }
exit 0
