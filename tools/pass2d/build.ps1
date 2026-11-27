# Session 101 lane 9: builds the 2D pass's test program (tools\pass2d\pass2d_test.cpp) to out\tools\pass2d_test.exe: the renderer's sources (as
# tools\renderer\build.ps1 compiles them), src\renderer_scene\*.cpp and the test. Headless, no SDK, no game build, no run lock. Only changed files are
# recompiled (-MMD dependency files). stb_image.h comes from the SDK source checkout (a worktree falls back to the main checkout's thirdparty).
#   pwsh -NoProfile -File tools\pass2d\build.ps1 [-Clean] [-NoShaders]
param([switch]$Clean, [switch]$NoShaders)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $repo 'out\pass2d'
$objects = Join-Path $out 'obj'
if ($Clean -and (Test-Path $objects)) { Remove-Item -Recurse -Force -Confirm:$false $objects }
New-Item -ItemType Directory -Force $objects | Out-Null

if (-not $NoShaders) {
  & py (Join-Path $repo 'tools\renderer\compile_shaders.py')
  if ($LASTEXITCODE -ne 0) { throw 'shader compile failed' }
}

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) {
  $main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
  $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb'
}
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { throw "stb_image.h not found under $stb" }
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-DRDR_DEV', "-I$sdk\Include", "-I$stb", "-I$repo", '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS', '-MMD')

$sources = @(Get-ChildItem (Join-Path $repo 'src\renderer') -Filter '*.cpp' | Where-Object { $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)   # S106 TIPFIX: surface_lifecycle_test.cpp has its own main
$sources += @(Get-ChildItem (Join-Path $repo 'src\renderer_scene') -Filter '*.cpp' | Where-Object { $_.Name -in @('pass2d.cpp', 'ui_assets.cpp') } | Select-Object -ExpandProperty FullName)
$sources += (Join-Path $repo 'tools\pass2d\pass2d_test.cpp')
$sources += (Join-Path $repo 'tools\pass2d\gaps_test.cpp')  # S101 lane 23: a second program (the gap tests); each exe links the library objects and its own main
$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
. (Join-Path $PSScriptRoot '..\admit\Admit.ps1'); try { $sources | ForEach-Object -ThrottleLimit (Request-BuildParallel -Want 12) -Parallel {   # CQW: the compile queue (tools\admit\Admit.ps1)
  $source = $_
  $object = Join-Path $using:objects (($source.Substring($using:repo.Length + 1) -replace '[\\/:]', '_') + '.o')
  $deps = [IO.Path]::ChangeExtension($object, '.d')
  $stale = -not (Test-Path $object)
  if (-not $stale -and (Test-Path $deps)) {
    $objectTime = (Get-Item $object).LastWriteTimeUtc
    $list = (Get-Content -Raw $deps) -replace '\\\r?\n', ' '
    $list = $list.Substring($list.IndexOf(': ') + 2)
    foreach ($item in ($list -split '(?<!\\)\s+')) {
      $path = $item -replace '\\ ', ' '
      if ($path -and (Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).LastWriteTimeUtc -gt $objectTime) { $stale = $true; break }
    }
  }
  if ($stale) {
    $text = & $using:clang @using:flags -c $source -o $object 2>&1
    if ($LASTEXITCODE -ne 0) { ($using:failed).Add("$source`n$($text -join "`n")") } elseif ($text) { Write-Host ($text -join "`n") }
  }
} } finally { Release-BuildParallel }
if ($failed.Count) { $failed | ForEach-Object { Write-Host $_ }; throw "$($failed.Count) file(s) failed to compile" }

New-Item -ItemType Directory -Force (Join-Path $repo 'out\tools') | Out-Null
# S106 TIPFIX: link only this build's own sources' objects (a stale test object in a reused out\pass2d\obj is never linked)
$allObjects = @($sources | ForEach-Object { Join-Path $objects (($_.Substring($repo.Length + 1) -replace '[\\/:]', '_') + '.o') })
foreach ($pair in @(@('pass2d_test.exe', 'gaps_test'), @('pass2d_gaps_test.exe', 'pass2d_test'))) {
  $exe = Join-Path $repo ('out\tools\' + $pair[0])
  $objectFiles = @($allObjects | Where-Object { (Split-Path $_ -Leaf) -notlike ('*' + $pair[1] + '.cpp.o') })  # the other program's main stays out
  & $clang -fuse-ld=lld -g @objectFiles -luser32 -lgdi32 -o $exe   # S106 TIPFIX: swapchain.cpp / swapchain.cpp (rdr::Swapchain::Build) GetDeviceCaps (gdi32)
  if ($LASTEXITCODE -ne 0) { throw "link failed: $exe" }
  Write-Host "built $exe"
}
