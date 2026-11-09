# Builds the asset viewer: src\assets\view\*.cpp + the test seam -> out\tools\wwe2k14_assets.exe.
# Compile only; no game, no run lock. Windows subsystem with main() (no console window when the launcher starts it). The integration swaps loader_seam_test.cpp for the real seam (pass -Seam <file.cpp>).
# Also copies the exe to out\build\win-amd64-release\ when that folder exists (the launcher's ASSET VIEWER button looks for it beside itself).
# Usage: pwsh -NoProfile -File tools\assets\build_viewer.ps1 [-Seam path\to\seam.cpp] [-Extra "extra clang args"] [-DebugUi]
# -DebugUi (Session 98 phone fixes): Dear ImGui's recoverable errors assert and show their tooltip again (a player build only logs them, ui.cpp Init).
param([string]$Seam = '', [string[]]$Extra = @(), [switch]$DebugUi)
if ($DebugUi) { $Extra += '-DWWE_VIEWER_DEBUG' }
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'wwe2k14_assets.exe'
$v = Join-Path $repo 'src\assets\view'
# Session 96: the default is the real seam (the player's game files); -Seam test builds the built-in test model.
if (-not $Seam) { $Seam = Join-Path $v 'loader_seam_game.cpp' } elseif ($Seam -eq 'test') { $Seam = Join-Path $v 'loader_seam_test.cpp' }
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {  # a worktree has no thirdparty folder: use the main checkout's
  $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
# Session 98 (Arena Viewer UI): Dear ImGui core + the win32 / dx11 backends, from the SDK's copy (the main checkout's when in a worktree).
# Session 98 (phone port): + the sdl3 / vulkan backends (--backend vk). SDL3 = built below from the SDK source (also SDL_SavePNG for
# shots); Vulkan = the SDK source's headers (the loader is opened at run time: vk_loader.cpp).
# Compiled once to out\tools\imgui_obj\*.o (rebuilt only when the source is newer); the viewer's own sources are compiled every time, in parallel.
$imgui = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\imgui'
if (-not (Test-Path (Join-Path $imgui 'imgui.h'))) {
  $imgui = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\imgui'
}
$vkh = Join-Path (Split-Path $imgui -Parent) 'vulkan-headers\include'
# SDL3: built here once from the SDK source tree, static with the STATIC C runtime (/MT, like the rest of this exe: no VC runtime
# DLLs needed - the SDK install's SDL3-static.lib is /MD and pulled VCRUNTIME140.dll in). out\tools\sdl3_mt\SDL3-static.lib.
$sdlSrc = Join-Path (Split-Path $imgui -Parent) 'sdl3'
$sdlBuild = Join-Path $outDir 'sdl3_mt'
$sdlLib = Join-Path $sdlBuild 'SDL3-static.lib'
if (-not (Test-Path $sdlLib)) {
  & cmake -S $sdlSrc -B $sdlBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF -DSDL_CAMERA=OFF -DSDL_GPU=OFF -DSDL_AUDIO=OFF | Out-Null
  & cmake --build $sdlBuild | Out-Null
  if (-not (Test-Path $sdlLib)) { throw "SDL3 build failed ($sdlBuild)" }
}
$sdlInc = Join-Path $sdlSrc 'include'
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES', '-DWWE_VIEWER_VK')
$objDir = Join-Path $outDir 'imgui_obj'
New-Item -ItemType Directory -Force $objDir | Out-Null
$imguiObjs = @()
# The objects are rebuilt when any imgui source / header is newer than the stamp, or the flags / -Extra differ from the stamp's.
$imFlags = '-O2 -std=c++20 ' + ($defs -join ' ') + ' ' + ($Extra -join ' ')
$newest = (Get-ChildItem -Path $imgui -Recurse -Include *.h, *.cpp | Where-Object { $_.FullName -notmatch '\\(examples|misc|docs)\\' } | Measure-Object LastWriteTimeUtc -Maximum).Maximum.Ticks
$stampFile = Join-Path $objDir 'stamp.txt'
$stamp = "$newest|$imFlags"
$rebuildAll = -not (Test-Path $stampFile) -or ((Get-Content $stampFile -Raw).Trim() -ne $stamp)
foreach ($f in @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends\imgui_impl_win32.cpp', 'backends\imgui_impl_dx11.cpp', 'backends\imgui_impl_sdl3.cpp', 'backends\imgui_impl_vulkan.cpp')) {
  $src = Join-Path $imgui $f
  $obj = Join-Path $objDir ((Split-Path $f -Leaf) + '.o')
  if ($rebuildAll -or -not (Test-Path $obj)) {
    & clang++ -O2 -std=c++20 -c @defs -Wno-unused-command-line-argument -I $imgui -I $vkh -I $sdlInc @Extra $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "clang++ failed on $f ($LASTEXITCODE)" }
  }
  $imguiObjs += $obj
}
Set-Content -Path $stampFile -Value $stamp
$inc = @('-I', $stb, '-I', (Join-Path $repo 'src\launcher'), '-I', $imgui, '-I', $sdlInc, '-I', $vkh)
# Session 98 (stance, src\assets\stance_table.h): the retail XEX key, taken from the SDK source at build time (not kept in this repo), so
# the viewer can read the stance table from the player's own default.xex. Missing = the viewer falls back to one stance for everyone.
$xexMod = Join-Path (Split-Path (Split-Path $imgui -Parent) -Parent) 'src\system\xex_module.cpp'
if ((Test-Path $xexMod) -and ((Get-Content $xexMod -Raw) -match 'xe_xex2_retail_key\[16\]\s*=\s*\{([^}]*)\}')) {
  $inc += @("-DWWE_XEX_RETAIL_KEY=$((($Matches[1] -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join ',')")
} else { Write-Host "warning: no XEX key in $xexMod (the stance table is then read from --guest_image only)" }
$names = @('viewer_main', 'renderer', 'render_shared', 'renderer_d3d11', 'renderer_vk', 'vk_loader', 'shot', 'platform_win32', 'platform_sdl', 'ui', 'ui_light',
           'ui_backend_win32_dx11', 'ui_backend_sdl3_vk', 'post', 'shadow', 'pick', 'ui_inspect', 'render_job', 'ui_path', 'ui_timeline', 'ui_mods', 'ui_importer', 'mods_win32',
           'video_player')
