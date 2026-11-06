# Fast incremental SDK build of a renderer builder's worktree (Session 82, harness builder).
#   & .\tools\owned\fast_build.ps1 -Worktree C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\s82-p0 -Name p0
#   pwsh -NoProfile -File tools\owned\fast_build.ps1 -Worktree <wt> -Name p0         (also fine: no array arguments)
#
# What it does (the old way, tools\bisect\wt_build.ps1, checks one COMMIT out into a separate "-build" worktree under an
# EXCLUSIVE run lock; the builds themselves were 14-48 s, the 2-3 min per build were the queue plus the checkout touching
# files and a new exe folder per commit, each new folder also triggering a Windows firewall prompt):
#   1. builds the worktree's CURRENT WORKING TREE (uncommitted edits included, no commit, no checkout) incrementally in a
#      persistent build folder of its own, <worktree>\out\build\win-amd64 (configured once like the main SDK build:
#      preset win-amd64 = Ninja Multi-Config + clang, build_sdk.ps1's Release flags, the main SDK cache's REXGLUE_*
#      options). Only the two targets runs need are built: rexgpu-xenos and rexruntime (no Tracy, no rexglue.exe, no tests).
#   2. takes the BUILD class of the run lock (tools\runlock.ps1 "BUILD class"): it shares the PC with untimed game runs,
#      never runs next to a timed (exclusive) run, and at most 2 builds run at once.
#   3. stages the two DLLs (+ PDBs) in ONE fixed exe folder per builder, out\harness\bisect\exe\live_<Name>\ - a copy of
#      the main exe made once (wwe2k14.exe, .map, amd_fidelityfx_vk.dll, shader_list.txt if the main folder has one);
#      later builds replace only the rex DLLs that changed. One folder = one firewall prompt, ever.
#   The submodule folders of the worktree are junctions to the main SDK checkout ONLY while this script builds (created
#   after the safety checks, removed in the finally block, exactly as wt_build.ps1; the commit's submodule pins must equal the
#   main tree's checked-out ones). NEVER delete these worktrees with "git worktree remove": tools\bisect\wt_build.ps1 -Clean.
#   Do not "git add -A" / commit while a build runs (the junctions exist for ~1 min); stage your files by name.
#
# The main exe files in live_<Name> (wwe2k14.exe, .map, amd_fidelityfx_vk.dll, shader_list.txt) are re-copied whenever the main folder's copy
# differs in size or time (a rebuilt main exe), never while a game runs from the folder (exit 9). Staging is serialized per
# exe folder (stage.lock) and any failed copy fails the build (exit 3/6), so a run never loads old or mixed DLLs silently.
# The exe is NOT rebuilt: never change a header the game exe includes in a way that changes its layout or virtuals
# (include\rex\graphics\vulkan\command_processor.h public / virtual members, cvar names the game sets). New cvars are fine.
# The DLLs are unpinned (tools\build.ps1 would refuse them): live_<Name> is for checks only, never copied to the main exe folder.
#
# Runs from the staged exe (the build prints these too):
#   -Exe C:\Users\Jarrod\wwe2k14-recomp\out\harness\bisect\exe\live_<Name>\wwe2k14.exe    (run_list -Exe / owned_check -Exe)
#   --game_data_root=C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted                    (always)
#   --wwe2k14_replay_dir=C:\Users\Jarrod\wwe2k14-recomp\out\replays                        (else no replay: the attract demo plays)
#   --wwe2k14_scenario_dir=C:\Users\Jarrod\wwe2k14-recomp\out\scenarios
#   the NATIVE settings: --wwe2k14_settings_file=C:\Users\Jarrod\wwe2k14-recomp\out\owned_check\settings_native720.json (or ..._native1440.json)
#       --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false
#   owned_check.ps1 -Exe <exe> adds all of these itself. Never run from this folder without --wwe2k14_settings_file
#   (the game would read Jarrod's real launcher settings).
#
# Options: -Reconfigure (re-run CMake), -Jobs N (default 10: three builds may run at once), -NoCache (no sccache), -RefreshExe (copy the main exe
# again even if equal), -ExeSource DIR (default the main exe folder, read only), -NoLock (no run lock hold; not for unattended use),
# -LockWaitMinutes N, -Log PATH. Exit codes: 0 ok, 2 bad arguments, 3 failure, 4 lock timeout, 5 build failed, 6 DLLs missing or
# the staged ones are not the built ones, 7 another fast_build is running on this worktree, 8 another fast_build has held this
# exe folder's stage.lock for 3 min, 9 a game is running from the staged exe folder (DLLs built, not staged).
# Output ends with "FAST_BUILD_EXE <path>" on success.
param([Parameter(Mandatory)][string]$Worktree, [Parameter(Mandatory)][string]$Name,
      [switch]$Reconfigure, [int]$Jobs = 10, [switch]$RefreshExe, [string]$ExeSource = '', [switch]$NoLock,
      [double]$LockWaitMinutes = 30, [string]$Log = '', [switch]$NoCache)
