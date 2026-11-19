# Builds lane F's offline test (Session 103): out\tools\f_test.exe = tools\scene\f_test.cpp (the see-through order, the particle tables, the contributor, and the
# live particle hook drawn through the scene front-end with a headless renderer). Compile only: no game, no run lock, no SDK. Shares rsc_obj with build_rsc.ps1.
#   pwsh -NoProfile -File tools\scene\build_f_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rsc_obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$s = Join-Path $repo 'src\renderer_scene'
$srcs = @((Join-Path $PSScriptRoot 'f_test.cpp'), (Join-Path $s 'record_io.cpp'), (Join-Path $s 'front_end.cpp'), (Join-Path $s 'char_effects.cpp'), (Join-Path $s 'shader_reload.cpp'),
          (Join-Path $s 'post_pass.cpp'), (Join-Path $s 'draw_lists.cpp'), (Join-Path $s 'live_particles.cpp'), (Join-Path $repo 'src\scene_live\particle_contrib.cpp'), (Join-Path $repo 'src\scene_live\particle_reader.cpp'))
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
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
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-ladvapi32', '-lgdi32')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'f_test.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'f_test link failed' }
Write-Host "built $(Join-Path $outDir 'f_test.exe')"
