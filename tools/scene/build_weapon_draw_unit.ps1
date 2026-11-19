# Builds out\tools\weapon_draw_unit.exe (S103 lane PW-3): the weapon drawer's loader and pure part on the player's own m2.pac and a capture's steps draws.
# Also syntax-checks the GPU side (src\renderer_scene\rf_props.cpp, live_assets.cpp) against the same headers as a compile check (objects, not linked). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_weapon_draw_unit.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'weapon_draw_unit_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', (Split-Path $stb -Parent), '-I', (Join-Path (Split-Path $stb -Parent) 'xxHash'), '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$v = Join-Path $repo 'src\assets\view'
$srcs = @((Join-Path $PSScriptRoot 'weapon_draw_unit.cpp'), (Join-Path $v 'loader_seam_game.cpp'), (Join-Path $repo 'src\importer\importer_core.cpp'), (Join-Path $v 'mods_win32.cpp'))
# compile-only: the applier (rf_props.cpp) and the loader dispatch (live_assets.cpp)
$check = @((Join-Path $repo 'src\renderer_scene\rf_props.cpp'), (Join-Path $repo 'src\renderer_scene\live_assets.cpp'))
$all = $srcs + $check
$newestHdr = (Get-ChildItem (Join-Path $repo 'src') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$fails = $all | ForEach-Object -ThrottleLimit 8 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -c @using:defs -Wno-unused-command-line-argument @using:inc $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'weapon_draw_unit.exe') @libs
if ($LASTEXITCODE -ne 0) { throw 'weapon_draw_unit link failed' }
Write-Host "built $(Join-Path $outDir 'weapon_draw_unit.exe')"
