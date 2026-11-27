# S111 UNIFY2 (docs\research\s111\UNIFY2.md): the stock materials' texture slots are byte-identical before and after the one sampler table. Builds tools\scene\material_slots_dump.cpp twice: against the
# commit before the lane (the parent of the commit that added src\assets\material_samplers.h, extracted read-only with git archive) and against this tree. Runs both through the production LoadWrestler on
# ch100 / 103 / 107 / 161 / 177 (every attire, up to 16) and compares the two dumps byte for byte. Compile only + the player's files read-only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\scene\build_material_samplers_check.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
New-Item -ItemType Directory -Force $outDir | Out-Null
# A worktree has no thirdparty / game_data folder: fall back to the main checkout's (three levels up: out\wt\<name> or .claude\worktrees\<name>).
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path $main 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
$game = Join-Path $repo 'game_data\extracted'
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { $game = Join-Path $main 'game_data\extracted' }
if (-not (Test-Path (Join-Path $game 'pac\ch\ch161.pac'))) { Write-Host "SKIP: no pac\ch\ch161.pac under $game"; exit 0 }
$added = (& git -C $repo log -1 --diff-filter=A --format=%H -- src/assets/material_samplers.h)
if (-not $added) { throw 'no commit adds src/assets/material_samplers.h (the lane is not committed)' }
$baseDir = Join-Path $outDir 'unify2_base'
if (Test-Path $baseDir) { Remove-Item -Recurse -Force $baseDir }
New-Item -ItemType Directory -Force $baseDir | Out-Null
$tar = Join-Path $outDir 'unify2_base.tar'
& git -C $repo archive --format=tar -o $tar "$added^" src
if ($LASTEXITCODE -ne 0) { throw 'git archive of the commit before the lane failed' }
& (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $tar -C $baseDir   # Windows' own bsdtar: the Git usr\bin tar first on PATH reads 'C:' as a remote host
if ($LASTEXITCODE -ne 0) { throw 'tar -x of the commit before the lane failed' }
function Build-Dump([string]$tree, [string]$exe) {
  $spv = @(if ($tree -ne $repo) { Join-Path $tree 'src\renderer\shaders\spv\fx_spv_data.cpp' })   # S111 TOOLLINK-D: only the OLD tree's registry holds kFx_ addresses (its own data unit); this tree's units read no words here
  & clang++ -O1 -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wno-unused-command-line-argument "-I$stb" (Join-Path $PSScriptRoot 'material_slots_dump.cpp') (Join-Path $tree 'src\renderer_scene\char_effects.cpp') (Join-Path $tree 'src\renderer_scene\draw_lists.cpp') (Join-Path $tree 'src\renderer_scene\record_io.cpp') @spv (Join-Path $tree 'src\renderer\problem.cpp') -I (Join-Path $tree 'src') -o $exe
  if ($LASTEXITCODE -ne 0) { throw "material_slots_dump compile failed ($tree)" }
}
$before = Join-Path $outDir 'material_slots_dump_before.exe'
$after = Join-Path $outDir 'material_slots_dump_after.exe'
Build-Dump $baseDir $before
Build-Dump $repo $after
Write-Host "built $before (the tree of $added^) and $after (this tree)"
$a = Join-Path $outDir 'unify2_slots_before.txt'
$b = Join-Path $outDir 'unify2_slots_after.txt'
& $before --game $game | Set-Content -Encoding utf8 $a
if ($LASTEXITCODE -ne 0) { throw 'the before dump failed' }
& $after --game $game | Set-Content -Encoding utf8 $b
if ($LASTEXITCODE -ne 0) { throw 'the after dump failed' }
$x = Get-Content $a
$y = Get-Content $b
Write-Host ("before: " + $x[-1] + "; after: " + $y[-1])
$diff = Compare-Object $x $y -SyncWindow 0
if ($diff) {
  $diff | Select-Object -First 10 | ForEach-Object { Write-Host ($_.SideIndicator + ' ' + $_.InputObject) }
  throw "UNIFY2 FAIL: the stock materials' texture slots differ before / after ($(@($diff).Count) lines)"
}
Write-Host "UNIFY2 PASS: every stock material's texture slots are identical before and after ($($x.Count - 1) material lines, $a = $b)"
