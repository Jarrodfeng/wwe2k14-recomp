# S111 CHAMBERSHADOW: builds + runs out\weapons\stage_node_cast_test.exe (src\renderer_scene\stage_struct_nodes.h through weapon_plan.h CastsMapB / CastsPropsMap), then its planted twin
# (--plant node_blind: every own submit but the cage node's casts, the old rule) must FAIL; then builds + runs the units that share the rows: mapb_test, props_cast_test, stage_struct (selftest),
# ladder (selftest; S111 CHAMBERSHADOW review: its build passes the stb include now).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_stage_node_cast.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$exe = Join-Path $out 'stage_node_cast_test.exe'
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\stage_node_cast_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'stage_node_cast_test build failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'stage_node_cast_test FAILED' }
& $exe --plant node_blind | Out-Null
if ($LASTEXITCODE -ne 1) { throw 'stage_node_cast_test --plant node_blind did not fail: the planted twin (the old any-own-caller rule) was not caught' }
Write-Host 'stage_node_cast_test --plant node_blind: the planted twin fails, as it must'
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_mapb.ps1')
if ($LASTEXITCODE -ne 0) { throw 'mapb_test build failed' }
& (Join-Path $out 'mapb_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'mapb_test FAILED' }
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_props_cast.ps1')
if ($LASTEXITCODE -ne 0) { throw 'props_cast_test build failed' }
& (Join-Path $out 'props_cast_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'props_cast_test FAILED' }
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_stage_struct.ps1')
if ($LASTEXITCODE -ne 0) { throw 'stage_struct_test build failed' }
& py -I (Join-Path $repo 'tools\weapons\stage_struct_selftest.py') --root $repo
if ($LASTEXITCODE -ne 0) { throw 'stage_struct selftest FAILED' }
& pwsh -NoProfile -File (Join-Path $repo 'tools\weapons\build_ladder.ps1')
if ($LASTEXITCODE -ne 0) { throw 'ladder_test build failed' }
& py -I (Join-Path $repo 'tools\weapons\ladder_selftest.py') --root $repo
if ($LASTEXITCODE -ne 0) { throw 'ladder selftest FAILED' }
Write-Host 'mapb_test + props_cast_test + stage_struct + ladder: pass'
