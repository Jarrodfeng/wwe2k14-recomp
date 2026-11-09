# Session 98: builds tools\tron_zip\tron_zip_test.cpp (the PC check of src\tron_zip.h) into out\s98\tron_zip\tron_zip_test.exe.
# No game, no SDK: clang-cl from the dev shell, nlohmann/json from the SDK source tree's vendored copy.
# Run: out\s98\tron_zip\tron_zip_test.exe CACHE WORKDIR   (CACHE: a tron cache; WORKDIR: under out\, it gets a ~1x CACHE zip + copy)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$tp = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty'
if (-not (Test-Path "$tp\inja\third_party\include\nlohmann\json.hpp")) {
  # a git worktree (.claude\worktrees\<name>) has no thirdparty folder: the main checkout's
  $tp = Join-Path $repo '..\..\..\thirdparty\rexglue-sdk-src\thirdparty' | Resolve-Path | ForEach-Object Path
}
$out = Join-Path $repo 'out\s98\tron_zip'
New-Item -ItemType Directory -Force $out | Out-Null
& clang-cl /nologo /std:c++20 /EHsc /O2 /MT /W3 -Wno-deprecated-declarations /D_CRT_SECURE_NO_WARNINGS `
  "/I$tp\inja\third_party\include" (Join-Path $PSScriptRoot 'tron_zip_test.cpp') "/Fe$out\tron_zip_test.exe" "/Fo$out\\"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"built $out\tron_zip_test.exe"