# Session 94 (Jarrod: "Cache s would save an amazing amount of time"): compiles go through sccache when it is installed
# (winget Mozilla.sccache; -NoCache = without). SCCACHE_BASEDIRS = this worktree, so every worktree of the same commit hits
# the same cache entries (the paths differ only by the worktree folder). A build folder configured before the cache existed
# is reconfigured once (one full build fills the cache). -Jobs default 10: three builds may run at once (runlock.ps1).
$ErrorActionPreference = 'Continue'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$SdkSrc = "$R\thirdparty\rexglue-sdk-src"
if ($Name -notmatch '^[A-Za-z0-9_-]{1,40}$') { "fast_build: -Name must be letters, digits, - or _ (got '$Name')"; exit 2 }
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
if ($Worktree -ieq $SdkSrc) { 'fast_build: refusing the main SDK checkout (thirdparty\rexglue-sdk-src); use tools\build_sdk.ps1 there'; exit 2 }
if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { "fast_build: $Worktree is not a git worktree (no .git file)"; exit 2 }
if (-not (Test-Path -LiteralPath "$Worktree\CMakePresets.json")) { "fast_build: $Worktree has no CMakePresets.json: not an SDK worktree"; exit 2 }
if (-not $ExeSource) { $ExeSource = "$R\out\build\win-amd64-release" }
if (-not $Log) { $Log = "$R\out\harness\bisect\fast_build_$Name.log" }
$Log = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Log)
$null = New-Item -ItemType Directory -Force -Path (Split-Path $Log -Parent)
Set-Content -LiteralPath $Log -Value "fast_build $Name $((Get-Date).ToString('s')) worktree $Worktree"
function Say([string]$m) { Write-Host $m; Add-Content -LiteralPath $Log -Value $m }

# --- junction safety (same as tools\bisect\wt_build.ps1)
function Find-Links([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
    $di = [System.IO.DirectoryInfo]::new($Dir)
    if ($di.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $di.FullName; return }
    foreach ($e in $di.EnumerateFileSystemInfos()) {
        if ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $e.FullName }
        elseif ($e -is [System.IO.DirectoryInfo]) { Find-Links $e.FullName }
    }
}
# Only the worktree's own source folders are searched (thirdparty\, the junction places); out\ is the build folder.
function Remove-Links([string]$Dir) {
    $n = 0
    foreach ($l in @(Find-Links $Dir)) {
        $item = Get-Item -LiteralPath $l -Force
        if ($item -is [System.IO.DirectoryInfo]) {
            [System.IO.Directory]::Delete($l, $false)
            # An empty folder stays (git reads it as the uninitialized submodule it is; a missing folder would show as a
            # deleted submodule in "git status" and be committed by "git commit -a").
            $null = New-Item -ItemType Directory -Path $l
        } else { [System.IO.File]::Delete($l) }
        $n++
    }
    if (@(Find-Links $Dir).Count) { throw "links still present under ${Dir}" }
    return $n
}
function New-Junction([string]$Link, [string]$Target) {
    if (Test-Path -LiteralPath $Link) {
        $item = Get-Item -LiteralPath $Link -Force
        if ($item.LinkType -eq 'Junction' -and ($item.Target -replace '\\$', '') -ieq ($Target -replace '\\$', '')) { return }
        if ($item.LinkType -ne 'Junction' -and @(Get-ChildItem -LiteralPath $Link -Force).Count -eq 0) { Remove-Item -LiteralPath $Link -Force }
        else { throw "$Link exists and is not a junction to $Target" }
    }
    $null = New-Item -ItemType Junction -Path $Link -Target $Target
}

# --- one fast_build per worktree
$null = New-Item -ItemType Directory -Force -Path "$Worktree\out"
$mutexFile = "$Worktree\out\fast_build.lock"
$mutex = $null
try { $mutex = [System.IO.File]::Open($mutexFile, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) }
catch { Say "fast_build: another fast_build is running on $Worktree (lock $mutexFile). Not started."; exit 7 }

