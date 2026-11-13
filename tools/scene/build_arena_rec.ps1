# Builds out\tools\arena_rec.exe (Session 101, lane 28): the arena contributor's record from the player's own files, no GPU, no game.
# Compile only; reuses the object cache of build_rsc.ps1 for the files both share (the loader seam, the record file code, the importer).
#   pwsh -NoProfile -File tools\scene\build_arena_rec.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'arena_rec_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$inc = @('-I', $stb, '-I', (Join-Path $repo 'src\launcher'))
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING')
$v = Join-Path $repo 'src\assets\view'
$srcs = @((Join-Path $PSScriptRoot 'arena_rec.cpp'), (Join-Path $repo 'src\scene_live\arena_contrib.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'),
          (Join-Path $v 'loader_seam_game.cpp'), (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $v 'mods_win32.cpp'))
$repoLen = $repo.Path.Length + 1
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$fails = $srcs | ForEach-Object -ThrottleLimit 8 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & clang++ -O1 -g -std=c++20 -c @using:defs -Wno-unused-command-line-argument @using:inc $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'arena_rec.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'arena_rec link failed' }
Write-Host "built $(Join-Path $outDir 'arena_rec.exe')"
