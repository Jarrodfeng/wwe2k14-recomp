# Build one commit for tools\bisect\autobisect.py commits mode in a scratch git worktree (Session 8, task 14).
# The main tree, its build folder, its exe folder and the installed SDK are never written. Called by autobisect.py under
# the run lock (tools\runlock.ps1 run ...), so no game run is going while this loads the CPU; prints
# "BISECT_EXE <path>" on success.
#
# JUNCTIONS (fixer, review finding 1): the worktrees hold junctions into the main tree (game_data\extracted; the SDK
# submodule checkouts). "git worktree remove --force" FOLLOWS a junction and deletes the target's contents (reproduced
# with Git 2.52.0.windows.1), so: the junctions exist only while a build runs (created after the checkout, removed in
# the finally block, also removed before every checkout), and the only supported way to delete a worktree is
#   pwsh -NoProfile -File tools\bisect\wt_build.ps1 -Clean -Repo ours|sdk [-Worktree PATH]
# which removes every reparse point in the tree as a link only, re-checks that none is left, then deletes the folder
# and runs "git worktree prune". NEVER use "git worktree remove" on these worktrees.
#
# OWNER (review finding 3): <worktree>.owner names the bisect (pid) that uses the worktree. A call with -Owner PID
# refuses (exit 7) when another live process owns it; a call without -Owner refuses when any live owner is recorded.
# autobisect.py deletes its owner file when it ends; an owner whose process is gone is taken over.
#
#   -Repo ours: worktree of this repo at -Worktree (detached at -Commit; created on first use, re-checked-out after),
#     with its own build folder <worktree>\out\build\win-amd64-release. game_data\extracted inside it is a junction to
#     the main game_data\extracted during the build only (read by codegen: the manifest names
#     game_data/extracted/default.xex; nothing writes there; game runs get --game_data_root from autobisect.py).
#     generated\ is NOT copied from the main tree: ninja reruns a custom command that has no entry in the (fresh)
#     build folder's .ninja_log, so the codegen step runs on the first build (~90 s) whatever is copied; after that a
#     checkout only touches files that differ, so codegen reruns only for commits whose manifest/config differ.
#     SDK: the main tools\env.ps1 (VS, LLVM, REXSDK = the pinned installed SDK); the exe folder gets the rex*.dll/pdb
#     of -DllSource (autobisect.py passes its exe snapshot taken once at the start and pin-checked; default the main
#     exe folder), so only our code differs between commits.
#   -Repo sdk: worktree of thirdparty\rexglue-sdk-src at thirdparty\sdk-wt\bisect (detached at -Commit). During the
#     build each submodule folder is a junction to the main SDK tree's checkout, allowed only when the commit records
#     the same submodule commit as the main tree has checked out (no fetching, nothing external). Configured with the
#     worktree's own "win-amd64" preset (Ninja Multi-Config, clang, -march=x86-64-v2) plus build_sdk.ps1's Release
#     flags plus every REXGLUE_* option of the main SDK build cache (REXGLUE_USE_VULKAN=ON ...); built (no install:
#     the install prefix is the shared SDK) into <sdk worktree>\out\build\win-amd64; then
#     out\harness\bisect\exe\sdk_<commit9>\ gets a copy of -ExeSource's wwe2k14.exe/.map (autobisect.py passes its
#     exe snapshot) and this build's rexgpu-xenos / rexruntime DLLs + PDBs. These DLLs are unpinned: expected for
#     bisect, and they never go into the main exe folder. The exe is not rebuilt against the commit's headers.
param([ValidateSet('ours', 'sdk')][string]$Repo = 'ours', [string]$Commit = '',
      [string]$Worktree = '', [string]$Log = '', [string]$ExeSource = '', [string]$DllSource = '',
      [string]$Owner = '', [string]$MainRepo = '', [switch]$Clean, [switch]$DryRun)
