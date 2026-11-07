# PGO / ThinLTO builds of the two SDK DLLs (rexgpu-xenos.dll + rexruntime.dll), Session 95 item E (read_E.md experiments 1 + 2).
#   & .\tools\release\pgo_build.ps1 -Stage gen                      # instrumented DLLs (training build)
#   & .\tools\release\pgo_build.ps1 -Stage use -ProfdataRuntime P1 -ProfdataGpu P2   # profile-guided DLLs
#   & .\tools\release\pgo_build.ps1 -Stage lto [-ProfdataRuntime P1 -ProfdataGpu P2] # ThinLTO (optionally + profile)
# Options: -Worktree W (default thirdparty\sdk-wt\s95-pgo; any SDK worktree), -Name N (default pgo_<stage>; the staged folder is
# out\harness\bisect\exe\live_<Name>\ with the STOCK main exe, exactly like tools\owned\fast_build.ps1), -Jobs N (default 8, Session 95),
# -LtoJobs N (ThinLTO backend threads, default 8), -ConfigureOnly (configure, print the per-target flags, build nothing), -NoCache
# (no sccache), -Reconfigure, -LockWaitMinutes N, -Log PATH.
#
# Each stage has its OWN build folder (<worktree>\out\build\win-amd64-<stage>) and output folder (<worktree>\out\<stage>), so the three
# variants never overwrite each other and the dev build of the same worktree (fast_build: out\build\win-amd64) is untouched. The main SDK
# build (thirdparty\rexglue-sdk-src, $env:REXSDK\bin) is never touched. Takes the BUILD class of the run lock (like fast_build).
# Only the two DLLs are built; the game exe is NOT rebuilt (the stock exe is copied in).
#
# What the stages do:
#  gen : -fprofile-instr-generate (clang frontend instrumentation: the profile is keyed by function name + source-structure hash, it
#        survives edits elsewhere in the file), -DREX_PGO_GEN=1 and -DREXGLUE_PGO_GEN=ON. The SDK patch (src\system\pgo_write.cpp,
#        src\graphics\vulkan\native\pgo_write.cpp, ReXApp::OnClosing) exports rex_pgo_write / rex_pgo_write_all (rexruntime) and
#        rexgpu_pgo_write (rexgpu-xenos) and writes both profiles BEFORE ReXApp::OnClosing's std::_Exit(0) (atexit never runs). No exe change.
#  use : -fprofile-instr-use=<runtime profdata> for everything folded into rexruntime (rexcore, rexui ... OBJECT libs), and the GPU
#        profdata for rexgpu-xenos only (target_compile_options after the global flags; the later flag wins). Missing or stale profile data
#        is only a warning (-Wno-profile-instr-unprofiled / -out-of-date are NOT silenced: the count of "out of date" warnings in the log
#        is the staleness measure, reported at the end).
#  lto : -flto=thin for compile and link, lld /opt:lldltojobs=<LtoJobs> and a lld LTO cache in <worktree>\out\lto_cache. The link time and
#        peak RAM of the whole build are measured (a sampler watches clang / lld-link processes) and printed.
# Profiles are generated artefacts (they come from running the game): keep them under out\pgo\ (gitignored by out\), never commit or ship.
# The DLLs are unpinned (tools\build.ps1 refuses them): live_<Name> is for checks only. A PGO / LTO release build needs its own pin
# (tools\build_sdk.ps1 -Pin) and is NOT wired into make_release.ps1 here.
# Exit codes: 0 ok, 2 bad arguments, 3 failure, 4 lock timeout, 5 build / configure failed, 6 DLLs missing, 7 another pgo_build on this
# worktree+stage, 9 a game runs from the staged folder. Last line on success: PGO_BUILD_EXE <path>.
param([Parameter(Mandatory)][ValidateSet('gen', 'use', 'lto')][string]$Stage,
      [string]$Worktree = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\s95-pgo', [string]$Name = '',
      [string]$ProfdataRuntime = '', [string]$ProfdataGpu = '', [int]$Jobs = 8, [int]$LtoJobs = 8,
      [switch]$ConfigureOnly, [switch]$NoCache, [switch]$Reconfigure, [double]$LockWaitMinutes = 60, [string]$Log = '')
