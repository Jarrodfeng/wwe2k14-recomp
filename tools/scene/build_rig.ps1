# Builds the offline rig's drawing half (Session 103, lane R): out\tools\frame_rig.exe = one scene record drawn from the record alone through the shared record -> frame
# library (src\renderer_scene\record_frame.h) and the scene front-end, with GameFilesSource (live_assets.cpp) as the asset source (the same loaders the live game uses).
# Compile only: no game, no run lock, no SDK. frame_rig.py runs it and makes the sheets.
#   pwsh -NoProfile -File tools\scene\build_rig.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
# QTF (S103, WTA fix 6, review C1): a hard timeout for the whole build, run by a watcher (tools\scene\build_watchdog.ps1) that holds a handle on THIS
# process, so a reused pid can never be hit, and on expiry kills only the compile descendants (clang++ / lld / ninja) and this script's own process.
# It only starts when this script runs as its own process (pwsh -File). Run in-process (`& build_rig.ps1` from a shell) the host shell would be the
# target, so the body is re-run in a child `pwsh -File` instead.
$isFile = $false   # run as its OWN process iff the -File argument of this process's command line is THIS script (not just any -File in a host's line)
if ([Environment]::CommandLine -match '(?i)\s-File\s+("([^"]+)"|(\S+))') {
  $fa = if ($Matches[2]) { $Matches[2] } else { $Matches[3] }
  try { $isFile = ((Resolve-Path -LiteralPath $fa -ErrorAction Stop).Path -eq $PSCommandPath) } catch { $isFile = $false }
}
if (-not $isFile) {
  $fwd = @(); if ($Clean) { $fwd += '-Clean' }
  & pwsh -NoProfile -File $PSCommandPath @fwd
  if ($LASTEXITCODE -ne 0) { throw "build_rig.ps1 failed in its child pwsh (exit $LASTEXITCODE)" }
  return
}
$wdMin = if ($env:WWE_BUILD_TIMEOUT_MIN) { [int]$env:WWE_BUILD_TIMEOUT_MIN } else { 40 }
$me = [Diagnostics.Process]::GetCurrentProcess()
$null = Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'build_watchdog.ps1'), '-TargetPid', $me.Id, '-StartTicks', $me.StartTime.Ticks, '-Secs', ($wdMin * 60)
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$env:WWE_SWEEP_OWNER = '{0}:{1}' -f $me.Id, [DateTimeOffset]::new($me.StartTime).ToUnixTimeSeconds()   # QTF: AFTER the cache setup, so the sccache server never inherits it; opt-in ownership for tools\harness\proc_sweep.py: my compile children are sweepable orphans if I die
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rig_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Split-Path $stb -Parent))   # S103 D1: + the SDK's thirdparty folder (<xxHash/xxhash.h> for live_assets.cpp)
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$rs = Join-Path $repo 'src\renderer_scene'
$srcs = @((Join-Path $PSScriptRoot 'frame_rig.cpp'),
          (Join-Path $rs 'record_io.cpp'), (Join-Path $rs 'front_end.cpp'), (Join-Path $rs 'char_effects.cpp'), (Join-Path $rs 'shader_reload.cpp'), (Join-Path $rs 'post_pass.cpp'), (Join-Path $rs 'post_from_record.cpp'),
          (Join-Path $rs 'record_frame.cpp'), (Join-Path $rs 'record_frame_classes.cpp'), (Join-Path $rs 'rf_arena.cpp'), (Join-Path $rs 'rf_crowd.cpp'), (Join-Path $rs 'live_crowd.cpp'), (Join-Path $rs 'rf_models.cpp'),
          (Join-Path $repo 'src\renderer_scene\rf_wrestlers.cpp'), (Join-Path $repo 'src\renderer_scene\rf_c_core.cpp'), (Join-Path $repo 'src\renderer_scene\rf_arena_live.cpp'), (Join-Path $repo 'src\renderer_scene\rf_ring.cpp'), (Join-Path $repo 'src\renderer_scene\rf_props.cpp'), (Join-Path $repo 'src\renderer_scene\rf_screens.cpp'), (Join-Path $repo 'src\renderer_scene\live_flashes.cpp'), (Join-Path $repo 'src\renderer_scene\draw_lists.cpp'),  # S103 S: the class table names lane B's and lane C's appliers
          (Join-Path $rs 'live_particles.cpp'),  # S103 F: the particle probe
          (Join-Path $rs 'live_maps.cpp'), (Join-Path $rs 'live_map_shadow.cpp'), (Join-Path $rs 'live_map_circles.cpp'), (Join-Path $rs 'live_map_props.cpp'), (Join-Path $rs 'live_map_e2_input.cpp'), (Join-Path $rs 'live_map_rim.cpp'), (Join-Path $rs 'live_map_chain.cpp'), (Join-Path $rs 'live_map_plane.cpp'), (Join-Path $rs 'live_map_rope.cpp'), (Join-Path $rs 'live_map_rope_input.cpp'), (Join-Path $repo 'src\assets\ring_game_space.cpp'), (Join-Path $rs 'live_map_prelight.cpp'), (Join-Path $rs 'live_map_sweat.cpp'), (Join-Path $rs 'live_particles.cpp'), (Join-Path $rs 'frame_hooks.cpp'),  # S103 R2: lane E's maps hook and lane F's particle hook, attached as the live frame attaches them
          (Join-Path $rs 'live_assets.cpp'),  # carries its own static stb_image
          (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $repo 'src\assets\view\mods_win32.cpp'), (Join-Path $repo 'src\assets\view\loader_seam_game.cpp'), (Join-Path $repo 'src\assets\crowd_body.cpp'), (Join-Path $repo 'src\assets\crowd_prop_game_space.cpp'))
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
$newestHdr = (Get-ChildItem (Join-Path $repo 'src'), (Join-Path $repo 'tools/scene') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$incRel = ConvertTo-CxxRel $inc   # S103 H1: repo-relative paths (the same text in every worktree)
. (Join-Path $PSScriptRoot '..\admit\Admit.ps1'); try { $fails = $srcs | ForEach-Object -ThrottleLimit (Request-BuildParallel -Want ([int]($(if ($env:WWE_RIG_JOBS) { $env:WWE_RIG_JOBS } else { 16 })))) -Parallel {   # CQW: the compile queue (tools\admit\Admit.ps1). S103 UPL: WWE_RIG_JOBS lowers the parallelism when other lanes hold the RAM (a clang++ "out of memory" crash looks like a compile error)
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & $using:cxx0 @using:cxxRest -O1 -g -std=c++20 -c @using:defs -Wno-unused-command-line-argument @using:incRel ($_.Substring($using:repoLen)) -o ($o.Substring($using:repoLen)) 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
} } finally { Release-BuildParallel }
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
Show-CxxCache
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'frame_rig.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'frame_rig link failed' }
Write-Host "built $(Join-Path $outDir 'frame_rig.exe')"

# the ModelCache miss-queue unit check
$test = Join-Path $obj 'tools_scene_record_frame_test.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\record_frame_test.cpp' -o ($test.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'record_frame_test compile failed' }
$tobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $test
& clang++ -fuse-ld=lld -g @tobjs -o (Join-Path $outDir 'record_frame_test.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'record_frame_test link failed' }
Write-Host "built $(Join-Path $outDir 'record_frame_test.exe')"

# S103 NR: the not-ready rule's unit (tools\scene\nr_unit.cpp): the same objects, its own main
$nr = Join-Path $obj 'tools_scene_nr_unit.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\nr_unit.cpp' -o ($nr.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'nr_unit compile failed' }
$nobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $nr
& clang++ -fuse-ld=lld -g @nobjs -o (Join-Path $outDir 'nr_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'nr_unit link failed' }
Write-Host "built $(Join-Path $outDir 'nr_unit.exe')"

# S103 EYF: the piece-alpha proof through BuildFrame + the front end (tools\scene\eyes_frame_unit.cpp): the same objects, its own main
$ey = Join-Path $obj 'tools_scene_eyes_frame_unit.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\eyes_frame_unit.cpp' -o ($ey.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'eyes_frame_unit compile failed' }
$eobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $ey
& clang++ -fuse-ld=lld -g @eobjs -o (Join-Path $outDir 'eyes_frame_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'eyes_frame_unit link failed' }
Write-Host "built $(Join-Path $outDir 'eyes_frame_unit.exe')"

# S103 PVR: the posed-vertex ring's unit (tools\scene\pvr_unit.cpp): a synthetic re-posed model through the REAL FrontEnd + renderer, ring vs the old staged copy, the same objects, its own main
$pv = Join-Path $obj 'tools_scene_pvr_unit.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\pvr_unit.cpp' -o ($pv.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'pvr_unit compile failed' }
$pobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $pv
& clang++ -fuse-ld=lld -g @pobjs -o (Join-Path $outDir 'pvr_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'pvr_unit link failed' }
Write-Host "built $(Join-Path $outDir 'pvr_unit.exe')"

# S103 CS1B: a guest model casts into the shadow maps (tools\scene\cs1b_unit.cpp): PutGuest keeps the CPU copy, MeasureCasters n > 0, the caster cache is swept; the same objects, its own main
$cs = Join-Path $obj 'tools_scene_cs1b_unit.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\cs1b_unit.cpp' -o ($cs.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'cs1b_unit compile failed' }
$cobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $cs
& clang++ -fuse-ld=lld -g @cobjs -o (Join-Path $outDir 'cs1b_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'cs1b_unit link failed' }
Write-Host "built $(Join-Path $outDir 'cs1b_unit.exe')"

# S103 UP: the shared stage page's unit (tools\scene\crowd_page_unit.cpp): CrowdPages + the REAL FrontEnd (AdoptPage / UpdatePage / JoinCurrentPage / the AddModelSharing assert), the same objects, its own main
$up = Join-Path $obj 'tools_scene_crowd_page_unit.cpp.o'
& $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel 'tools\scene\crowd_page_unit.cpp' -o ($up.Substring($repoLen))
if ($LASTEXITCODE -ne 0) { throw 'crowd_page_unit compile failed' }
$uobjs = @($objs | Where-Object { $_ -notmatch 'frame_rig\.cpp\.o$' }) + $up
& clang++ -fuse-ld=lld -g @uobjs -o (Join-Path $outDir 'ups_page_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'ups_page_unit link failed' }
Write-Host "built $(Join-Path $outDir 'ups_page_unit.exe')"
