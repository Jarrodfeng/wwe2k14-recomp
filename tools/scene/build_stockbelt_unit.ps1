# Builds out\tools\stockbelt_unit.exe (S111 lane STOCKBELT): the R5 stock-belt recolour of a created title (belt_colour.h, the CBPM mask belt_mask.h, the cc_mask pick), with the REAL ChooseEffect
# (the unit #includes front_end.cpp, as thb_unit does; renderer symbols it never calls stay unresolved at link) and record_io.cpp. Compile only: no game, no run lock, no SDK build.
#   pwsh -NoProfile -File tools\scene\build_stockbelt_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'stockbelt_unit.exe'
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
$inc = @("-I$stb", "-I$xxh", "-I$vkh", ('-I' + (Join-Path $repo 'src\launcher')), ('-I' + (Join-Path $repo 'src')))
if ($sdk) { $inc += ('-I' + (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$srcs = @((Join-Path $PSScriptRoot 'stockbelt_unit.cpp'), (Join-Path $repo 'src\renderer_scene\record_io.cpp'), (Join-Path $repo 'src\renderer\shaders\spv\fx_spv_data.cpp'))
$log = & clang++ -O0 -std=c++20 -fuse-ld=lld @defs -Wno-unused-command-line-argument @inc @srcs -o $exe '-Wl,/force:unresolved' '-Wl,/ignore:4217' @libs 2>&1
if (@($log | Where-Object { "$_" -match 'undefined symbol.*kFx_' }).Count) { throw "link: kFx_ symbols unresolved (fx_spv_data.cpp not linked); /force:unresolved would hide them as null" }
if ($LASTEXITCODE -ne 0) { $log | Select-Object -First 60 | ForEach-Object { Write-Host $_ }; throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