$ErrorActionPreference = 'Continue'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$SdkSrc = "$R\thirdparty\rexglue-sdk-src"
if (-not $Name) { $Name = "pgo_$Stage" }
if ($Name -notmatch '^[A-Za-z0-9_-]{1,40}$') { "pgo_build: -Name must be letters, digits, - or _ (got '$Name')"; exit 2 }
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
if ($Worktree -ieq $SdkSrc) { 'pgo_build: refusing the main SDK checkout; use a worktree'; exit 2 }
if (-not (Test-Path -LiteralPath "$Worktree\.git" -PathType Leaf)) { "pgo_build: $Worktree is not a git worktree"; exit 2 }
if (-not (Test-Path -LiteralPath "$Worktree\src\system\pgo_write.cpp")) { "pgo_build: $Worktree lacks the PGO patch (src\system\pgo_write.cpp); base it on branch s95-pgo"; exit 2 }
if ($Stage -eq 'use') {
    foreach ($p in $ProfdataRuntime, $ProfdataGpu) {
        if (-not $p -and -not $ConfigureOnly) { 'pgo_build: -Stage use needs -ProfdataRuntime and -ProfdataGpu (llvm-profdata merge outputs under out\pgo\)'; exit 2 }
        if ($p -and -not (Test-Path -LiteralPath $p) -and -not $ConfigureOnly) { "pgo_build: profile data $p not found"; exit 2 }
    }
}
if ($ProfdataRuntime) { $ProfdataRuntime = [System.IO.Path]::GetFullPath($ProfdataRuntime) -replace '\\', '/' }
if ($ProfdataGpu) { $ProfdataGpu = [System.IO.Path]::GetFullPath($ProfdataGpu) -replace '\\', '/' }
if (-not $Log) { $Log = "$R\out\harness\bisect\pgo_build_${Name}_$Stage.log" }
$Log = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Log)
$null = New-Item -ItemType Directory -Force -Path (Split-Path $Log -Parent)
Set-Content -LiteralPath $Log -Value "pgo_build $Stage $Name $((Get-Date).ToString('s')) worktree $Worktree"
function Say([string]$m) { Write-Host $m; Add-Content -LiteralPath $Log -Value $m }