$ErrorActionPreference = 'Continue'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$SdkSrc = "$R\thirdparty\rexglue-sdk-src"
if (-not $MainRepo) { $MainRepo = if ($Repo -eq 'ours') { $R } else { $SdkSrc } }
if (-not $Worktree) { $Worktree = if ($Repo -eq 'ours') { "$R\out\harness\bisect\wt\ours" } else { "$R\thirdparty\sdk-wt\bisect" } }
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
if (-not $Clean -and -not $Commit) { 'wt_build: -Commit is required (or -Clean)'; exit 2 }
$tag = if ($Clean) { 'clean' } else { $Commit.Substring(0, [Math]::Min(9, $Commit.Length)) }
if (-not $Log) { $Log = "$R\out\harness\bisect\wt_build_$tag.log" }
$Log = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Log)   # absolute: the build does Set-Location
$null = New-Item -ItemType Directory -Force -Path (Split-Path $Log -Parent)
function Say([string]$m) { Write-Host $m; Add-Content -LiteralPath $Log -Value $m }

# --- junction safety
# Every reparse point (junction / symlink) under $Dir, WITHOUT descending into any of them.
function Find-Links([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
    $di = [System.IO.DirectoryInfo]::new($Dir)
    if ($di.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $di.FullName; return }
    foreach ($e in $di.EnumerateFileSystemInfos()) {
        if ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $e.FullName }
        elseif ($e -is [System.IO.DirectoryInfo]) { Find-Links $e.FullName }
    }
}
# Removes each link itself (never its target's contents). Returns the number removed; throws if one is left.
function Remove-Links([string]$Dir) {
    $n = 0
    foreach ($l in @(Find-Links $Dir)) {
        $item = Get-Item -LiteralPath $l -Force
        if ($item -is [System.IO.DirectoryInfo]) { [System.IO.Directory]::Delete($l, $false) }   # non-recursive: the link only
        else { [System.IO.File]::Delete($l) }
        Say "removed link $l"
        $n++
    }
    $left = @(Find-Links $Dir)
    if ($left.Count) { throw "links still present under ${Dir}: $($left -join ', ')" }
    return $n
}

# --- owner file
$OwnerFile = "$Worktree.owner"
function Test-OwnerAlive([string]$text) {
    if ($text -match 'pid=(\d+)') { return [bool](Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue) }
    return $false
}
function Assert-Owner {
    if (Test-Path -LiteralPath $OwnerFile) {
        $cur = (Get-Content -LiteralPath $OwnerFile -Raw).Trim()
        $mine = $Owner -and ($cur -match "pid=$Owner\b")
        if (-not $mine -and (Test-OwnerAlive $cur)) {
            Say "worktree $Worktree is in use: $cur (live). Refusing (wait for that bisect to finish)."
            exit 7
        }
        if (-not $mine) { Say "stale owner file ($cur, process gone): taking over" }
    }
    if ($Owner -and -not $DryRun) { Set-Content -LiteralPath $OwnerFile -Value "pid=$Owner since $((Get-Date).ToString('s'))" }
}

