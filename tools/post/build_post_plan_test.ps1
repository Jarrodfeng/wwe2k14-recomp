# Builds out\tools\post_plan_test.exe (Session 101, lane PS): the unit test of the post plan (src\renderer_scene\post_plan.cpp) against the census numbers.
# Also compiles src\renderer_scene\post_pass.cpp to an object as a compile check of the marked lane PS lines (not linked). Compile only: no game, no run lock, no SDK.
#   pwsh -NoProfile -File tools\post\build_post_plan_test.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
. (Join-Path $repo 'tools\env.ps1') | Out-Null
$outDir = Join-Path $repo 'out\tools'
$obj = Join-Path $outDir 'post_plan_test_obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force -Confirm:$false $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$defs = @('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-DRDR_DEV')
$inc = @('-I', (Join-Path $repo 'src'))
$srcs = @((Join-Path $PSScriptRoot 'post_plan_test.cpp'), (Join-Path $repo 'src\renderer_scene\post_plan.cpp'))
$check = @((Join-Path $repo 'src\renderer_scene\post_pass.cpp'))
$all = $srcs + $check
$newestHdr = (Get-ChildItem (Join-Path $repo 'src\renderer_scene'), (Join-Path $repo 'src\renderer_adapter'), (Join-Path $repo 'src\renderer') -Recurse -Include *.h, *.hpp | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$repoLen = $repo.Path.Length + 1
$fails = $all | ForEach-Object -ThrottleLimit 4 -Parallel {
  $name = ($_.Substring($using:repoLen) -replace '[\\/:]', '_') + '.o'
  $o = Join-Path $using:obj $name
  if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Item $_).LastWriteTimeUtc -and (Get-Item $o).LastWriteTimeUtc -gt $using:newestHdr) { return }
  $log = & clang++ -O1 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -c @using:defs -Wno-unused-command-line-argument @using:inc $_ -o $o 2>&1
  if ($LASTEXITCODE -ne 0) { "FAILED $_`n" + ($log -join "`n") }
}
$bad = @($fails | Where-Object { $_ -match '^FAILED' })
$bad | ForEach-Object { Write-Host $_ }
if ($bad.Count) { throw "clang++ failed on $($bad.Count) file(s)" }
$objs = @($srcs | ForEach-Object { Join-Path $obj (($_.Substring($repoLen) -replace '[\\/:]', '_') + '.o') })
& clang++ -fuse-ld=lld -g @objs -o (Join-Path $outDir 'post_plan_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'post_plan_test link failed' }
Write-Host "built $(Join-Path $outDir 'post_plan_test.exe')"
