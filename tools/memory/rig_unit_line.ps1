# CQM (2026-11-20): ONE compile of one source with build_rig's exact compile line (tools\scene\build_rig.ps1:33-39 include / define lists, :57 the line),
# so measure_peak.py can measure a single tool-build unit. The object goes to -Out (under this tree's out\). Same cache switch as the rig: WWE_NO_CCACHE=1 = plain clang++.
# Keep in step with build_rig.ps1 by hand if its flags change (this file is a measurement helper, not a build).
#   pwsh -NoProfile -File tools\memory\rig_unit_line.ps1 -Src src\renderer_scene\front_end.cpp -Out out\cqm\rig\front_end.o [-Show]
#   -Extra '-DX=1' appends flags (a define no other compile has = a guaranteed cache miss, for the sccache parent probe)
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Out, [switch]$Show, [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
. (Join-Path $repo 'tools\cxx_cache.ps1')
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path $repo 'src\launcher'), '-I', (Split-Path $stb -Parent))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV')
$incRel = ConvertTo-CxxRel $inc
New-Item -ItemType Directory -Force (Split-Path (Join-Path $repo $Out) -Parent) | Out-Null
Push-Location $repo
try {
  if ($Show) { Write-Host ("$cxx0 " + (@($cxxRest) + @('-O1', '-g', '-std=c++20', '-c') + $defs + @('-Wno-unused-command-line-argument') + $incRel + $Extra + @($Src, '-o', $Out) -join ' ')) }
  & $cxx0 @cxxRest -O1 -g -std=c++20 -c @defs -Wno-unused-command-line-argument @incRel @Extra $Src -o $Out
  exit $LASTEXITCODE
} finally { Pop-Location }
