# S105 C4 (from S103 TQF build_technique_census.ps1): builds tools\assets\choice_pair_unit.cpp -> out\tools\choice_pair_unit.exe (-DRDR_DEV). One unit that #includes src\renderer_scene\front_end.cpp so the REAL ChooseEffect runs; the renderer symbols the
# census never calls are left unresolved at link (/force:unresolved). Compile only: no game, no run lock, no SDK build.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'choice_pair_unit.exe'
$main = $null
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
$inc = @("-I$stb", "-I$xxh", "-I$vkh", ('-I' + (Join-Path $repo 'src\launcher')))
if ($sdk) { $inc += ('-I' + (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$libs = @('-lole32', '-luser32', '-luuid', '-lshell32', '-lshlwapi', '-lwindowscodecs', '-ladvapi32', '-lgdi32', '-lcomdlg32', '-lpropsys', '-lmfplat', '-lmfreadwrite', '-lmfuuid')
$log = & clang++ -O0 -std=c++20 -fuse-ld=lld @defs -Wno-unused-command-line-argument @inc (Join-Path $repo 'tools\assets\choice_pair_unit.cpp') -o $exe '-Wl,/force:unresolved' '-Wl,/ignore:4217' @libs 2>&1
if ($LASTEXITCODE -ne 0) { $log | Select-Object -First 60 | ForEach-Object { Write-Host $_ }; throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