# --- -Clean: the safe way to delete a bisect worktree
if ($Clean) {
    try {
        if (Test-Path -LiteralPath $OwnerFile) {
            $cur = (Get-Content -LiteralPath $OwnerFile -Raw).Trim()
            if (Test-OwnerAlive $cur) { Say "worktree $Worktree is in use: $cur (live). Not cleaned."; exit 7 }
        }
        if (-not (Test-Path -LiteralPath $Worktree)) { Say "no worktree at $Worktree" }
        else {
            if ($Worktree -ieq ([System.IO.Path]::GetFullPath($MainRepo).TrimEnd('\'))) { throw "refusing to delete the main tree $Worktree" }
            if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { throw "$Worktree has no .git file: not a git worktree, not deleted" }
            $n = Remove-Links $Worktree
            Say "removed $n link(s); none left under $Worktree"
            if ($DryRun) { Say "dry run: would delete $Worktree"; exit 0 }
            Remove-Item -LiteralPath $Worktree -Recurse -Force
            if (Test-Path -LiteralPath $Worktree) { throw "could not delete $Worktree" }
            Say "deleted $Worktree"
        }
        git -C $MainRepo worktree prune 2>&1 | ForEach-Object { Say "  $_" }
        if (Test-Path -LiteralPath $OwnerFile) { Remove-Item -LiteralPath $OwnerFile -Force }
        Say "git worktree prune done"
        exit 0
    } catch { Say "wt_build -Clean failed: $($_.Exception.Message)"; exit 3 }
}

function Set-Worktree([string]$Main, [string]$Path, [string]$Rev) {
    $full = (git -C $Main rev-parse --verify "$Rev^{commit}" 2>$null)
    if (-not $full) { throw "unknown commit $Rev in $Main" }
    if (-not (Test-Path -LiteralPath "$Path\.git")) {
        Say "worktree add $Path at $full"
        if (-not $DryRun) { git -C $Main worktree add --detach $Path $full 2>&1 | ForEach-Object { Say "  $_" } }
    } else {
        Say "worktree $Path checkout $full"
        if (-not $DryRun) { git -C $Path checkout --detach --force $full 2>&1 | ForEach-Object { Say "  $_" } }
    }
    if (-not $DryRun) {
        $head = (git -C $Path rev-parse HEAD).Trim()
        if ($head -ne $full) { throw "worktree HEAD $head is not $full" }
    }
    return $full
}

function New-Junction([string]$Link, [string]$Target) {
    if (Test-Path -LiteralPath $Link) {
        $item = Get-Item -LiteralPath $Link -Force
        if ($item.LinkType -eq 'Junction' -and ($item.Target -replace '\\$', '') -ieq ($Target -replace '\\$', '')) { return }
        if ($item.LinkType -ne 'Junction' -and @(Get-ChildItem -LiteralPath $Link -Force).Count -eq 0) {
            Remove-Item -LiteralPath $Link -Force   # an empty submodule folder from the checkout
        } else { throw "$Link exists and is not a junction to $Target" }
    }
    Say "junction $Link -> $Target"
    if (-not $DryRun) { $null = New-Item -ItemType Junction -Path $Link -Target $Target }
}

$t0 = Get-Date
$rc = 0
$exeLine = ''
Assert-Owner   # before the try: a refused call must not touch the other owner's junctions in the finally block
try {
    # Links left by an interrupted build go before the checkout (git must never see them).
    if (-not $DryRun -and (Test-Path -LiteralPath $Worktree)) { $null = Remove-Links $Worktree }
    if ($Repo -eq 'ours') {
        if (-not $DllSource) { $DllSource = "$R\out\build\win-amd64-release" }
        $full = Set-Worktree $MainRepo $Worktree $Commit
        if (-not $DryRun) { $null = New-Item -ItemType Directory -Force -Path "$Worktree\game_data" }
        New-Junction "$Worktree\game_data\extracted" "$R\game_data\extracted"
        # Session 21: src\native_zlib.h (Session 15b) includes stb_image.h from the SDK source tree by a relative path;
        # read-only, a junction during the build like game_data\extracted.
        if (Test-Path -LiteralPath "$SdkSrc\thirdparty\stb") {
            if (-not $DryRun) { $null = New-Item -ItemType Directory -Force -Path "$Worktree\thirdparty\rexglue-sdk-src\thirdparty" }
            New-Junction "$Worktree\thirdparty\rexglue-sdk-src\thirdparty\stb" "$SdkSrc\thirdparty\stb"
        }
        if ($DryRun) { $exeLine = "BISECT_EXE (dry run) $Worktree\out\build\win-amd64-release\wwe2k14.exe" }
        else {
            . "$R\tools\env.ps1" *> $null
            $ErrorActionPreference = 'Continue'
            Set-Location $Worktree
            $bdir = "$Worktree\out\build\win-amd64-release"
            if (-not (Test-Path "$bdir\CMakeCache.txt")) {
                Say "configure (cmake --preset win-amd64-release) in $Worktree"
                cmake --preset win-amd64-release *>&1 | Out-File -Append -LiteralPath $Log
                if ($LASTEXITCODE) { Say "configure failed (exit $LASTEXITCODE)"; $rc = 4 }
            }
            if (-not $rc) {
                # generated/rexglue.cmake includes generated\default\sources.cmake only if it exists at configure
                # time, so the build that first runs codegen (or a codegen that changes the file list) needs a
                # reconfigure + second build.
                $srcList = "$Worktree\generated\default\sources.cmake"
                $listBefore = if (Test-Path -LiteralPath $srcList) { (Get-FileHash -LiteralPath $srcList).Hash } else { '' }
                if ($listBefore -and -not (Select-String -LiteralPath "$bdir\build.ninja" -Pattern 'wwe2k14_recomp' -SimpleMatch -Quiet)) {
                    Say "generated sources exist but $bdir was configured without them: reconfigure"
                    cmake -S $Worktree -B $bdir *>&1 | Out-File -Append -LiteralPath $Log
                }
                Say "build $full in $bdir"
                cmake --build $bdir -j 16 *>&1 | Out-File -Append -LiteralPath $Log
                $code = $LASTEXITCODE
                $listAfter = if (Test-Path -LiteralPath $srcList) { (Get-FileHash -LiteralPath $srcList).Hash } else { '' }
                if ($listAfter -and $listAfter -ne $listBefore) {
                    Say "codegen wrote a new generated source list (first build exit $code): reconfigure + build again"
                    cmake -S $Worktree -B $bdir *>&1 | Out-File -Append -LiteralPath $Log
                    cmake --build $bdir -j 16 *>&1 | Out-File -Append -LiteralPath $Log
                    $code = $LASTEXITCODE
                }
                Say ("build exit $code, {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
                if ($code) {
                    Get-Content -LiteralPath $Log | Select-String 'error|FAILED' | Select-Object -First 10 | ForEach-Object { Say $_.Line }
                    $rc = 5
                } else {
                    $dlls = @(Get-ChildItem "$DllSource\rex*.dll", "$DllSource\rex*.pdb" -File -ErrorAction SilentlyContinue)
                    if (-not @($dlls | Where-Object Extension -eq '.dll').Count) { Say "no rex*.dll in $DllSource"; $rc = 6 }
                    else {
                        foreach ($f in $dlls) { Copy-Item -LiteralPath $f.FullName -Destination $bdir -Force }
                        Say "SDK DLLs from $DllSource ($(@($dlls).Count) files)"
                        $exeLine = "BISECT_EXE $bdir\wwe2k14.exe"
                    }
                }
            }
        }
    } else {
        if (-not $ExeSource) { $ExeSource = "$R\out\build\win-amd64-release" }
        $full = Set-Worktree $MainRepo $Worktree $Commit
        # Submodules: junctions to the main SDK checkout when the commit pins the same submodule commit.
        $subs = @(git -C $MainRepo ls-tree -r $full | Where-Object { $_ -match '^160000 commit ' })
        foreach ($s in $subs) {
            $parts = $s -split '\s+', 4
            $sha = $parts[2]; $path = $parts[3]
            if (-not (Test-Path -LiteralPath "$MainRepo\$($path -replace '/', '\')\.git")) {
                Say "submodule $path is not initialized in the main SDK tree either: left empty (the SDK build does not use it there)"
                continue
            }
            $mainSha = (git -C "$MainRepo\$path" rev-parse HEAD 2>$null)
            if (-not $mainSha -or $mainSha.Trim() -ne $sha) {
                throw "submodule $path at $($sha.Substring(0,9)) in $($full.Substring(0,9)), main SDK tree has $($mainSha): would need a fetch (not done)"
            }
            New-Junction "$Worktree\$($path -replace '/', '\')" "$MainRepo\$($path -replace '/', '\')"
        }
        $exeDir = "$R\out\harness\bisect\exe\sdk_$($full.Substring(0, 9))"
        if ($DryRun) { $exeLine = "BISECT_EXE (dry run) $exeDir\wwe2k14.exe" }
        else {
            . "$R\tools\env.ps1" *> $null
            $ErrorActionPreference = 'Continue'
            Set-Location $Worktree
            $bdir = "$Worktree\out\build\win-amd64"
            if (-not (Test-Path "$bdir\CMakeCache.txt")) {
                # Review finding 2: the main SDK build is the "win-amd64" preset (Ninja Multi-Config, clang) + REXGLUE_*
                # options; plain "cmake -S . -B" picked the VS generator / MSVC and the SDK stops ("requires Clang").
                $mainCache = "$SdkSrc\out\build\win-amd64\CMakeCache.txt"
                $opts = @(if (Test-Path -LiteralPath $mainCache) {
                    Get-Content -LiteralPath $mainCache | Where-Object { $_ -match '^REXGLUE_[A-Z0-9_]+:BOOL=' } | ForEach-Object { '-D' + $_ }
                })
                Say "configure SDK in $bdir (preset win-amd64, build_sdk.ps1 Release flags, main cache options: $($opts -join ' '))"
                cmake --preset win-amd64 @opts "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" `
                    "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" `
                    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" *>&1 | Out-File -Append -LiteralPath $Log
                if ($LASTEXITCODE) { Say "SDK configure failed (exit $LASTEXITCODE)"; $rc = 4 }
            }
            if (-not $rc -and $env:WT_BUILD_CONFIGURE_ONLY) { Say 'configure only (WT_BUILD_CONFIGURE_ONLY): no build'; $rc = 8 }
            if (-not $rc) {
                Say "SDK build $full in $bdir (no install)"
                cmake --build $bdir --config Release -j 16 *>&1 | Out-File -Append -LiteralPath $Log
                $code = $LASTEXITCODE
                Say ("SDK build exit $code, {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
                if ($code) {
                    Get-Content -LiteralPath $Log | Select-String 'error|FAILED' | Select-Object -First 10 | ForEach-Object { Say $_.Line }
                    $rc = 5
                } else {
                    $null = New-Item -ItemType Directory -Force -Path $exeDir
                    foreach ($n in 'wwe2k14.exe', 'wwe2k14.map') {
                        if (Test-Path "$ExeSource\$n") { Copy-Item -LiteralPath "$ExeSource\$n" -Destination $exeDir -Force }
                    }
                    foreach ($n in 'rexgpu-xenos', 'rexruntime') {
                        $dll = Get-ChildItem -LiteralPath "$Worktree\out" -Recurse -Filter "$n.dll" -File -ErrorAction SilentlyContinue |
                            Sort-Object LastWriteTime -Descending | Select-Object -First 1
                        if (-not $dll) { Say "no $n.dll under $Worktree\out"; $rc = 6; break }
                        Copy-Item -LiteralPath $dll.FullName -Destination $exeDir -Force
                        $pdb = [System.IO.Path]::ChangeExtension($dll.FullName, 'pdb')
                        if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $exeDir -Force }
                        Say "staged $($dll.FullName) -> $exeDir (unpinned: bisect only)"
                    }
                    if (-not $rc) { $exeLine = "BISECT_EXE $exeDir\wwe2k14.exe" }
                }
            }
        }
    }
} catch {
    Say "wt_build failed: $($_.Exception.Message)"
    $rc = 3
} finally {
    # The junctions exist only during the build (finding 1): remove them whatever happened.
    if (-not $DryRun -and (Test-Path -LiteralPath $Worktree)) {
        try { $n = Remove-Links $Worktree; if ($n) { Say "build done: $n junction(s) removed from $Worktree" } }
        catch { Say "WARNING: $($_.Exception.Message). Do NOT delete this worktree with git; use wt_build.ps1 -Clean."; if (-not $rc) { $rc = 3 } }
    }
}
if ($rc) { exit $rc }
$exeLine
exit 0
