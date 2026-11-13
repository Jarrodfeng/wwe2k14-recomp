# Builds fview (the frame viewer, Session 101 lane 2): out\renderer\<dev|release>\fview.exe. No SDK, no game, no run lock. Only changed files are recompiled.
#   pwsh -NoProfile -File tools\fview\build.ps1 [-Release] [-Clean]
# It compiles the renderer's own sources the way tools\renderer\build.ps1 does, MINUS src\renderer\player\ (rplay and its window), plus src\fview\*.cpp and
# Dear ImGui (core + win32 + dx11 backends, from the SDK's copy: the main checkout's when this is a worktree). Its objects live in out\renderer\<kind>\fview_obj
# so tools\renderer\build.ps1's output is never touched. The shader headers (src\renderer\shaders\spv) are used as they are: run tools\renderer\compile_shaders.py
# first when an effect changed.
param([switch]$Release, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$kind = if ($Release) { 'release' } else { 'dev' }
$out = Join-Path $repo "out\renderer\$kind"
$objects = Join-Path $out 'fview_obj'
if ($Clean -and (Test-Path $objects)) { Remove-Item -Recurse -Force -Confirm:$false $objects }
New-Item -ItemType Directory -Force $objects | Out-Null
# S101 lane 239: two builds at once (the wrestler gate lanes) collided on fview_obj and on the link; one named mutex per exe serialises them (released when this process ends)
. (Join-Path $repo "tools/wrestler/atomic_build.ps1")
$null = Enter-ExeBuild (Join-Path $out 'fview.exe')

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$imgui = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\imgui'
if (-not (Test-Path (Join-Path $imgui 'imgui.h'))) {
  $imgui = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\imgui'
}
if (-not (Test-Path (Join-Path $imgui 'imgui.h'))) { throw "Dear ImGui not found ($imgui)" }

$stb = Join-Path (Split-Path $imgui -Parent) 'stb'   # S101 lane 35: stb_image.h (crowd_body.cpp's declarations)
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', "-I$sdk\Include", "-I$imgui", "-I$stb", "-I$(Join-Path (Split-Path $stb -Parent) 'xxHash')", '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations', '-MMD')   # (S101 lane 70: xxhash.h for ring_hash.h)
if ($Release) { $flags += @('-O2', '-DNDEBUG') } else { $flags += @('-O1', '-DRDR_DEV') }
$imguiFlags = @('-std=c++20', '-g', '-O1', "-I$imgui", '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-MMD')

$renderer = @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.FullName -notmatch '\\src\\renderer\\player\\' -and $_.Name -notmatch '_test\.cpp$' } | Select-Object -ExpandProperty FullName)
$fview = @(Get-ChildItem (Join-Path $repo 'src\fview') -Filter '*.cpp' | Select-Object -ExpandProperty FullName)
# The sources session.cpp needs beyond src\renderer and src\fview (shader reload, the crowd drawer, the record reader, the arena bodies): one shared list,
# also read by tools\post\build_proof.ps1.
$fview += @(Get-Content (Join-Path $PSScriptRoot 'session_deps.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { Join-Path $repo $_.Trim() })
$imguiSources = @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends\imgui_impl_win32.cpp', 'backends\imgui_impl_dx11.cpp') | ForEach-Object { Join-Path $imgui $_ }
$jobs = @()
$jobs += $renderer | ForEach-Object { [pscustomobject]@{ Source = $_; Flags = $flags } }
$jobs += $fview | ForEach-Object { [pscustomobject]@{ Source = $_; Flags = $flags } }
$jobs += $imguiSources | ForEach-Object { [pscustomobject]@{ Source = $_; Flags = $imguiFlags } }

$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$jobs | ForEach-Object -ThrottleLimit 12 -Parallel {
  $source = $_.Source
  $flagList = $_.Flags
  $object = Join-Path $using:objects (($source -replace '[\\/:]', '_') + '.o')
  $deps = [IO.Path]::ChangeExtension($object, '.d')
  $stale = -not (Test-Path $object)
  if (-not $stale -and (Test-Path $deps)) {
    $objectTime = (Get-Item $object).LastWriteTimeUtc
    $list = (Get-Content -Raw $deps) -replace '\\\r?\n', ' '
    $list = $list.Substring($list.IndexOf(': ') + 2)
    foreach ($item in ($list -split '(?<!\\)\s+')) {
      $path = $item -replace '\\ ', ' '
      if ($path -and (Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).LastWriteTimeUtc -gt $objectTime) { $stale = $true; break }
    }
  }
  if ($stale) {
    $text = & $using:clang @flagList -c $source -o $object 2>&1
    if ($LASTEXITCODE -ne 0) { ($using:failed).Add("$source`n$($text -join "`n")") } elseif ($text) { Write-Host ($text -join "`n") }
  }
}
if ($failed.Count) { $failed | ForEach-Object { Write-Host $_ }; throw "$($failed.Count) file(s) failed to compile" }

$exe = Join-Path $out 'fview.exe'
# Link only the objects of the sources compiled above (a source that was renamed or removed leaves an old .o behind; it must not be linked).
$objectFiles = @($jobs | ForEach-Object { Join-Path $objects (($_.Source -replace '[\\/:]', '_') + '.o') })
$missingObjects = @($objectFiles | Where-Object { -not (Test-Path $_) })
if ($missingObjects.Count) { throw "missing objects: $($missingObjects -join ', ')" }
# S101 lane 163: no object is newer than the exe -> nothing to link. Relinking an unchanged tree failed with 'permission denied' whenever another
# process (a gate lane, a viewer) had the exe open.
if ((Test-Path $exe) -and -not @($objectFiles | Where-Object { (Get-Item $_).LastWriteTimeUtc -gt (Get-Item $exe).LastWriteTimeUtc }).Count) { Write-Host "up to date $exe"; exit 0 }
$tmpExe = Join-Path $out ("fview.tmp$PID.exe")   # lane 239: link beside, then rename into place (a running fview.exe cannot be overwritten, only renamed)
& $clang -fuse-ld=lld -g @objectFiles -luser32 -lgdi32 -ld3d11 -ldxgi -ldwmapi -lole32 -luuid -lshell32 -o $tmpExe
if ($LASTEXITCODE -ne 0) { Remove-Item (Join-Path $out "fview.tmp$PID.*") -Force -ErrorAction SilentlyContinue; throw 'link failed' }
Publish-Exe $tmpExe $exe
Remove-Item (Join-Path $out "fview.tmp$PID.*") -Force -ErrorAction SilentlyContinue
Write-Host "built $exe"
