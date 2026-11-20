# Builds out\tools\w2_skin_path_check.exe and out\tools\w2_unused.exe (Session 103, lane W1). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\wrestler\build_w1_check.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$out = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $out | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$v = Join-Path $repo 'src\assets\view'
$flags = @('-std=c++20', '-O1', '-g', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations')
$srcs = @('tools\wrestler\w2_skin_path_check.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\char_effects.cpp', 'src\renderer\shaders\spv\fx_spv_data.cpp','src\assets\view\loader_seam_game.cpp', 'src\importer\importer_core.cpp', 'src\assets\view\mods_win32.cpp', 'src\assets\crowd_body.cpp', 'src\assets\crowd_prop_game_space.cpp') | ForEach-Object { Join-Path $repo $_ }
& clang++ @flags -I $stb -I (Join-Path $repo 'src\launcher') @srcs -o (Join-Path $out 'w2_skin_path_check.exe') -lole32 -luser32 -lshell32 -ladvapi32 -lshlwapi -lwindowscodecs -luuid -lgdi32 -lcomdlg32 -lpropsys -lmfplat -lmfreadwrite -lmfuuid
if ($LASTEXITCODE -ne 0) { throw 'skin check build failed' }
Write-Host "built $out"