$srcs = @($names | ForEach-Object { Join-Path $v "$_.cpp" }) + @($Seam)
$srcs += @(Join-Path $repo 'src\importer\importer_core.cpp')  # Session 98: the Superstar Importer core (src\importer\importer_core.h)
$vobj = Join-Path $outDir 'viewer_obj'
New-Item -ItemType Directory -Force $vobj | Out-Null
$fails = $srcs | ForEach-Object -ThrottleLimit 16 -Parallel {
  $o = Join-Path $using:vobj ([IO.Path]::GetFileNameWithoutExtension($_) + '.o')
  $log = & clang++ -O2 -std=c++20 -c @using:defs -Wno-unused-command-line-argument @using:inc @using:Extra $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") } elseif ($log) { ($log | Where-Object { $_ -match 'warning' }) -join "`n" }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$fails | Where-Object { $_ } | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$objs = @($srcs | ForEach-Object { Join-Path $vobj ([IO.Path]::GetFileNameWithoutExtension($_) + '.o') }) + $imguiObjs
$libs = @('-Wl,/SUBSYSTEM:WINDOWS,/ENTRY:mainCRTStartup', '-ld3d11', '-ldxgi', '-ld3dcompiler', '-lwindowscodecs', '-lole32', '-luser32', '-luuid', '-lshell32', '-ldwmapi', '-lgdi32', '-lcomdlg32', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lshlwapi', '-lpropsys',
          $sdlLib, '-lsetupapi', '-lwinmm', '-limm32', '-lversion', '-loleaut32', '-lcfgmgr32', '-ladvapi32', '-lhid')
& clang++ -O2 -Wno-unused-command-line-argument @objs -o $exe @libs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
Write-Host "built $exe"
$rel = Join-Path $repo 'out\build\win-amd64-release'
if (Test-Path $rel) { Copy-Item -Force $exe (Join-Path $rel 'wwe2k14_assets.exe'); Write-Host "copied to $rel" }
