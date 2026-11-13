# Builds tools\audio\music_live_import.exe (lane 303): the importer CLI the combined custom-music live check uses to make its cache.
#   pwsh -File tools\audio\music_live_import_build.ps1          -> out\audio_import\music_live_import.exe
# The same stand-alone compile as import_selftest.ps1 (libopus compiled as C into out\audio_import\opus\ once). Not the game build:
# no run lock, nothing shared, no game started. Re-run after the importer changes (the exe links audio_import.cpp + lookup.cpp).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root\tools\env.ps1" | Out-Null
$out = "$root\out\audio_import"
$opus = "$root\third_party\audio\opus"
[void][System.IO.Directory]::CreateDirectory("$out\opus")
Push-Location $out
try {
    $objs = @()
    $copts = @('/nologo', '/TC', '/c', '/O2', '/MT', '-w', '/DOPUS_BUILD', '/DUSE_ALLOCA', '/DPACKAGE_VERSION="1.5.2"',
               "/I$opus\include", "/I$opus\celt", "/I$opus\silk", "/I$opus\silk\float", "/I$opus")
    foreach ($s in Get-Content "$opus\sources.txt") {
        $o = "$out\opus\" + ($s -replace '[/\\]', '_') + '.obj'
        $objs += $o
        if (-not (Test-Path $o)) {
            & clang-cl @copts "$opus\$s" "/Fo$o" 2>&1 | Out-File "$out\live_build.log" -Append
            if ($LASTEXITCODE -ne 0) { Get-Content "$out\live_build.log" | Select-Object -Last 20; exit 1 }
        }
    }
    & clang-cl /nologo /std:c++20 /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations `
        "$root\tools\audio\music_live_import.cpp" "$root\src\audio_import\audio_import.cpp" "$root\src\audio_import\lookup.cpp" @objs /Fe:music_live_import.exe 2>&1 | Out-File "$out\live_build.log" -Append
    $code = $LASTEXITCODE
} finally { Pop-Location }
if ($code -ne 0) { Get-Content "$out\live_build.log" | Select-String 'error' | Select-Object -First 20; exit $code }
Write-Host "built $out\music_live_import.exe"
