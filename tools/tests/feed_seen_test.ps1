# S110 FEEDREC: builds and runs tools\tests\feed_seen_test.cpp (the read recorder of src/feed_seen.h). 0 game runs.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\feed_seen_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'feed_seen_test.exe'
& clang++ -std=c++23 -O1 -Wno-deprecated-declarations "-I$root\src" "$PSScriptRoot\feed_seen_test.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { "FAIL does not compile"; exit 2 }
Push-Location $Out
& $exe; $rc = $LASTEXITCODE
Pop-Location
# the release shape: no coverage define -> the plain pointer type and empty macros (the header must compile alone)
$rel = Join-Path $Out 'release_shape.cpp'
Set-Content -Path $rel -Value '#include "feed_seen.h"
static_assert(sizeof(wwe2k14::feedseen::ReadFnT) == sizeof(void*), "release reader is the plain pointer");
static bool H(wwe2k14::feedseen::ReadFnT r, unsigned a WWE_FEED_LOC_PARAM) { char c; return r(a, &c, 1 WWE_FEED_LOC_ARG); }
int main() { return H(nullptr, 0) ? 1 : 0; }'
& clang++ -std=c++23 -fsyntax-only "-I$root\src" $rel
if ($LASTEXITCODE -ne 0) { "FAIL release shape"; $rc = 1 } else { "ok   release shape: plain pointer, empty macros" }
# the pure scene readers in the coverage shape: they compile with the ReadFnT struct, and a shadowing lambda keeps the caller's line
$sexe = Join-Path $Out 'feed_seen_scene_test.exe'
& clang++ -std=c++23 -O1 -Wno-deprecated-declarations "-I$root\src" "$PSScriptRoot\feed_seen_scene_test.cpp" -o $sexe
if ($LASTEXITCODE -ne 0) { "FAIL the scene readers do not compile in the coverage shape"; $rc = 1 } else {
  Push-Location $Out
  & $sexe; if ($LASTEXITCODE -ne 0) { $rc = 1 }
  Pop-Location
}
if ($rc) { "feed_seen_test: problems"; exit 1 }
exit 0
