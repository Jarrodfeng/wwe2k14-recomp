# Lane 286 (custom music on the phone): music_core.h + music_mods.h + the music arguments, on planted folders. Host clang.
#   pwsh -File tools\android\cpu_semantics\run_music_test.ps1   (run from the repo / worktree root; builds under out\android\music_test)
$ErrorActionPreference = 'Stop'
. C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1 | Out-Null
$R = (Get-Location).Path
$json = 'C:/Users/Jarrod/wwe2k14-recomp/thirdparty/rexglue-sdk-src/thirdparty/inja/third_party/include'
$exe = Join-Path $R 'out\android\music_test.exe'
New-Item -ItemType Directory -Force (Split-Path $exe) | Out-Null
& clang++ -w -std=gnu++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN "-I$json" (Join-Path $PSScriptRoot 'music_test.cpp') -o $exe
if ($LASTEXITCODE) { throw 'build failed' }
& $exe
exit $LASTEXITCODE
