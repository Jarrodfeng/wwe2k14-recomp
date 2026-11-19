# S103 PN23: clang -fsyntax-only of tools\scene\picture_resolver_syntax.cpp (the header-only PictureResolver compiles on its own). Compile only; 0 game runs.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$vkh = Join-Path (Split-Path $stb -Parent) 'vulkan-headers\include'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1)
$inc = @('-I', $stb, '-I', $vkh, '-I', (Join-Path (Split-Path $stb -Parent) 'xxHash'))
if ($sdk) { $inc += @('-I', (Join-Path $sdk.FullName 'Include')) }
& clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -DNOMINMAX -DWIN32_LEAN_AND_MEAN @inc (Join-Path $PSScriptRoot 'picture_resolver_syntax.cpp')
if ($LASTEXITCODE -ne 0) { throw 'picture_resolver_syntax failed' }
Write-Host 'picture_resolver.h compiles'
