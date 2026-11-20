# Builds lane C's offline driver (Session 103): out\tools\c_driver.exe. Compile only: no game, no run lock, no SDK.  pwsh -NoProfile -File tools\scene\build_c_driver.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'cdrv_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$v = Join-Path $repo 'src\assets\view'
$rs = Join-Path $repo 'src\renderer_scene'
$srcs = @((Join-Path $PSScriptRoot 'c_driver.cpp'), (Join-Path $v 'loader_seam_game.cpp'), (Join-Path $rs 'record_io.cpp'), (Join-Path $rs 'front_end.cpp'), (Join-Path $rs 'draw_lists.cpp'), (Join-Path $rs 'char_effects.cpp'),
          (Join-Path $rs 'shader_reload.cpp'), (Join-Path $rs 'post_pass.cpp'), (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $v 'mods_win32.cpp'),
          (Join-Path $repo 'src\assets\crowd_body.cpp'), (Join-Path $repo 'src\scene_live\crowd_contrib.cpp'),
          (Join-Path $rs 'rf_c_core.cpp'), (Join-Path $rs 'rf_arena_live.cpp'), (Join-Path $rs 'rf_ring.cpp'), (Join-Path $rs 'rf_props.cpp'), (Join-Path $rs 'rf_screens.cpp'), (Join-Path $rs 'live_flashes.cpp'))
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
$newestHdr = (@(Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp) + @(Get-ChildItem (Join-Path $repo 'tools\scene') -Filter *.h) | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$incRel = ConvertTo-CxxRel $inc   # S103 H1: repo-relative paths (the same text in every worktree)
. (Join-Path $PSScriptRoot '..\admit\Admit.ps1'); try { $fails = $srcs | ForEach-Object -ThrottleLimit (Request-BuildParallel -Want 16) -Parallel {   # CQW: the compile queue (tools\admit\Admit.ps1)
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
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'c_driver.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'c_driver link failed' }
Write-Host "built $(Join-Path $outDir 'c_driver.exe')"