function Find-Links([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
    $di = [System.IO.DirectoryInfo]::new($Dir)
    if ($di.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $di.FullName; return }
    foreach ($e in $di.EnumerateFileSystemInfos()) {
        if ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $e.FullName }
        elseif ($e -is [System.IO.DirectoryInfo]) { Find-Links $e.FullName }
    }
}
function Remove-Links([string]$Dir) {
    $n = 0
    foreach ($l in @(Find-Links $Dir)) {
        $item = Get-Item -LiteralPath $l -Force
        if ($item -is [System.IO.DirectoryInfo]) { [System.IO.Directory]::Delete($l, $false); $null = New-Item -ItemType Directory -Path $l } else { [System.IO.File]::Delete($l) }
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

$null = New-Item -ItemType Directory -Force -Path "$Worktree\out"
$mutex = $null
try { $mutex = [System.IO.File]::Open("$Worktree\out\pgo_build_$Stage.lock", [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) }
catch { Say "pgo_build: another pgo_build $Stage is running on $Worktree. Not started."; exit 7 }

$t0 = Get-Date; $rc = 0; $lock = $null; $exeLine = ''; $tConf = 0; $tBuild = 0; $waited = 0; $sampler = $null; $sampleFile = ''
try {
    $full = (git -C $Worktree rev-parse HEAD).Trim()
    $branch = (git -C $Worktree rev-parse --abbrev-ref HEAD).Trim()
    $dirty = @(git -C $Worktree status --porcelain --untracked-files=no).Count
    $dirtyNote = if ($dirty) { "$dirty changed file(s) on top of" } else { 'clean at' }
    Say "worktree $Worktree ($branch): $dirtyNote $($full.Substring(0, 9)), stage $Stage"
    if (-not $ConfigureOnly) {
        . "$R\tools\runlock.ps1"
        $script:RunLockMsgToStderr = $false
        $tl = Get-Date
        try { $lock = Enter-RunLock -Name "pgo_build $Name $Stage" -Mode Build -TimeoutMinutes $LockWaitMinutes }
        catch { Say "pgo_build: $($_.Exception.Message)"; $rc = 4; throw }
        $waited = ((Get-Date) - $tl).TotalSeconds
    }
    $null = Remove-Links "$Worktree\thirdparty"
    $subs = @(git -C $SdkSrc ls-tree -r $full 2>$null | Where-Object { $_ -match '^160000 commit ' })
    if (-not $subs.Count) { $subs = @(git -C $Worktree ls-tree -r HEAD | Where-Object { $_ -match '^160000 commit ' }) }
    foreach ($s in $subs) {
        $parts = $s -split '\s+', 4
        $sha = $parts[2]; $path = $parts[3] -replace '/', '\'
        if (-not (Test-Path -LiteralPath "$SdkSrc\$path\.git")) { continue }
        $mainSha = (git -C "$SdkSrc\$path" rev-parse HEAD 2>$null)
        if (-not $mainSha -or $mainSha.Trim() -ne $sha) { throw "submodule $path pinned at $($sha.Substring(0, 9)) here, main SDK tree has ${mainSha}: would need a fetch (not done)" }
        if ((Test-Path -LiteralPath "$Worktree\$path") -and @(Get-ChildItem -LiteralPath "$Worktree\$path" -Force).Count -gt 0 -and
            -not ((Get-Item -LiteralPath "$Worktree\$path" -Force).LinkType)) { continue }
        New-Junction "$Worktree\$path" "$SdkSrc\$path"
    }

    . "$R\tools\env.ps1" *> $null
    $ErrorActionPreference = 'Continue'
    Set-Location $Worktree
    $bdir = "$Worktree\out\build\win-amd64-$Stage"
    $odir = "$Worktree\out\$Stage"
    $sccache = ''
    if (-not $NoCache) {
        $c = Get-Command sccache -ErrorAction SilentlyContinue
        $sccache = if ($c) { $c.Source } elseif (Test-Path "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe") { "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe" } else { '' }
    }
    $launcherOpts = @()
    if ($sccache) {
        $env:SCCACHE_BASEDIRS = $Worktree
        $h = 0; foreach ($ch in $Worktree.ToLowerInvariant().ToCharArray()) { $h = ($h * 31 + [int]$ch) % 1000003 }
        $env:SCCACHE_SERVER_PORT = [string](4300 + ($h % 600))
        $env:SCCACHE_DIRECT = 'false'  # Session 95: fast_build.ps1 says why (a direct hit took another branch's object)
        & $sccache --stop-server *> $null
        if (-not $env:SCCACHE_CACHE_SIZE) { $env:SCCACHE_CACHE_SIZE = '40G' }
        $launcherOpts = @("-DCMAKE_C_COMPILER_LAUNCHER=$sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache")
        Say "sccache: on (separate cache keys per stage: the flags differ)"
    }

    # flags per stage (the dev flags of fast_build.ps1 plus the stage's own)
    $base = '-O3 -DNDEBUG -g -gcodeview'
    $lnk = '-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF'
    $extraCxx = ''; $extraLnk = ''; $extraCmake = @()
    switch ($Stage) {
        'gen' {
            $extraCxx = '-fprofile-instr-generate -DREX_PGO_GEN=1'
            $extraLnk = '-fprofile-instr-generate -Wl,/force:unresolved'
            # CMake's WINDOWS_EXPORT_ALL_SYMBOLS def file lists the profile counters (__profc_..., it reads the COFF objects itself, --nm is not used) and
            # they are not defined at link: /force:unresolved turns that into warnings; the build below fails if any warning names anything else.
            $extraCmake = @('-DREXGLUE_PGO_GEN=ON')
        }
        'use' {
            if ($ProfdataRuntime) { $extraCxx = "-fprofile-instr-use=$ProfdataRuntime" }
            if ($ProfdataGpu) { $extraCmake = @("-DREXGLUE_PGO_USE_GPU=$ProfdataGpu") }
        }
        'lto' {
            $null = New-Item -ItemType Directory -Force -Path "$Worktree\out\lto_cache"
            $cacheDir = ("$Worktree\out\lto_cache" -replace '\\', '/')
            $extraCxx = '-flto=thin' + $(if ($ProfdataRuntime) { " -fprofile-instr-use=$ProfdataRuntime" } else { '' })
            $extraLnk = "-flto=thin -Wl,/opt:lldltojobs=$LtoJobs -Wl,/lldltocache:$cacheDir"
            if ($ProfdataGpu) { $extraCmake = @("-DREXGLUE_PGO_USE_GPU=$ProfdataGpu") }
        }
    }
    if ($Reconfigure -or -not (Test-Path "$bdir\CMakeCache.txt")) {
        $mainCache = "$SdkSrc\out\build\win-amd64\CMakeCache.txt"
        $opts = @(if (Test-Path -LiteralPath $mainCache) {
            Get-Content -LiteralPath $mainCache | Where-Object { $_ -match '^REXGLUE_[A-Z0-9_]+:BOOL=' } | ForEach-Object { '-D' + $_ }
        })
        Say "configure (preset win-amd64, build folder $bdir, output $odir): CXX flags '$extraCxx', link flags '$extraLnk', $($extraCmake -join ' ')"
        $tc = Get-Date
        cmake --preset win-amd64 -B $bdir @opts @launcherOpts @extraCmake "-DREXGLUE_OUTPUT_DIR=$($odir -replace '\\', '/')" `
            "-DCMAKE_CXX_FLAGS_RELEASE=$base $extraCxx" "-DCMAKE_C_FLAGS_RELEASE=$base $extraCxx" `
            "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=$lnk $extraLnk" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=$lnk $extraLnk" *>&1 | Out-File -Append -LiteralPath $Log
        $tConf = ((Get-Date) - $tc).TotalSeconds
        if ($LASTEXITCODE) { Say "configure failed (exit $LASTEXITCODE), see $Log"; $rc = 5; throw 'configure failed' }
    }
    if ($ConfigureOnly) {
        Say "configure only. Compile command of one rexgpu-xenos and one rexruntime object (flags check):"
        $impl = Get-Content -LiteralPath "$bdir\CMakeFiles\impl-Release.ninja"
        foreach ($o in 'owned_scene.cpp.obj', 'kernel_state.cpp.obj') {
            $fl = '(object not found)'
            for ($i = 0; $i -lt $impl.Count; $i++) {
                if ($impl[$i] -notmatch ('^build .*[\/]' + [regex]::Escape($o) + ':')) { continue }
                $cand = ($impl[$i..([Math]::Min($i + 10, $impl.Count - 1))] | Where-Object { $_ -match '^\s+FLAGS =' } | Select-Object -First 1)
                if ($cand -match ' -O3 ') { $fl = $cand; break }
            }
            $fl = ($fl -replace '-Xclang -include-pch.*$', '' -replace '\s+', ' ')
            Say "  [$o] $fl"
        }
        throw 'configure only'
    }

    # sampler: peak RAM of the compiler / linker processes, and when lld-link ran (the link = the long pole of LTO)
    $sampleFile = "$bdir\pgo_sampler.txt"
    Remove-Item -LiteralPath $sampleFile -Force -ErrorAction SilentlyContinue
    $sampler = Start-Job -ArgumentList $sampleFile -ScriptBlock {
        param($f)
        $peak = 0; $minFree = [long]::MaxValue; $linkFirst = $null; $linkLast = $null; $peakLink = 0
        while ($true) {
            $ps = @(Get-Process -Name clang, 'clang++', clang-cl, lld-link, ld.lld -ErrorAction SilentlyContinue)
            $sum = ($ps | Measure-Object -Property WorkingSet64 -Sum).Sum
            if ($sum -gt $peak) { $peak = $sum }
            $lk = @($ps | Where-Object { $_.ProcessName -match 'lld' })
            if ($lk.Count) {
                if (-not $linkFirst) { $linkFirst = Get-Date }
                $linkLast = Get-Date
                $ls = ($lk | Measure-Object -Property WorkingSet64 -Sum).Sum
                if ($ls -gt $peakLink) { $peakLink = $ls }
            }
            $free = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB
            if ($free -lt $minFree) { $minFree = $free }
            "peak_all_mb=$([int]($peak / 1MB)) peak_link_mb=$([int]($peakLink / 1MB)) min_free_gb=$([math]::Round($minFree / 1GB, 1)) link_first=$(if ($linkFirst) { $linkFirst.ToString('s') }) link_last=$(if ($linkLast) { $linkLast.ToString('s') })" | Set-Content -LiteralPath $f
            Start-Sleep -Milliseconds 1000
        }
    }
    Say "build rexgpu-xenos rexruntime (Release, -j $Jobs) in $bdir"
    $tb = Get-Date
    cmake --build $bdir --config Release --target rexgpu-xenos rexruntime -j $Jobs *>&1 | Out-File -Append -LiteralPath $Log
    $code = $LASTEXITCODE
    $tBuild = ((Get-Date) - $tb).TotalSeconds
    if ($code) {
        Say "build FAILED (exit $code) after $([int]$tBuild) s:"
        Get-Content -LiteralPath $Log | Select-String ': error|FAILED|error:|undefined symbol' | Select-Object -First 12 | ForEach-Object { Say ('  ' + $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length))) }
        $rc = 5; throw 'build failed'
    }
    Say ("build ok, {0:N1} s" -f $tBuild)
    if ($Stage -eq 'gen') {
        # /force:unresolved is only allowed to excuse profile-runtime symbols (__profc_ / __profd_ / __profn_ ... in the def file)
        $bad = @(Select-String -LiteralPath $Log -Pattern 'undefined symbol' | Where-Object { $_.Line -notmatch '__prof' })
        if ($bad.Count) { Say "gen build: $($bad.Count) undefined symbol warning(s) that are NOT profile symbols (a real missing symbol):"; $bad | Select-Object -First 8 | ForEach-Object { Say ('  ' + $_.Line.Trim()) }; $rc = 5; throw 'real undefined symbols' }
        $prof = @(Select-String -LiteralPath $Log -Pattern 'undefined symbol: __prof').Count
        Say "gen build: $prof undefined profile-counter symbol(s) from the export-all def file excused by /force:unresolved (expected)"
    }
    if (Test-Path $sampleFile) { Start-Sleep -Milliseconds 1200; Say ("resources: " + (Get-Content -LiteralPath $sampleFile -Raw).Trim() + "  (peak_link = lld-link working set, link_first..link_last = when it ran)") }
    if ($Stage -eq 'use') {
        $ood = @(Select-String -LiteralPath $Log -Pattern 'profile data may be out of date|profile-instr-out-of-date').Count
        $mal = @(Select-String -LiteralPath $Log -Pattern 'malformed|Could not read profile|unsupported instrumentation').Count
        $unp = @(Select-String -LiteralPath $Log -Pattern 'no profile data available|profile-instr-unprofiled').Count
        Say "profile use: out-of-date warnings $ood, no-data warnings $unp, malformed / unreadable $mal (log $Log)"
    }

    # stage into live_<Name> (same layout and rules as fast_build.ps1)
    $exeDir = "$R\out\harness\bisect\exe\live_$Name"
    $null = New-Item -ItemType Directory -Force -Path $exeDir
    $inUse = @(Get-Process -Name wwe2k14 -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and $_.Path -like "$exeDir\*" } catch { $false } })
    if ($inUse.Count) { Say "a game is running from ${exeDir}: DLLs built, not staged (exit 9)"; $rc = 9; throw 'exe in use' }
    $ExeSource = "$R\out\build\win-amd64-release"
    foreach ($n in 'wwe2k14.exe', 'wwe2k14.map', 'amd_fidelityfx_vk.dll', 'shader_list.txt') {
        $s = "$ExeSource\$n"; $d = "$exeDir\$n"
        if (-not (Test-Path -LiteralPath $s)) { continue }
        $differs = -not (Test-Path -LiteralPath $d)
        if (-not $differs) { $si = Get-Item -LiteralPath $s; $di = Get-Item -LiteralPath $d; $differs = ($si.Length -ne $di.Length) -or ($si.LastWriteTimeUtc -ne $di.LastWriteTimeUtc) }
        if ($differs) { Copy-Item -LiteralPath $s -Destination $d -Force -ErrorAction Stop; Say "staged $n from $ExeSource" }
    }
    $outDir = "$odir\Release"
    if (-not (Test-Path "$outDir\rexruntime.dll")) { $outDir = $odir }
    foreach ($n in 'rexgpu-xenos', 'rexruntime') {
        foreach ($ext in 'dll', 'pdb') {
            $s = "$outDir\$n.$ext"
            if (-not (Test-Path -LiteralPath $s)) { if ($ext -eq 'dll') { Say "no $n.dll in $outDir"; $rc = 6; throw "missing $n.dll" }; continue }
            Copy-Item -LiteralPath $s -Destination "$exeDir\$n.$ext" -Force -ErrorAction Stop
        }
    }
    foreach ($sub in 'logs', 'captures') { $null = New-Item -ItemType Directory -Force -Path "$exeDir\$sub" }
    $sizes = ('rexgpu-xenos', 'rexruntime' | ForEach-Object { "$_.dll {0:N0} B" -f (Get-Item "$exeDir\$_.dll").Length }) -join ', '
    Set-Content -LiteralPath "$exeDir\build_info.txt" -Value @("builder: $Name (pgo_build $Stage)", "worktree: $Worktree", "branch: $branch", "commit: $full ($dirtyNote)", "built: $((Get-Date).ToString('s'))", "flags: cxx '$extraCxx' link '$extraLnk'", $sizes)
    Say "staged into $exeDir ($sizes)"
    if ($Stage -eq 'gen') {
        $clean = (& dumpbin /nologo /exports "$exeDir\rexruntime.dll" 2>$null | Select-String 'rex_pgo_write'); $gpu = (& dumpbin /nologo /exports "$exeDir\rexgpu-xenos.dll" 2>$null | Select-String 'rexgpu_pgo_write')
        Say "exports: rexruntime: $(@($clean).Count) match(es) [$((@($clean) | ForEach-Object { $_.Line.Trim() }) -join ' | ')]; rexgpu-xenos: $(@($gpu).Count) match(es)"
        Say "TRAIN: set LLVM_PROFILE_FILE=$R\out\pgo\raw\%m_%p.profraw before the run, then the run command in tools\release\pgo_train.txt"
    }
    $exeLine = "PGO_BUILD_EXE $exeDir\wwe2k14.exe"
} catch {
    if (-not $rc -and "$($_.Exception.Message)" -ne 'configure only') { Say "pgo_build failed: $($_.Exception.Message)"; $rc = 3 }
} finally {
    if ($sampler) { Stop-Job $sampler -ErrorAction SilentlyContinue; Remove-Job $sampler -Force -ErrorAction SilentlyContinue }
    Set-Location $R
    if (Test-Path -LiteralPath $Worktree) {
        try { $n = Remove-Links "$Worktree\thirdparty"; if ($n) { Say "$n junction(s) removed from $Worktree" } }
        catch { Say "WARNING: $($_.Exception.Message). Do NOT delete this worktree with git; use tools\bisect\wt_build.ps1 -Clean."; if (-not $rc) { $rc = 3 } }
    }
    if ($lock) { Exit-RunLock $lock }
    if ($mutex) { $mutex.Dispose() }
}
$tot = ((Get-Date) - $t0).TotalSeconds
Say ("pgo_build {0} {1}: exit {2}; total {3:N1} s (lock wait {4:N1} s, configure {5:N1} s, build {6:N1} s)" -f $Name, $Stage, $rc, $tot, $waited, $tConf, $tBuild)
if ($rc) { exit $rc }
if ($ConfigureOnly) { exit 0 }
$exeLine
exit 0
