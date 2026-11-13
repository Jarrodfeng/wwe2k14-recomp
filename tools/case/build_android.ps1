# Compiles src\case for Android (arm64-v8a, the NDK's clang, API 28) as objects: the proof that the phone build can take it. No link, no APK, no phone.
#   pwsh -NoProfile -File tools\case\build_android.ps1
# The real phone build links these through the game's CMake (lane 1 adds src\case\*.cpp, minus case_selftest.cpp, to the game target).
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\android\env.ps1"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main = if ($env:RDR_MAIN_CHECKOUT) { $env:RDR_MAIN_CHECKOUT } else { 'C:\Users\Jarrod\wwe2k14-recomp' }
$thirdparty = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty'
$out = Join-Path $repo 'out\case\android'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = Join-Path $Llvm 'clang++.exe'
$sysroot = Join-Path (Split-Path -Parent $Llvm) 'sysroot'
$flags = @('--target=aarch64-linux-android28', "--sysroot=$sysroot", '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-O2', '-fPIC', "-I$thirdparty", '-c')
foreach ($name in 'zip_writer.cpp', 'case_ring.cpp', 'case_writer.cpp') {
  $src = Join-Path $repo "src\case\$name"
  & $clang @flags $src -o (Join-Path $out ($name + '.o'))
  if ($LASTEXITCODE -ne 0) { throw "android compile of $name failed" }
}
# the JNI entry point and the hook header, as a translation unit of their own
$probe = Join-Path $out 'jni_probe.cpp'
Set-Content -Path $probe -Value "#include `"$($repo -replace '\\','/')/src/case/case_jni.h`"`n"
& $clang @flags $probe -o (Join-Path $out 'jni_probe.o')
if ($LASTEXITCODE -ne 0) { throw 'android compile of case_jni.h failed' }
Write-Host "android objects in $out"
