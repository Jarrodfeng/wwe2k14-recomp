# Builds the scene-record tools (Session 101, lane 14): out\tools\rsc_dump.exe (print a record's tables) and out\tools\rsc_draw.exe (load a
# record from a file, draw it through the scene front-end into a PNG). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_rsc.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rsc_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
# the viewer's loader seam needs the XEX key for the stance table (optional; without it one stance for everyone: irrelevant to a record's rigid draw)
$v = Join-Path $repo 'src\assets\view'
$srcs = @((Join-Path $PSScriptRoot 'rsc_draw.cpp'), (Join-Path $v 'loader_seam_game.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'),
          (Join-Path $repo 'src\renderer_scene\front_end.cpp'), (Join-Path $repo 'src\renderer_scene\char_effects.cpp'), (Join-Path $repo 'src\renderer_scene\shader_reload.cpp'), (Join-Path $repo 'src\renderer_scene\post_pass.cpp'),  # S101 lane24: front_end.cpp calls ShaderReload; lane 34: and the post pass
          (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $v 'mods_win32.cpp'),
          (Join-Path $repo 'src\assets\crowd_body.cpp'), (Join-Path $repo 'src\scene_live\crowd_contrib.cpp'), (Join-Path $PSScriptRoot 'crowd_make.cpp'))  # S101 lane27: the crowd contributor + its offline driver
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' } | Select-Object -ExpandProperty FullName)
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$fails = $srcs | ForEach-Object -ThrottleLimit 16 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & clang++ -O1 -g -std=c++20 -c @using:defs -Wno-unused-command-line-argument @using:inc $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }

# rsc_dump: the record file code only
$dump = Join-Path $obj 'tools_scene_rsc_dump.cpp.o'
& clang++ -O1 -g -std=c++20 -c @defs (Join-Path $PSScriptRoot 'rsc_dump.cpp') -o $dump
if ($LASTEXITCODE -ne 0) { throw 'rsc_dump compile failed' }
$rio = Join-Path $obj 'src_renderer_scene_record_io.cpp.o'
& clang++ -fuse-ld=lld -g $dump $rio -o (Join-Path $outDir 'rsc_dump.exe')
if ($LASTEXITCODE -ne 0) { throw 'rsc_dump link failed' }
Write-Host "built $(Join-Path $outDir 'rsc_dump.exe')"

$objs = @($srcs | Where-Object { $_ -notmatch 'crowd_make\.cpp$' } | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'rsc_draw.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'rsc_draw link failed' }
Write-Host "built $(Join-Path $outDir 'rsc_draw.exe')"

# S101 lane27: crowd_make (the crowd contributor's offline driver): no renderer, no front end
$cm = @($srcs | Where-Object { $_ -notmatch '[\\/](rsc_draw|front_end|char_effects|post_pass)\.cpp$' -and $_ -notmatch '[\\/]src[\\/]renderer[\\/]' } | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
& clang++ -fuse-ld=lld -g @cm -o (Join-Path $outDir 'crowd_make.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'crowd_make link failed' }
Write-Host "built $(Join-Path $outDir 'crowd_make.exe')"
