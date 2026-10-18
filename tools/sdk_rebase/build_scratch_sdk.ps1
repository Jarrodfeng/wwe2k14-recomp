# Build + install a rebased ReXGlue SDK worktree into a SCRATCH folder (Session 8, task 17; driver: sdk_rebase.py).
# Never writes the real SDK install (REXSDK, thirdparty\rexglue-sdk-vulkan), the main SDK tree's build folder, or the
# game's exe folder. Normally started by sdk_rebase.py under the run lock:
#   pwsh -NoProfile -File tools\runlock.ps1 run -Name "sdk_rebase build X" -- pwsh -NoProfile -File tools\sdk_rebase\build_scratch_sdk.ps1 -Worktree W -Log L
#   pwsh -NoProfile -File tools\sdk_rebase\build_scratch_sdk.ps1 -Clean -Worktree W     the safe way to delete W
#     (-Clean also removes any CMake user package registry value pointing inside W; runs when W is already gone)
# The SDK's install step registers the scratch prefix in HKCU\Software\Kitware\CMake\Packages\rexglue; this script
# removes that value again right after the install (line SDKREBASE_REGISTRY_REMOVED n), --keep or not.
#
# Worktree: must be thirdparty\sdk-wt\rebase-<stamp> (refused otherwise). Build folder <W>\out\build\win-amd64 (the
# SDK's own "win-amd64" preset: Ninja Multi-Config, clang), install prefix <W>\out\install\win-amd64 (set explicitly
# and checked in CMakeCache before anything is installed), CMake options = every REXGLUE_* option of the main SDK build
# cache + tools\build_sdk.ps1's Release flags (same as tools\bisect\wt_build.ps1 -Repo sdk).
#
# Submodules: junctions to the main SDK tree's checkouts, only when the worktree's commit records the same submodule
# commit the main tree has checked out (nothing is fetched; a different pin = exit 9, "SDKREBASE_SUBMODULE_MISMATCH").
# JUNCTIONS: "git worktree remove --force" FOLLOWS a junction and deletes the target's contents (tools\bisect\wt_build.ps1
# header, reproduced with Git 2.52.0.windows.1). So the junctions exist only during the build (removed in the finally
# block, and before the build in case an earlier one was interrupted), and -Clean removes every reparse point as a link
# only, re-checks that none is left, then deletes the folder and runs "git worktree prune". Never "git worktree remove".
#
# Output lines for the driver: SDKREBASE_CONFIGURE_EXIT n, SDKREBASE_BUILD_EXIT n, SDKREBASE_INSTALL <dir>,
# SDKREBASE_ELAPSED s, SDKREBASE_RESULT ok|fail <why>. Exit: 0 ok, 2 bad args, 3 junction/cleanup problem,
# 4 configure failed, 5 build/install failed, 6 install prefix not scratch, 9 submodule pin differs.
param([string]$Worktree = '', [string]$Log = '', [switch]$Clean, [switch]$ConfigureOnly)
$ErrorActionPreference = 'Continue'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$SdkSrc = "$R\thirdparty\rexglue-sdk-src"
$WtRoot = "$R\thirdparty\sdk-wt"
if (-not $Worktree) { 'build_scratch_sdk: -Worktree is required'; exit 2 }
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
$leaf = Split-Path $Worktree -Leaf
if ((Split-Path $Worktree -Parent) -ine $WtRoot -or $leaf -notmatch '^rebase-[A-Za-z0-9_.-]+$') {
    "build_scratch_sdk: refusing $Worktree (only $WtRoot\rebase-<stamp> worktrees)"; exit 2
}
if (-not $Log) { $Log = "$R\out\harness\sdk_rebase\$leaf.$(if ($Clean) { 'clean' } else { 'build' }).log" }
$Log = [System.IO.Path]::GetFullPath($Log)
$null = New-Item -ItemType Directory -Force -Path (Split-Path $Log -Parent)
function Say([string]$m) { Write-Host $m; Add-Content -LiteralPath $Log -Value $m }

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

