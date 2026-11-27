# S111 EQUIV piece B: build the equivalence tester's test DLL (equiv\equiv_tests.cpp; docs\research\s111\EQUIV0.md).
#   pwsh -NoProfile -File tools\equiv\build_equiv.ps1 [-Preset win-amd64-release]   the CMake target wwe2k14_equiv_tests (ninja of that one
#        target, next to the preset's wwe2k14.exe; the exe is never touched: the target has no dependency on it)
#   pwsh -NoProfile -File tools\equiv\build_equiv.ps1 -Standalone   the DLL and the coverage tier's entry unit (tools\equiv\equiv_cov_unit.cpp)
#        with clang++ alone, into out\tools\equiv\ of THIS checkout (no build folder, no game, no SDK); the DLL runs in a game exe's headless
#        tester (tools\equiv\equiv.py run). S111 EQUIVTIER: the fake host (hand transcriptions of the listings) is retired.
# No run lock: one small DLL that no game holds open (the build_probes.ps1 shape). Never next to a locked build folder.
param([string]$Preset = 'win-amd64-release', [switch]$Standalone)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$t0 = Get-Date
if ($Standalone) {
  $out = Join-Path $repo 'out\tools\equiv'
  New-Item -ItemType Directory -Force $out | Out-Null
  # the asset headers' stb include: the worktree has no thirdparty folder, so fall back to the main checkout's (as build_arena_levels_unit.ps1)
  $stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
  if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path 'C:\Users\Jarrod\wwe2k14-recomp' 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
  # -O0: equiv_tests.cpp compiles in ~1.0 s (-O1 2.6 s); -ffp-contract=off: no a x b + c is fused unless a model asks (std::fma), as the
  # PowerPC rounds each fmuls / fadds
  $common = @('-O0', '-g', '-ffp-contract=off', '-std=c++20', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS',
              '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', "-I$repo\src", "-I$repo\equiv", "-I$stb")
  # Incremental like the CMake target: the precompiled header (equiv_pch.h: the asset headers) and stb's unit are rebuilt only when a file
  # they read (their clang -MD lists) is newer than them; a change to equiv_tests.cpp recompiles that unit and links.
  function Test-Stale([string]$obj) {
    $dep = "$obj.d"
    if (-not (Test-Path $obj) -or -not (Test-Path $dep)) { return $true }
    $t = (Get-Item $obj).LastWriteTimeUtc
    if ((Get-Item $PSCommandPath).LastWriteTimeUtc -gt $t) { return $true }   # the flags above changed
    $files = ((Get-Content $dep -Raw) -replace '\\\r?\n', ' ' -replace '^[^:]+:\s', '') -split '(?<!\\)\s+' | Where-Object { $_ } | ForEach-Object { $_ -replace '\\ ', ' ' }
    foreach ($f in $files) { if ((Test-Path $f) -and (Get-Item $f).LastWriteTimeUtc -gt $t) { return $true } }
    return $false
  }
  $pch = Join-Path $out 'equiv_pch.pch'
  if (Test-Stale $pch) {
    & clang++ @common -x c++-header (Join-Path $repo 'equiv\equiv_pch.h') -MD -MF "$pch.d" -o $pch
    if ($LASTEXITCODE -ne 0) { "build_equiv: the precompiled header failed ($LASTEXITCODE)"; exit 1 }
  }
  $stbo = Join-Path $out 'equiv_stb.obj'
  if (Test-Stale $stbo) {
    & clang++ @common -c (Join-Path $repo 'equiv\equiv_stb.cpp') -MD -MF "$stbo.d" -o $stbo
    if ($LASTEXITCODE -ne 0) { "build_equiv: stb's unit failed ($LASTEXITCODE)"; exit 1 }
  }
  $t1 = Get-Date
  $testo = Join-Path $out 'equiv_tests.obj'
  & clang++ @common -include-pch $pch -c (Join-Path $repo 'equiv\equiv_tests.cpp') -o $testo
  if ($LASTEXITCODE -ne 0) { "build_equiv: equiv_tests.cpp failed ($LASTEXITCODE)"; exit 1 }
  $dll = Join-Path $out 'wwe2k14_equiv_tests.dll'
  & clang++ -shared $testo $stbo -o $dll
  if ($LASTEXITCODE -ne 0) { "build_equiv: the DLL link failed ($LASTEXITCODE)"; exit 1 }
  $t2 = Get-Date
  # the coverage tier's entry decision (src\equiv\equiv_cov.h) on its own: tools\equiv\equiv_selftest.py runs it (check cov.entry)
  $covu = Join-Path $out 'equiv_cov_unit.exe'
  if (Test-Stale $covu) {
    & clang++ @common (Join-Path $repo 'tools\equiv\equiv_cov_unit.cpp') -MD -MF "$covu.d" -o $covu
    if ($LASTEXITCODE -ne 0) { "build_equiv: the coverage-tier entry unit failed ($LASTEXITCODE)"; exit 1 }
  }
  "build_equiv: -Standalone pch + stb {0:N1} s, the DLL (equiv_tests.cpp + link) {1:N1} s, entry unit {2:N1} s; {3}; {4}" -f ($t1 - $t0).TotalSeconds,
    ($t2 - $t1).TotalSeconds, ((Get-Date) - $t2).TotalSeconds, $dll, $covu
  exit 0
}
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$dir = "$R\out\build\$Preset"
. "$R\tools\runlock.ps1"
$st = Get-RunLockState
if ($st.LockAlive -and "$($st.Lock.name)" -match 'build') { "ABORT: a build holds the run lock ($($st.Lock.name)); run this again when it is done"; exit 3 }
if (-not (Test-Path "$dir\build.ninja")) { "ABORT: $dir has no build.ninja (configure that preset first: tools\build.ps1 -Preset $Preset)"; exit 2 }
$log = & ninja -C $dir wwe2k14_equiv_tests 2>&1
$code = $LASTEXITCODE
$log | Select-Object -Last 6
$f = Get-ChildItem "$dir\wwe2k14_equiv_tests.dll" -ErrorAction SilentlyContinue
"build_equiv: exit $code in {0:N1} s; {1}" -f ((Get-Date) - $t0).TotalSeconds, $(if ($f) { "$($f.FullName) ($($f.LastWriteTime.ToString('HH:mm:ss')))" } else { 'no DLL' })
exit $code
