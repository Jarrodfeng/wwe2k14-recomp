# S111 VTXEXTENT: builds tools\scene\vtxextent_ours.cpp -> out\tools\vtxextent_ours[_NAME].exe (our loader's per-mesh vertex extent for tools\scene\vtxextent.py). No game, no run lock.
#   pwsh -NoProfile -File tools\scene\build_vtxextent_ours.ps1                    this checkout's src\assets
#   pwsh -NoProfile -File tools\scene\build_vtxextent_ours.ps1 -Root C:\...\land_s111-matlive -Name matlive   another checkout's src\assets (the cpp stays this one's)
param([string]$Root = '', [string]$Name = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
if (-not $Root) { $Root = $repo }
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir ($(if ($Name) { "vtxextent_ours_$Name.exe" } else { 'vtxextent_ours.exe' }))
# A worktree has no thirdparty folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $Root 'thirdparty\rexglue-sdk-src\thirdparty\stb'
foreach ($base in @($repo, $main)) { if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $base 'thirdparty\rexglue-sdk-src\thirdparty\stb' } }
$assets = Join-Path $Root 'src\assets'
& clang++ -O1 -std=c++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -w "-I$stb" "-I$assets" (Join-Path $repo 'tools\scene\vtxextent_ours.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang++ failed ($LASTEXITCODE)" }
Write-Host "built $exe"
