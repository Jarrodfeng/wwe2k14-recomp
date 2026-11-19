# Builds lane E3's unit proofs (Session 103): out\tools\e3_unit.exe = the fitted wrestler shadow camera (src\renderer_scene\shadow_fit.h), pure maths, headers only;
# out\tools\e3_key_check.exe = the key-light-from-the-record route checked against a capture's own caster states (links the record reader).
# Compile only: no game, no run lock, no SDK.   pwsh -NoProfile -File tools\scene\build_e3_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'e3_unit.cpp') -o (Join-Path $outDir 'e3_unit.exe')
if ($LASTEXITCODE -ne 0) { throw 'e3_unit compile failed' }
Write-Host "built $(Join-Path $outDir 'e3_unit.exe')"
& clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wno-unused-command-line-argument (Join-Path $PSScriptRoot 'e3_key_check.cpp') (Join-Path $repo 'src\renderer_scene\record_io.cpp') -o (Join-Path $outDir 'e3_key_check.exe')
if ($LASTEXITCODE -ne 0) { throw 'e3_key_check compile failed' }
Write-Host "built $(Join-Path $outDir 'e3_key_check.exe')"
