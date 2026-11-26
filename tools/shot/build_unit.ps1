# S111 SHOTDIET: builds the lossless WebP + cut-trigger unit (tools\shot\shot_diet_unit.cpp) -> out\tools\shot_diet_unit.exe. Compile only; no game, no run lock.
# The vendored libwebp (third_party\libwebp) is built here as C into one static library, the same file set as third_party\libwebp\libwebp.cmake.
# Usage: pwsh -NoProfile -File tools\shot\build_unit.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$objDir = Join-Path $outDir 'shot_diet_obj'
New-Item -ItemType Directory -Force $outDir, $objDir | Out-Null
$lw = Join-Path $repo 'third_party\libwebp'
$srcs = @('src\dec', 'src\dsp', 'src\enc', 'src\utils', 'sharpyuv') | ForEach-Object { Get-ChildItem (Join-Path $lw $_) -Filter *.c } | ForEach-Object FullName
$lib = Join-Path $objDir 'libwebp_shot.lib'
if (-not (Test-Path $lib) -or ((Get-Item $lib).LastWriteTime -lt (Get-Item (Join-Path $lw 'PIN.txt')).LastWriteTime)) {
  $objs = @()
  foreach ($s in $srcs) {
    $o = Join-Path $objDir (([IO.Path]::GetFileNameWithoutExtension($s)) + '_' + ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($s))) + '.o')
    # clang defines _MSC_VER here, so cpu.h turns SSE4.1 / AVX2 on everywhere: those two file families get the flags (same as libwebp.cmake)
    [string[]]$isa = if ($s -like '*_avx2.c') { @('-mavx2', '-msse4.1') } elseif ($s -like '*_sse41.c') { @('-msse4.1') } else { @() }
    & clang -O2 -w @isa -I $lw -c $s -o $o
    if ($LASTEXITCODE -ne 0) { throw "clang failed on $s" }
    $objs += $o
  }
  & llvm-ar rcs $lib @objs
  if ($LASTEXITCODE -ne 0) { throw "llvm-ar failed" }
}
$exe = Join-Path $outDir 'shot_diet_unit.exe'
& clang++ -O2 -std=c++20 -Wall -Wno-unused-command-line-argument -I $lw (Join-Path $repo 'tools\shot\shot_diet_unit.cpp') $lib -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
