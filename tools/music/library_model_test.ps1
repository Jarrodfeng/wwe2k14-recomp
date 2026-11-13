# Builds and runs the phone launcher's song library model test (lane 297): tools\music\library_model_test.cpp with the real importer.
#   pwsh -File tools\music\library_model_test.ps1 [-NoMutation]
# 1. synthetic samples (py tools\audio\import_samples.py, needs ffmpeg), 2. libopus as C once, 3. the test, 4. the SAME test built with
# -DWWE_TEST_MUTATE_FOCUS_PLAYS (the known bug: keyboard / pad focus starts a preview) which must FAIL: that proves the test catches it.
# A stand-alone compile into out\l297\ (no game build, no run lock needed: nothing here starts the game).
param([switch]$NoMutation)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root\tools\env.ps1" | Out-Null
$out = "$root\out\l297"
$opus = "$root\third_party\audio\opus"
[void][System.IO.Directory]::CreateDirectory("$out\opus")
$samples = "$out\samples"
if (-not (Test-Path "$samples\manifest.txt")) { py "$root\tools\audio\import_samples.py" $samples | Out-Null }
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
    $variants = @(@{ Name = 'library_model_test'; Def = @() })
    if (-not $NoMutation) { $variants += @{ Name = 'library_model_test_mutant'; Def = @('/DWWE_TEST_MUTATE_FOCUS_PLAYS') } }
    foreach ($v in $variants) {
        & clang-cl /nologo /std:c++20 /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations @($v.Def) `
            "$root\tools\music\library_model_test.cpp" "$root\src\audio_import\audio_import.cpp" "$root\src\audio_import\lookup.cpp" @objs "/Fe:$($v.Name).exe" 2>&1 | Out-File "$out\build_$($v.Name).log"
        if ($LASTEXITCODE -ne 0) { Get-Content "$out\build_$($v.Name).log" | Select-String 'error' | Select-Object -First 20; exit 1 }
    }
} finally { Pop-Location }
"== the model test"
& "$out\library_model_test.exe" $samples "$out\work"
$code = $LASTEXITCODE
if (-not $NoMutation) {
    "== the same test with the planted bug (focus starts a preview): it must FAIL"
    & "$out\library_model_test_mutant.exe" $samples "$out\work_mutant"
    $m = $LASTEXITCODE
    if ($m -eq 0) { "MUTANT PASSED: the test does not catch the focus-plays bug"; exit 1 }
    "mutant failed as it should (exit $m)"
}
exit $code
