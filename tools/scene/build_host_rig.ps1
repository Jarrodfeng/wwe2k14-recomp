# S103 P7 H1: builds out\tools\host_rig.exe = the scene host (src\renderer_host) driven headless from capture scene records (tools\scene\host_rig.cpp). Same sources and flags as
# build_rig.ps1 (the object folder is shared: unchanged units are reused) plus live_scene_draw.cpp and scene_host.cpp. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_host_rig.ps1 [-Clean]
# (the text below is build_rig.ps1's header:) out\tools\frame_rig.exe = one scene record drawn from the record alone through the shared record -> frame
# library (src\renderer_scene\record_frame.h) and the scene front-end, with GameFilesSource (live_assets.cpp) as the asset source (the same loaders the live game uses).
# Compile only: no game, no run lock, no SDK. frame_rig.py runs it and makes the sheets.
#   pwsh -NoProfile -File tools\scene\build_rig.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'host_rig_obj'   # S103 P7 (review): its own object folder: every unit here is compiled with -DWWE2K14_KEEP_STREAM=0 (the build the host belongs to), so none may be shared with build_rig.ps1's stream-mode objects (no ODR split)
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Split-Path $stb -Parent), '-I', (Join-Path (Split-Path $stb -Parent) 'xxHash'))   # S103 PN4: xxhash.h for image_content_table.h (the picture source); S103 D1: + the SDK's thirdparty folder (<xxHash/xxhash.h> for live_assets.cpp)
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV', '-DWWE2K14_KEEP_STREAM=0')   # S103 P7 (review): every unit of the host rig is an OFF-build unit
$rs = Join-Path $repo 'src\renderer_scene'
$srcs = @((Join-Path $PSScriptRoot 'host_rig.cpp'), (Join-Path $repo 'src\renderer_host\scene_host.cpp'), (Join-Path $rs 'live_scene_draw.cpp'),
          (Join-Path $rs 'record_io.cpp'), (Join-Path $rs 'front_end.cpp'), (Join-Path $rs 'char_effects.cpp'), (Join-Path $rs 'shader_reload.cpp'), (Join-Path $rs 'post_pass.cpp'), (Join-Path $rs 'post_from_record.cpp'),
          (Join-Path $rs 'record_frame.cpp'), (Join-Path $rs 'record_frame_classes.cpp'), (Join-Path $rs 'rf_arena.cpp'), (Join-Path $rs 'rf_crowd.cpp'), (Join-Path $rs 'live_crowd.cpp'), (Join-Path $rs 'rf_models.cpp'),
          (Join-Path $repo 'src\renderer_scene\rf_wrestlers.cpp'), (Join-Path $repo 'src\renderer_scene\rf_c_core.cpp'), (Join-Path $repo 'src\renderer_scene\rf_arena_live.cpp'), (Join-Path $repo 'src\renderer_scene\rf_ring.cpp'), (Join-Path $repo 'src\renderer_scene\rf_props.cpp'), (Join-Path $repo 'src\renderer_scene\rf_screens.cpp'), (Join-Path $repo 'src\renderer_scene\live_flashes.cpp'), (Join-Path $repo 'src\renderer_scene\draw_lists.cpp'),  # S103 S: the class table names lane B's and lane C's appliers
          (Join-Path $rs 'live_particles.cpp'),  # S103 F: the particle probe
          (Join-Path $rs 'live_maps.cpp'), (Join-Path $rs 'live_map_shadow.cpp'), (Join-Path $rs 'live_map_circles.cpp'), (Join-Path $rs 'live_map_props.cpp'), (Join-Path $rs 'live_map_e2_input.cpp'), (Join-Path $rs 'live_map_rim.cpp'), (Join-Path $rs 'live_map_chain.cpp'), (Join-Path $rs 'live_map_plane.cpp'), (Join-Path $rs 'live_map_rope.cpp'), (Join-Path $rs 'live_map_rope_input.cpp'), (Join-Path $repo 'src\assets\ring_game_space.cpp'), (Join-Path $rs 'live_map_prelight.cpp'), (Join-Path $rs 'live_map_sweat.cpp'), (Join-Path $rs 'live_particles.cpp'), (Join-Path $rs 'frame_hooks.cpp'),  # S103 R2: lane E's maps hook and lane F's particle hook, attached as the live frame attaches them
          (Join-Path $rs 'live_assets.cpp'),  # carries its own static stb_image
          (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $repo 'src\assets\view\mods_win32.cpp'), (Join-Path $repo 'src\assets\view\loader_seam_game.cpp'), (Join-Path $repo 'src\assets\crowd_body.cpp'), (Join-Path $repo 'src\assets\crowd_prop_game_space.cpp'),
          (Join-Path $rs 'quads2d_draw.cpp'), (Join-Path $rs 'quads2d_register.cpp'),   # S103 P7 H2: the 2D rows' drawer and its programs
          (Join-Path $rs 'ui_file_index.cpp'))   # S103 PN4: the file index of the real picture resolver (--real)
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
$newestHdr = (Get-ChildItem (Join-Path $repo 'src'), (Join-Path $repo 'tools/scene') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$incRel = ConvertTo-CxxRel $inc   # S103 H1: repo-relative paths (the same text in every worktree)
$fails = $srcs | ForEach-Object -ThrottleLimit 16 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & $using:cxx0 @using:cxxRest -O1 -g -std=c++20 -c @using:defs -Wno-unused-command-line-argument @using:incRel ($_.Substring($using:repoLen)) -o ($o.Substring($using:repoLen)) 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
Show-CxxCache
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'host_rig.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'host_rig link failed' }
Write-Host "built $(Join-Path $outDir 'host_rig.exe')"

