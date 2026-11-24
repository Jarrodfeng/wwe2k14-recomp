# S110 FEEDREC: builds and runs tools\tests\store_watch_test.cpp (host half: watch set, slow path, stores.tsv) and checks the
# guest half: its drift guards compile against stub SDK macros equal to the copies (must pass) and against a stub with one
# token drifted (must FAIL), and the 9 overrides' fast path runs. 0 game runs.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
# env.ps1 sets $ErrorActionPreference = 'Stop' in this scope: with it, the stderr of a native command run as `& clang++ ... 2>&1`
# (the drift step MUST fail to compile, so clang writes errors) raises a terminating error and kills the script before it can
# read $LASTEXITCODE. Native exit codes are checked by hand below, so stderr text must not be an error.
$ErrorActionPreference = 'Continue'
if (-not $Out) { $Out = Join-Path $root 'out\store_watch_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$src = "$PSScriptRoot\store_watch_test.cpp"
$bad = 0
$exe = Join-Path $Out 'store_watch_test.exe'
& clang++ -std=c++23 -O1 -Wno-deprecated-declarations "-I$root\src" $src -o $exe
if ($LASTEXITCODE -ne 0) { "FAIL host unit does not compile"; exit 2 }
Push-Location $Out
& $exe; if ($LASTEXITCODE -ne 0) { "FAIL host unit"; $bad++ }
Pop-Location
$gexe = Join-Path $Out 'store_watch_guard.exe'
& clang++ -std=c++23 -O1 -DGUARD_STUB "-I$root\src" $src -o $gexe 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { "FAIL guard: the guest half does not compile against the matching stubs"; $bad++ } else {
  "ok   guard passes on matching macro bodies"
  & $gexe; if ($LASTEXITCODE -ne 0) { "FAIL guard unit: the overrides' fast path"; $bad++ }
}
# an override must not read back the volatile word it stores (a statement expression ending in the assignment uses its
# value: clang then emits a volatile load after every store, and warns at every store site of the generated code)
$msg = & clang++ -std=c++23 -fsyntax-only -DGUARD_STUB -Werror=deprecated-volatile "-I$root\src" $src 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { "FAIL guard: an override reads back its volatile store: $msg"; $bad++ } else { "ok   no override reads back the word it stores" }
$msg = & clang++ -std=c++23 -fsyntax-only -DGUARD_STUB -DDRIFT "-I$root\src" $src 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -or $msg -notmatch 'no longer matches') { "FAIL guard: a drifted SDK body was not caught"; $bad++ } else { "ok   guard fails loudly on a drifted body" }
if ($bad) { "store_watch_test: $bad problem(s)"; exit 1 }
"store_watch_test: 0 problems"; exit 0
