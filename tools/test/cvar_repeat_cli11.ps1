# S102 cvar-parse: builds and runs tools\test\cvar_repeat_cli11.cpp (fixed + planted). Uses the SDK's CLI11 headers. 0 game runs.
$root = Split-Path (Split-Path $PSScriptRoot)
$inc = "C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src\thirdparty\cli11\include"
$out = Join-Path $root "out\cvar_repeat"; New-Item -ItemType Directory -Force $out | Out-Null
. "$root\tools\env.ps1" | Out-Null
clang++ -std=c++20 -I $inc "$PSScriptRoot\cvar_repeat_cli11.cpp" -o "$out\t.exe"
if ($LASTEXITCODE) { exit 2 }
& "$out\t.exe"; $a = $LASTEXITCODE
& "$out\t.exe" --planted; $b = $LASTEXITCODE
"fixed exit=$a planted-reproduces-bug exit=$b"
exit ([int]($a -ne 0 -or $b -ne 0))
