# Builds the live scene writer's test exe (Session 101, lane 26): out\scene\live_scene_selftest.exe. Compile only: no SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\scene\build_live_writer.ps1 [-Release]
# Sources: the writer, the folder sink, record_io, the case ring / writer / zip (xxHash from the main checkout's SDK source), the case ring's scene sink.
param([switch]$Release)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main = if ($env:RDR_MAIN_CHECKOUT) { $env:RDR_MAIN_CHECKOUT } else { 'C:\Users\Jarrod\wwe2k14-recomp' }
$thirdparty = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty'
if (-not (Test-Path (Join-Path $thirdparty 'xxHash\xxhash.h'))) { throw "xxhash.h not found under $thirdparty" }
$out = Join-Path $repo 'out\scene'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', "-I$thirdparty", '-D_CRT_SECURE_NO_WARNINGS')
if ($Release) { $flags += @('-O2', '-DNDEBUG') } else { $flags += @('-O1') }
$list = @('src\renderer_scene\live_scene.cpp', 'src\renderer_scene\live_sinks.cpp', 'src\renderer_scene\record_io.cpp', 'src\renderer_scene\live_scene_selftest.cpp',
          'src\case\zip_writer.cpp', 'src\case\case_ring.cpp', 'src\case\case_writer.cpp', 'src\case\case_scene.cpp')
$sources = $list | ForEach-Object { Join-Path $repo $_ }
$exe = Join-Path $out 'live_scene_selftest.exe'
& $clang @flags -fuse-ld=lld @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'live_scene_selftest build failed' }
Write-Host "built $exe"
