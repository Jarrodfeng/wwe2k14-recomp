# S110 OBSERVE: builds and runs tools\tests\icall_observe_test.cpp (host half: pair set + writer, planted second target) and
# checks the guest half's drift guard: compiles against a stub SDK macro equal to the copy (must pass) and against a stub with
# one token drifted (must FAIL). 0 game runs.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\icall_observe_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$src = "$PSScriptRoot\icall_observe_test.cpp"
$bad = 0
$exe = Join-Path $Out 'icall_observe_test.exe'
& clang++ -std=c++20 -O1 "-I$root\src" $src -o $exe
if ($LASTEXITCODE -ne 0) { "FAIL host unit does not compile"; exit 2 }
& $exe; if ($LASTEXITCODE -ne 0) { "FAIL host unit"; $bad++ }
$gexe = Join-Path $Out 'icall_observe_guard.exe'
& clang++ -std=c++20 -O1 -DGUARD_STUB "-I$root\src" $src -o $gexe 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { "FAIL guard: the guest half does not compile against the matching stub"; $bad++ } else {
  "ok   guard passes on a matching macro body"
  & $gexe; if ($LASTEXITCODE -ne 0) { "FAIL guard unit: the override's fast path (recording off / on)"; $bad++ }
}
$msg = & clang++ -std=c++20 -fsyntax-only -DGUARD_STUB -DDRIFT "-I$root\src" $src 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -or $msg -notmatch 'no longer matches') { "FAIL guard: a drifted SDK body was not caught"; $bad++ } else { "ok   guard fails loudly on a drifted body" }
if ($bad) { "icall_observe_test: $bad problem(s)"; exit 1 }
"icall_observe_test: 0 problems"; exit 0
