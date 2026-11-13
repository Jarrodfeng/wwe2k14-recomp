# Builds out\tools\ui_ours_test.exe (Session 101, lane 66): the unit test of our UI textures (ui_ours_core) on the files and the captures.
# Also compiles src\renderer_scene\ui_ours.cpp (the GPU side of the hook) to an object, as a compile check. Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\ui\build_ui_ours_test.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'ui_ours_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Join-Path (Split-Path $stb -Parent) 'xxHash'))   # (xxhash.h: the contract's one content hash)
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$srcs = @((Join-Path $PSScriptRoot 'ui_ours_test.cpp'), (Join-Path $repo 'src\renderer_scene\ui_ours_core.cpp'))
# compile-only: the GPU side of the hook (needs the renderer's headers; not linked into the test)
$check = @((Join-Path $repo 'src\renderer_scene\ui_ours.cpp'))
$all = $srcs + $check
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$fails = $all | ForEach-Object -ThrottleLimit 8 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & clang++ -O2 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -c @using:defs -Wno-unused-command-line-argument @using:inc $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") } elseif ($log) { Write-Host ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-ladvapi32')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'ui_ours_test.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'ui_ours_test link failed' }
Write-Host "built $(Join-Path $outDir 'ui_ours_test.exe')"
