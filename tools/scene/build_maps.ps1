# Builds lane E's offline driver (Session 103): out\tools\maps_draw.exe (the Maps stage hook of src\renderer_scene\live_maps.h on one scene record, headless).
# Compile only: no game, no run lock, no SDK. Reuses the object folder of build_rsc.ps1 (run that first, once: the renderer's objects live there).
#   pwsh -NoProfile -File tools\scene\build_maps.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rsc_obj'
if (-not (Test-Path $obj)) { throw 'run tools\scene\build_rsc.ps1 first (the renderer objects)' }
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$new = @('tools\scene\maps_draw.cpp', 'src\renderer_scene\live_maps.cpp', 'src\renderer_scene\live_map_shadow.cpp', 'src\renderer_scene\live_map_circles.cpp', 'src\renderer_scene\live_map_props.cpp',
          'src\renderer_scene\live_map_e2_input.cpp', 'src\renderer_scene\live_map_rim.cpp', 'src\renderer_scene\live_map_chain.cpp','src\renderer_scene\live_map_plane.cpp', 'src\renderer_scene\live_map_rope.cpp', 'src\renderer_scene\live_map_rope_input.cpp', 'src\assets\ring_game_space.cpp', 'src\renderer_scene\live_map_prelight.cpp', 'src\renderer_scene\live_map_sweat.cpp')   # S103 E2   (S105 SPV: the effects' words, fx_spv_data.cpp, come with build_rsc.ps1's objects, which are linked above)
$repoLen = $repo.Path.Length + 1
$incRel = ConvertTo-CxxRel $inc   # S103 H1
foreach ($rel in $new) {
  $src = Join-Path $repo $rel
  $o = Join-Path $obj (($rel -replace '[\\/:]', '_') + '.o')
  $log = & $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel $rel -o ($o.Substring($repoLen)) 2>&1
  if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw "clang++ failed on $rel" }
}
Show-CxxCache
# link: everything build_rsc made except its own main and the crowd tool, plus ours
$skip = 'tools_scene_rsc_draw|tools_scene_rsc_dump|tools_scene_crowd_make|src_scene_live_crowd_contrib|tools_scene_\w*(_test|_unit|_check|_rig)\.cpp\.o$'   # S103 05:3x: crowd_live_rig / frame_rig objects have their own main too   # S103: any test / unit tool object in the shared folder has its own main (F4's f_test)   # S103 E2: crowd_body is needed now (rf_crowd.cpp calls PoseBody)
$objs = Get-ChildItem $obj -Filter '*.o' | Where-Object { $_.Name -notmatch $skip } | Select-Object -ExpandProperty FullName
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'maps_draw.exe') @libs 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'maps_draw link failed' }
Write-Host "built $(Join-Path $outDir 'maps_draw.exe')"
