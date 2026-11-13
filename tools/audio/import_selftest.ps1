# Builds and runs the song importer's offline proof (lane 283): tools\audio\import_selftest.cpp, no game, no game files.
#   pwsh -File tools\audio\import_selftest.ps1 [-Samples DIR]     (DIR from: py tools\audio\import_samples.py DIR)
# libopus (third_party\audio\opus, the sources listed in sources.txt) is compiled as C into out\audio_import\opus\ once.
# A stand-alone test compile into its own out folder (not the game build, no shared outputs): it does not take the run lock,
# so a running game never makes it wait. Output: out\audio_import\import_selftest.exe and build.log.
param([string]$Samples = '', [switch]$Mem)
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
            & clang-cl @copts "$opus\$s" "/Fo$o" 2>&1 | Out-File "$out\build.log" -Append
            if ($LASTEXITCODE -ne 0) { Get-Content "$out\build.log" | Select-Object -Last 20; exit 1 }
        }
    }
    & clang-cl /nologo /std:c++20 /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations `
        "$root\tools\audio\import_selftest.cpp" "$root\src\audio_import\audio_import.cpp" "$root\src\audio_import\lookup.cpp" @objs /Fe:import_selftest.exe 2>&1 | Out-File "$out\build.log"
    $code = $LASTEXITCODE
} finally { Pop-Location }
if ($code -ne 0) { Get-Content "$out\build.log" | Select-String 'error' | Select-Object -First 20; exit $code }
$exe = Join-Path $out 'import_selftest.exe'
$env:LOOKUP_CASE = Join-Path $out 'lookup_case'
if ($Mem) { & $exe --mem } elseif ($Samples) { & $exe $Samples } else { & $exe }
$rc = $LASTEXITCODE
# The game's view: a binary that links lookup.cpp ONLY (no decoder, no Import), run on the CJK-named mods folder the selftest prepared.
Push-Location $out
try {
    & clang-cl /nologo /std:c++20 /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS "$root\tools\audio\lookup_only_test.cpp" "$root\src\audio_import\lookup.cpp" /Fe:lookup_only_test.exe 2>&1 | Out-File "$out\build_lookup.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$out\build_lookup.log" | Select-Object -First 20; exit 1 }
} finally { Pop-Location }
if (-not $Mem) { & (Join-Path $out 'lookup_only_test.exe') $env:LOOKUP_CASE; if ($LASTEXITCODE -ne 0) { $rc = 1 } }
exit $rc
