# Builds src\case standalone into out\case\case_selftest.exe: no SDK, no game, no run lock. Same clang as tools\renderer\build.ps1.
#   pwsh -NoProfile -File tools\case\build.ps1 [-Release]
# xxHash comes from the main checkout's SDK source (gitignored thirdparty); src\case finds it by __has_include or this -I.
param([switch]$Release)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main = if ($env:RDR_MAIN_CHECKOUT) { $env:RDR_MAIN_CHECKOUT } else { 'C:\Users\Jarrod\wwe2k14-recomp' }
$thirdparty = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty'
if (-not (Test-Path (Join-Path $thirdparty 'xxHash\xxhash.h'))) { throw "xxhash.h not found under $thirdparty" }
$out = Join-Path $repo 'out\case'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', "-I$thirdparty", '-D_CRT_SECURE_NO_WARNINGS')
if ($Release) { $flags += @('-O2', '-DNDEBUG') } else { $flags += @('-O1') }
$sources = @('zip_writer.cpp', 'case_ring.cpp', 'case_writer.cpp', 'case_selftest.cpp') | ForEach-Object { Join-Path $repo "src\case\$_" }
$exe = Join-Path $out 'case_selftest.exe'
& $clang @flags -fuse-ld=lld @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'case_selftest build failed' }
Write-Host "built $exe"
