# Builds lane TQ2 fix unit (Session 103): out/tools/tq2_unit.exe = tools/scene/tq2_unit.cpp (the SearchLight YZ planar uv, game space vs the viewer y-negated space, through the REAL FrontEnd + a headless renderer). Compile only; shares rsc_obj with build_rfl_unit.ps1.
#   pwsh -NoProfile -File tools\scene\build_tq2_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')   # S103 H1: the shared compile cache (WWE_NO_CCACHE=1 = off)
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'rsc_obj'
New-Item -ItemType Directory -Force $obj | Out-Null
function Find-Third($rel) {
  $p = Join-Path $repo ('thirdparty\rexglue-sdk-src\thirdparty\' + $rel)
  if (Test-Path $p) { return $p }
  $m = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path ('thirdparty\rexglue-sdk-src\thirdparty\' + $rel)
  if (Test-Path $m) { return $m }
  throw "missing thirdparty $rel"
}
$stb = Find-Third 'stb'
$xxh = Find-Third 'xxHash'
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
if (-not (Test-Path $vkh)) { $vkh = Find-Third 'vulkan-headers\include' }
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $xxh, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$s = Join-Path $repo 'src\renderer_scene'
$srcs = @((Join-Path $PSScriptRoot 'tq2_unit.cpp'), (Join-Path $s 'record_io.cpp'), (Join-Path $s 'front_end.cpp'), (Join-Path $s 'char_effects.cpp'), (Join-Path $s 'shader_reload.cpp'),
          (Join-Path $s 'post_pass.cpp'), (Join-Path $s 'draw_lists.cpp'), (Join-Path $s 'live_particles.cpp'), (Join-Path $repo 'src\scene_live\particle_contrib.cpp'), (Join-Path $repo 'src\scene_live\particle_reader.cpp'))
$srcs += @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
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
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-ladvapi32', '-lgdi32', '-lwindowscodecs', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'tq2_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'tq2_unit link failed' }
Write-Host "built $(Join-Path $outDir 'tq2_unit.exe')"