$t0 = Get-Date
$rc = 0
$lock = $null
$exeLine = ''
$tConf = 0; $tBuild = 0; $tStage = 0; $waited = 0
try {
    # State of the tree BEFORE any junction exists.
    $full = (git -C $Worktree rev-parse HEAD).Trim()
    $branch = (git -C $Worktree rev-parse --abbrev-ref HEAD).Trim()
    $dirty = @(git -C $Worktree status --porcelain --untracked-files=no).Count
    $dirtyNote = if ($dirty) { "$dirty changed file(s) on top of" } else { 'clean at' }
    Say "worktree $Worktree ($branch): $dirtyNote $($full.Substring(0, 9))"

    if (-not $NoLock) {
        . "$R\tools\runlock.ps1"
        $script:RunLockMsgToStderr = $false
        $tl = Get-Date
        try { $lock = Enter-RunLock -Name "fast_build $Name" -Mode Build -TimeoutMinutes $LockWaitMinutes }
        catch { Say "fast_build: $($_.Exception.Message)"; $rc = 4; throw }
        $waited = ((Get-Date) - $tl).TotalSeconds
    }
    $tWork = Get-Date

    # Links left by an interrupted build go first.
    $null = Remove-Links "$Worktree\thirdparty"
    # Submodules: junctions to the main SDK checkout when the commit pins the same commit (no fetching).
    $subs = @(git -C $SdkSrc ls-tree -r $full 2>$null | Where-Object { $_ -match '^160000 commit ' })
    if (-not $subs.Count) { $subs = @(git -C $Worktree ls-tree -r HEAD | Where-Object { $_ -match '^160000 commit ' }) }
    foreach ($s in $subs) {
        $parts = $s -split '\s+', 4
        $sha = $parts[2]; $path = $parts[3] -replace '/', '\'
        if (-not (Test-Path -LiteralPath "$SdkSrc\$path\.git")) { continue }   # not initialized in the main tree either: the SDK build does not use it
        $mainSha = (git -C "$SdkSrc\$path" rev-parse HEAD 2>$null)
        if (-not $mainSha -or $mainSha.Trim() -ne $sha) { throw "submodule $path pinned at $($sha.Substring(0, 9)) here, main SDK tree has ${mainSha}: would need a fetch (not done)" }
        if ((Test-Path -LiteralPath "$Worktree\$path") -and @(Get-ChildItem -LiteralPath "$Worktree\$path" -Force).Count -gt 0 -and
            -not ((Get-Item -LiteralPath "$Worktree\$path" -Force).LinkType)) { continue }   # populated here: leave it
        New-Junction "$Worktree\$path" "$SdkSrc\$path"
    }

    . "$R\tools\env.ps1" *> $null
    $ErrorActionPreference = 'Continue'
    Set-Location $Worktree
    $bdir = "$Worktree\out\build\win-amd64"
    $sccache = ''
    if (-not $NoCache) {
        $c = Get-Command sccache -ErrorAction SilentlyContinue
        $sccache = if ($c) { $c.Source } elseif (Test-Path "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe") { "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe" } else { '' }
    }
    $launcherOpts = @()
    if ($sccache) {
        $env:SCCACHE_BASEDIRS = $Worktree
        # The base directory is fixed when the sccache server starts (the first build's worktree, Session 94: 0 hits across
        # worktrees on one shared server), so each worktree gets its own server on its own port; all share one cache folder.
        $h = 0; foreach ($ch in $Worktree.ToLowerInvariant().ToCharArray()) { $h = ($h * 31 + [int]$ch) % 1000003 }
        $env:SCCACHE_SERVER_PORT = [string](4300 + ($h % 600))
        if (-not $env:SCCACHE_CACHE_SIZE) { $env:SCCACHE_CACHE_SIZE = '40G' }
        $launcherOpts = @("-DCMAKE_C_COMPILER_LAUNCHER=$sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache")
        if ((Test-Path "$bdir\CMakeCache.txt") -and -not (Select-String -LiteralPath "$bdir\CMakeCache.txt" -Pattern '^CMAKE_CXX_COMPILER_LAUNCHER:.*sccache' -Quiet)) {
            Say "sccache: this build folder was configured without the cache - reconfiguring once"
            $Reconfigure = $true
        }
        Say "sccache: on ($sccache, basedir $Worktree)"
    } elseif (-not $NoCache) { Say "sccache: not installed (winget install Mozilla.sccache) - compiling without a cache" }
    if ($Reconfigure -or -not (Test-Path "$bdir\CMakeCache.txt")) {
        $mainCache = "$SdkSrc\out\build\win-amd64\CMakeCache.txt"
        $opts = @(if (Test-Path -LiteralPath $mainCache) {
            Get-Content -LiteralPath $mainCache | Where-Object { $_ -match '^REXGLUE_[A-Z0-9_]+:BOOL=' } | ForEach-Object { '-D' + $_ }
        })
        Say "configure (preset win-amd64, build_sdk.ps1 Release flags, the main cache's options: $($opts -join ' '))"
        $tc = Get-Date
        cmake --preset win-amd64 @opts @launcherOpts "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" `
            "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" `
            "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" *>&1 | Out-File -Append -LiteralPath $Log
        $tConf = ((Get-Date) - $tc).TotalSeconds
        if ($LASTEXITCODE) { Say "configure failed (exit $LASTEXITCODE), see $Log"; $rc = 5; throw 'configure failed' }
    }
    Say "build rexgpu-xenos rexruntime (Release, -j $Jobs) in $bdir"
    $tb = Get-Date
    cmake --build $bdir --config Release --target rexgpu-xenos rexruntime -j $Jobs *>&1 | Out-File -Append -LiteralPath $Log
    $code = $LASTEXITCODE
    $tBuild = ((Get-Date) - $tb).TotalSeconds
    if ($code) {
        Say "build FAILED (exit $code) after $([int]$tBuild) s:"
        Get-Content -LiteralPath $Log | Select-String ': error|FAILED|error:' | Select-Object -First 12 | ForEach-Object { Say ('  ' + $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length))) }
        $rc = 5; throw 'build failed'
    }
    Say ("build ok, {0:N1} s" -f $tBuild)

    # --- stage (review fixes: every copy -ErrorAction Stop so a locked / failed copy fails the build instead of leaving old
    # or mixed DLLs behind with exit 0; a game running from the folder is checked before AND after; two builds staging
    # into the same live_ folder are serialized by <exeDir>\stage.lock; the main exe files are re-copied whenever the main
    # folder's copy differs)
    $ts = Get-Date
    $exeDir = "$R\out\harness\bisect\exe\live_$Name"
    $null = New-Item -ItemType Directory -Force -Path $exeDir
    function Get-InUse { @(Get-Process -Name wwe2k14 -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and $_.Path -like "$exeDir\*" } catch { $false } }) }
    $stageLock = $null
    $until = (Get-Date).AddMinutes(3)
    while (-not $stageLock) {
        try { $stageLock = [System.IO.File]::Open("$exeDir\stage.lock", [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) }
        catch { if ((Get-Date) -gt $until) { Say "another fast_build has been staging into $exeDir for 3 min (stage.lock)"; $rc = 8; throw 'stage lock busy' }; Start-Sleep -Milliseconds 500 }
    }
    try {
        $inUse = Get-InUse
        if ($inUse.Count) { Say "a game (pid $(($inUse | ForEach-Object Id) -join ',')) is running from ${exeDir}: the DLLs are built but NOT staged (exit 9). Quit it and run again."; $rc = 9; throw 'exe in use' }
        foreach ($n in 'wwe2k14.exe', 'wwe2k14.map', 'amd_fidelityfx_vk.dll', 'shader_list.txt') {
            $s = "$ExeSource\$n"; $d = "$exeDir\$n"
            if (-not (Test-Path -LiteralPath $s)) { continue }
            $differs = -not (Test-Path -LiteralPath $d)
            if (-not $differs) { $si = Get-Item -LiteralPath $s; $di = Get-Item -LiteralPath $d; $differs = ($si.Length -ne $di.Length) -or ($si.LastWriteTimeUtc -ne $di.LastWriteTimeUtc) }
            if ($RefreshExe -or $differs) {
                Copy-Item -LiteralPath $s -Destination $d -Force -ErrorAction Stop
                Say "staged $n from $ExeSource (new, different or -RefreshExe)"
            }
        }
        $outDir = "$Worktree\out\win-amd64\Release"
        $copied = 0
        # Two phases (review fix): every changed file is first copied to <name>.new; only when ALL copies worked are they renamed
        # into place, and a BROKEN marker (which preflight.ps1 refuses) stands from the first rename until the verification
        # below passed, so a failure between two renames can never leave new xenos + old runtime looking healthy.
        $todo = @()
        try {
            foreach ($n in 'rexgpu-xenos', 'rexruntime') {
                foreach ($ext in 'dll', 'pdb') {
                    $s = "$outDir\$n.$ext"; $d = "$exeDir\$n.$ext"
                    if (-not (Test-Path -LiteralPath $s)) {
                        if ($ext -eq 'dll') { Say "no $n.dll in $outDir after the build"; $rc = 6; throw "missing $n.dll" }
                        continue
                    }
                    $si = Get-Item -LiteralPath $s
                    if ((Test-Path -LiteralPath $d) -and (Get-Item -LiteralPath $d).Length -eq $si.Length -and (Get-Item -LiteralPath $d).LastWriteTimeUtc -eq $si.LastWriteTimeUtc) { continue }
                    Copy-Item -LiteralPath $s -Destination "$d.new" -Force -ErrorAction Stop
                    $todo += $d
                }
            }
        } catch {
            Get-ChildItem -LiteralPath $exeDir -Filter '*.new' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
            throw
        }
        if ($todo.Count) {
            Set-Content -LiteralPath "$exeDir\BROKEN" -Value "fast_build $Name was replacing DLLs at $((Get-Date).ToString('s')) and did not finish: run tools\owned\fast_build.ps1 again" -ErrorAction Stop
            foreach ($d in $todo) { Move-Item -LiteralPath "$d.new" -Destination $d -Force -ErrorAction Stop; $copied++ }
        }
        # Verify: both DLLs staged are the ones just built (never one new and one old), and nobody started a game meanwhile.
        foreach ($n in 'rexgpu-xenos', 'rexruntime') {
            $si = Get-Item -LiteralPath "$outDir\$n.dll"; $di = Get-Item -LiteralPath "$exeDir\$n.dll"
            if ($si.Length -ne $di.Length -or $si.LastWriteTimeUtc -ne $di.LastWriteTimeUtc) { Say "staged $n.dll is not the one just built (stale / mixed DLLs): failing"; $rc = 6; throw "stage verify $n" }
        }
        Remove-Item -LiteralPath "$exeDir\BROKEN" -Force -ErrorAction SilentlyContinue
        $inUse = Get-InUse
        if ($inUse.Count) { Say "a game (pid $(($inUse | ForEach-Object Id) -join ',')) started from ${exeDir} while the DLLs were staged: it may have loaded old or mixed DLLs. Quit it and build again (exit 9)."; $rc = 9; throw 'exe in use after staging' }
        foreach ($sub in 'logs', 'captures') { $null = New-Item -ItemType Directory -Force -Path "$exeDir\$sub" }
        Set-Content -LiteralPath "$exeDir\build_info.txt" -Value @("builder: $Name", "worktree: $Worktree", "branch: $branch", "commit: $full ($dirtyNote)", "built: $((Get-Date).ToString('s'))", "dlls replaced this build: $copied") -ErrorAction Stop
    } finally { if ($stageLock) { $stageLock.Dispose() } }
    $tStage = ((Get-Date) - $ts).TotalSeconds
    Say ("staged {0} DLL/PDB file(s) in {1:N1} s -> {2}" -f $copied, $tStage, $exeDir)
    $exeLine = "FAST_BUILD_EXE $exeDir\wwe2k14.exe"
} catch {
    if (-not $rc) { Say "fast_build failed: $($_.Exception.Message)"; $rc = 3 }
} finally {
    Set-Location $R
    if (Test-Path -LiteralPath $Worktree) {
        try { $n = Remove-Links "$Worktree\thirdparty"; if ($n) { Say "$n junction(s) removed from $Worktree" } }
        catch { Say "WARNING: $($_.Exception.Message). Do NOT delete this worktree with git; use tools\bisect\wt_build.ps1 -Clean."; if (-not $rc) { $rc = 3 } }
    }
    if ($lock) { Exit-RunLock $lock }
    if ($mutex) { $mutex.Dispose() }
}
$tot = ((Get-Date) - $t0).TotalSeconds
Say ("fast_build {0}: exit {1}; total {2:N1} s (lock wait {3:N1} s, configure {4:N1} s, build {5:N1} s, stage {6:N1} s)" -f $Name, $rc, $tot, $waited, $tConf, $tBuild, $tStage)
if ($rc) { exit $rc }
Say "runs: -Exe $exeDir\wwe2k14.exe  with  --game_data_root=$R\game_data\extracted --wwe2k14_replay_dir=$R\out\replays --wwe2k14_scenario_dir=$R\out\scenarios  (owned_check -Exe adds them; settings: $R\out\owned_check\settings_native720.json)"
$exeLine
exit 0
