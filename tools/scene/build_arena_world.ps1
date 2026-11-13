# Builds lane 130's two test exes: out\scene\arena_world_test.exe (the writer's unit test) and out\scene\arena_world_oracle.exe (the capture-side oracle).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\scene\build_arena_world.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main = if ($env:RDR_MAIN_CHECKOUT) { $env:RDR_MAIN_CHECKOUT } else { 'C:\Users\Jarrod\wwe2k14-recomp' }
$out = Join-Path $repo 'out\scene'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$srcs = @('tools\scene\arena_world_test.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene.cpp') | ForEach-Object { Join-Path $repo $_ }
& $clang @flags -fuse-ld=lld @srcs -o (Join-Path $out 'arena_world_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'arena_world_test build failed' }
Write-Host "built $(Join-Path $out 'arena_world_test.exe')"
# the oracle reads the arena pac through the loader seam (like arena_rec); its objects are cached next to arena_rec's
$stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
$v = Join-Path $repo 'src\assets\view'
$inc = @('-I', $stb, '-I', (Join-Path $repo 'src\launcher'))
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING')
$osrcs = @((Join-Path $repo 'tools\scene\arena_world_oracle.cpp'), (Join-Path $v 'loader_seam_game.cpp'), (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $v 'mods_win32.cpp'))
$obj = Join-Path $out 'arena_world_obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$repoLen = $repo.Length + 1
$objs = @()
foreach ($s in $osrcs) {
  $o = Join-Path $obj ((($s.Substring($repoLen)) -replace '[\\/:]', '_') + '.o')
  $objs += $o
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $s).LastWriteTimeUtc -and $s -notmatch 'arena_world_oracle') { continue }
  & $clang -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @inc $s -o $o
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
}
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& $clang -fuse-ld=lld -g @objs -o (Join-Path $out 'arena_world_oracle.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'arena_world_oracle link failed' }
Write-Host "built $(Join-Path $out 'arena_world_oracle.exe')"
