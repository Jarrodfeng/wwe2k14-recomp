# Builds post_proof (S101 lane 34): out\renderer\<dev|release>\post_proof.exe. No SDK, no game, no run lock. Only changed files are recompiled.
#   pwsh -NoProfile -File tools\post\build_proof.ps1 [-Release] [-Clean]
# Compiles the renderer's sources (minus the player), the fview session / describe / effects (no ImGui, no window), our post pass and the runner.
param([switch]$Release, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$kind = if ($Release) { 'release' } else { 'dev' }
$out = Join-Path $repo "out\renderer\$kind"
$objects = Join-Path $out 'post_proof_obj'
if ($Clean -and (Test-Path $objects)) { Remove-Item -Recurse -Force -Confirm:$false $objects }
New-Item -ItemType Directory -Force $objects | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$imgui = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\imgui'
if (-not (Test-Path (Join-Path $imgui 'imgui.h'))) { $imgui = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\imgui' }
$stb = Join-Path (Split-Path $imgui -Parent) 'stb'   # stb_image.h for crowd_body.cpp (as tools\fview\build.ps1)
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', "-I$sdk\Include", "-I$stb", "-I$(Join-Path (Split-Path $stb -Parent) 'xxHash')", '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations', '-MMD')
if ($Release) { $flags += @('-O2', '-DNDEBUG') } else { $flags += @('-O1', '-DRDR_DEV') }
$sources = @()
$sources += Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\src\\renderer\\player\\' -and $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName
$sources += @('session.cpp', 'describe.cpp', 'effects.cpp') | ForEach-Object { Join-Path $repo "src\fview\$_" }
# What session.cpp needs beyond the renderer (the same list fview's build reads: tools\fview\session_deps.txt).
$sources += @(Get-Content (Join-Path $repo 'tools\fview\session_deps.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { Join-Path $repo $_.Trim() })
# (post_pass.cpp comes with tools\fview\session_deps.txt since S101 lane PO: session.cpp links the post chain's promotion)
$sources += Join-Path $repo 'src\post_proof\post_proof.cpp'
$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$sources | ForEach-Object -ThrottleLimit 12 -Parallel {
  $source = $_
  $object = Join-Path $using:objects (($source -replace '[\\/:]', '_') + '.o')
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
}
if ($failed.Count) { $failed | ForEach-Object { Write-Host $_ }; throw "$($failed.Count) file(s) failed to compile" }
$exe = Join-Path $out 'post_proof.exe'
$objectFiles = @($sources | ForEach-Object { Join-Path $objects (($_ -replace '[\\/:]', '_') + '.o') })
& $clang -fuse-ld=lld -g @objectFiles -luser32 -o $exe
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Write-Host "built $exe"
