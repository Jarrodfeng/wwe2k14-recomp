# S111 BLOODMOD / BLOODMOD2: builds + runs out\weapons\floor_blood_test.exe (the permanent blood mod's splat store, src\renderer_scene\floor_blood_keep.h, the blood rows' identity, src\scene_live\blood_slot_id.h,
# and the plan it feeds), then every planted twin must FAIL: drop_expired (the store following the game's timer), match_rows (the 0.1 row tolerance for identity), ignore_row_pool (an old pool's row kept into
# the new scene), replay_as_own (a played-back record taken for its live particle's own); S111 BLOODMOD3: flip_angle (a held splat's rows turned the wrong way), draw_held (the plan draws a held row).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_floor_blood.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
$exe = Join-Path $out 'floor_blood_test.exe'
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\floor_blood_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'floor_blood_test build failed' }
Write-Host "built $exe"
& $exe
if ($LASTEXITCODE -ne 0) { throw 'floor_blood_test FAILED' }
foreach ($plant in @('drop_expired', 'match_rows', 'ignore_row_pool', 'replay_as_own', 'flip_angle', 'draw_held')) {
  & $exe --plant $plant | Out-Null
  if ($LASTEXITCODE -ne 1) { throw "floor_blood_test --plant $plant did not fail: the planted twin was not caught" }
  Write-Host "floor_blood_test --plant ${plant}: the planted twin fails, as it must"
}
