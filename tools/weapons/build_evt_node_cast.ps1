# S111 WATRSHADOW: builds + runs out\weapons\evt_node_cast_test.exe (src\renderer_scene\event_prop_nodes.h through weapon_plan.h CastsMapB / CastsPropsMap / Plan), then its planted twin
# (--plant entry_blind: every entry taken for the own node's model, the old rule) must FAIL; then builds + runs the two filter units that share the rule (mapb_test, props_cast_test), unchanged.
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_evt_node_cast.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$exe = Join-Path $out 'evt_node_cast_test.exe'
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\evt_node_cast_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'evt_node_cast_test build failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'evt_node_cast_test FAILED' }
& $exe --plant entry_blind | Out-Null
if ($LASTEXITCODE -ne 1) { throw 'evt_node_cast_test --plant entry_blind did not fail: the planted twin (the old per-id rule) was not caught' }
Write-Host 'evt_node_cast_test --plant entry_blind: the planted twin fails, as it must'
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_mapb.ps1')
if ($LASTEXITCODE -ne 0) { throw 'mapb_test build failed' }
& (Join-Path $out 'mapb_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'mapb_test FAILED' }
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_props_cast.ps1')
if ($LASTEXITCODE -ne 0) { throw 'props_cast_test build failed' }
& (Join-Path $out 'props_cast_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'props_cast_test FAILED' }
Write-Host 'mapb_test + props_cast_test: pass unchanged'
