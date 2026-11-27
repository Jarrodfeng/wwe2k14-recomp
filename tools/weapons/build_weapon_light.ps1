# Builds S111 WEAPONLIGHT's offline test: out\weapons\weapon_light_test.exe (the CWeapon light set pick, weapon_light_key.h CWeaponLightKey).
# Compile only: no SDK, no game, no run lock.   pwsh -NoProfile -File tools\weapons\build_weapon_light.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'out\weapons'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS')
& $clang @flags -fuse-ld=lld (Join-Path $repo 'tools\weapons\weapon_light_test.cpp') -o (Join-Path $out 'weapon_light_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'weapon_light_test build failed' }
Write-Host "built $(Join-Path $out 'weapon_light_test.exe')"