# CMake user package registry: the SDK's install step (thirdparty\rexglue-sdk-src\cmake\rexglue_install.cmake,
# install(CODE) "reg add") registers EVERY install prefix under HKCU\Software\Kitware\CMake\Packages\rexglue (value name
# = md5 of the lowercased prefix). A scratch prefix left there lets a game configure that does not set rexglue_DIR
# resolve rexglue to the scratch SDK. Removes only values whose path lies inside $Dir; the real SDK's value is untouched.
$PkgReg = 'HKCU:\Software\Kitware\CMake\Packages\rexglue'
function Remove-ScratchRegistry([string]$Dir) {
    $n = 0
    if (-not (Test-Path -LiteralPath $PkgReg)) { return 0 }
    $key = Get-Item -LiteralPath $PkgReg
    foreach ($name in @($key.GetValueNames())) {
        $data = [string]$key.GetValue($name)
        $p = ($data -replace '/', '\').TrimEnd('\')
        if ($p -ieq $Dir -or $p.StartsWith("$Dir\", [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-ItemProperty -LiteralPath $PkgReg -Name $name -ErrorAction Stop
            Say "removed CMake package registry value $name -> $data"
            $n++
        }
    }
    $key = Get-Item -LiteralPath $PkgReg
    $left = @($key.GetValueNames() | Where-Object {
        $p = ([string]$key.GetValue($_) -replace '/', '\').TrimEnd('\')
        $p -ieq $Dir -or $p.StartsWith("$Dir\", [System.StringComparison]::OrdinalIgnoreCase) })
    if ($left.Count) { throw "CMake package registry values still point inside ${Dir}: $($left -join ', ')" }
    return $n
}

if ($Clean) {
    try {
        $null = Remove-ScratchRegistry $Worktree
        if (-not (Test-Path -LiteralPath $Worktree)) { Say "no worktree at $Worktree" }
        else {
            if ($Worktree -ieq $SdkSrc) { throw "refusing to delete the main SDK tree" }
            if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { throw "$Worktree has no .git file: not a git worktree, not deleted" }
            $n = Remove-Links $Worktree
            Say "removed $n link(s); none left under $Worktree"
            Remove-Item -LiteralPath $Worktree -Recurse -Force
            if (Test-Path -LiteralPath $Worktree) { throw "could not delete $Worktree" }
            Say "deleted $Worktree"
        }
        git -C $SdkSrc worktree prune 2>&1 | ForEach-Object { Say "  $_" }
        $still = @(git -C $SdkSrc worktree list --porcelain | Where-Object { $_ -match '^worktree ' -and ($_.Substring(9) -replace '/', '\') -ieq $Worktree })
        if ($still.Count) { throw "git still lists worktree $Worktree after prune" }
        Say "SDKREBASE_CLEAN ok $Worktree"
        exit 0
    } catch { Say "SDKREBASE_CLEAN fail $($_.Exception.Message)"; exit 3 }
}

function New-Junction([string]$Link, [string]$Target) {
    if (Test-Path -LiteralPath $Link) {
        $item = Get-Item -LiteralPath $Link -Force
        if ($item.LinkType -eq 'Junction' -and ($item.Target -replace '\\$', '') -ieq ($Target -replace '\\$', '')) { return }
        if ($item.LinkType -ne 'Junction' -and @(Get-ChildItem -LiteralPath $Link -Force).Count -eq 0) {
            Remove-Item -LiteralPath $Link -Force   # the empty submodule folder from the checkout
        } else { throw "$Link exists and is not a junction to $Target" }
    }
    Say "junction $Link -> $Target"
    $null = New-Item -ItemType Junction -Path $Link -Target $Target
}

if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { "build_scratch_sdk: $Worktree is not a git worktree"; exit 2 }
$t0 = Get-Date
$rc = 0
$why = ''
try {
    $null = Remove-Links $Worktree   # leftovers of an interrupted build
    $head = (git -C $Worktree rev-parse HEAD).Trim()
    Say "worktree $Worktree at $head"
    foreach ($s in @(git -C $Worktree ls-tree -r HEAD | Where-Object { $_ -match '^160000 commit ' })) {
        $parts = $s -split '\s+', 4
        $sha = $parts[2]; $path = $parts[3]
        $mainPath = "$SdkSrc\$($path -replace '/', '\')"
        if (-not (Test-Path -LiteralPath "$mainPath\.git")) {
            Say "submodule $path is not initialized in the main SDK tree either: left empty (the main SDK build does not use it)"
            continue
        }
        $mainSha = (git -C $mainPath rev-parse HEAD 2>$null)
        if (-not $mainSha -or $mainSha.Trim() -ne $sha) {
            Say "SDKREBASE_SUBMODULE_MISMATCH $path rebased=$sha main=$mainSha (would need a fetch: not done)"
            $rc = 9; $why = "submodule $path pinned to another commit"
            continue
        }
        New-Junction "$Worktree\$($path -replace '/', '\')" $mainPath
    }
    if (-not $rc) {
        . "$R\tools\env.ps1" *> $null
        $ErrorActionPreference = 'Continue'
        Set-Location $Worktree
        $bdir = "$Worktree\out\build\win-amd64"
        $inst = "$Worktree\out\install\win-amd64"
        $mainCache = "$SdkSrc\out\build\win-amd64\CMakeCache.txt"
        $opts = @(if (Test-Path -LiteralPath $mainCache) {
            Get-Content -LiteralPath $mainCache | Where-Object { $_ -match '^REXGLUE_[A-Z0-9_]+:BOOL=' } | ForEach-Object { '-D' + $_ }
        })
        Say "configure: cmake --preset win-amd64 $($opts -join ' ') (build_sdk.ps1 Release flags) -DCMAKE_INSTALL_PREFIX=$inst"
        cmake --preset win-amd64 @opts "-DCMAKE_INSTALL_PREFIX=$inst" `
            "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" `
            "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" `
            "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" *>&1 | Out-File -Append -LiteralPath $Log
        $c = $LASTEXITCODE
        Say "SDKREBASE_CONFIGURE_EXIT $c"
        if ($c) { $rc = 4; $why = "configure exit $c" }
        else {
            # The install prefix must be the scratch one before anything is installed.
            $pfx = (Get-Content -LiteralPath "$bdir\CMakeCache.txt" | Where-Object { $_ -match '^CMAKE_INSTALL_PREFIX:' } | Select-Object -First 1)
            $pfxPath = if ($pfx) { [System.IO.Path]::GetFullPath(($pfx -split '=', 2)[1]).TrimEnd('\') } else { '' }
            if (-not $pfxPath -or -not $pfxPath.StartsWith("$Worktree\", [System.StringComparison]::OrdinalIgnoreCase)) {
                Say "install prefix '$pfxPath' is not inside ${Worktree}: not building"
                $rc = 6; $why = 'install prefix not scratch'
            } elseif ($ConfigureOnly) { Say 'configure only: no build' }
            else {
                Say "build + install (cmake --build $bdir --config Release --target install -j 16)"
                cmake --build $bdir --config Release --target install -j 16 *>&1 | Out-File -Append -LiteralPath $Log
                $c = $LASTEXITCODE
                Say "SDKREBASE_BUILD_EXIT $c"
                if ($c) {
                    Get-Content -LiteralPath $Log | Select-String ': error|FAILED|error:' | Select-Object -First 30 | ForEach-Object { Say ('  ' + $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length))) }
                    $rc = 5; $why = "build exit $c"
                } else { Say "SDKREBASE_INSTALL $pfxPath" }
            }
        }
    }
} catch {
    Say "build_scratch_sdk failed: $($_.Exception.Message)"
    $rc = 3; $why = $_.Exception.Message
} finally {
    try { $n = Remove-Links $Worktree; if ($n) { Say "build done: $n junction(s) removed from $Worktree" } }
    catch { Say "WARNING: $($_.Exception.Message). Do NOT delete this worktree with git; use build_scratch_sdk.ps1 -Clean."; if (-not $rc) { $rc = 3; $why = 'junction removal failed' } }
    # Right after the install (also with sdk_rebase.py --keep): no registry value may point at the scratch install.
    try { $n = Remove-ScratchRegistry $Worktree; Say "SDKREBASE_REGISTRY_REMOVED $n" }
    catch { Say "WARNING: $($_.Exception.Message)"; if (-not $rc) { $rc = 3; $why = 'package registry cleanup failed' } }
}
Say ("SDKREBASE_ELAPSED {0:N0}" -f ((Get-Date) - $t0).TotalSeconds)
if ($rc) { Say "SDKREBASE_RESULT fail $why"; exit $rc }
Say 'SDKREBASE_RESULT ok'
exit 0
