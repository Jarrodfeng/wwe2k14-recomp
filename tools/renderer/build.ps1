# Builds the renderer and its player on their own: no SDK, no game build, no run lock. Only changed files are recompiled.
#   pwsh -NoProfile -File tools\renderer\build.ps1 [-Release] [-Clean] [-NoShaders]
# -NoShaders keeps the shader headers as they are: for a C++ change while someone is mid-edit on an effect.
param([switch]$Release, [switch]$Clean, [switch]$NoShaders)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$kind = if ($Release) { 'release' } else { 'dev' }
$out = Join-Path $repo "out\renderer\$kind"
$objects = Join-Path $out 'obj'
if ($Clean -and (Test-Path $objects)) { Remove-Item -Recurse -Force -Confirm:$false $objects }
New-Item -ItemType Directory -Force $objects | Out-Null

if (-not $NoShaders) {
  & py (Join-Path $repo 'tools\renderer\compile_shaders.py')
  if ($LASTEXITCODE -ne 0) { throw 'shader compile failed' }
}

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$sdk = (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', "-I$sdk\Include", '-D_CRT_SECURE_NO_WARNINGS', '-MMD')
if ($Release) { $flags += @('-O2', '-DNDEBUG') } else { $flags += @('-O1', '-DRDR_DEV') }

$sources = @(Get-ChildItem (Join-Path $repo 'src\renderer') -Recurse -Filter '*.cpp' | Where-Object { $_.Name -notlike '*_test.cpp' } | Select-Object -ExpandProperty FullName)
# S101 lane 47: rplay takes the promotion switches (--crowd-ours / --arena-ours / --wrestlers-ours, src\renderer_scene\promotion_rig.h), so it links the sources fview's session.cpp needs
# (the shared list tools\fview\session_deps.txt, also read by tools\fview\build.ps1 and tools\post\build_proof.ps1). They need stb_image.h (crowd_body.cpp) and the Windows defines.
$stb = Join-Path $repo 'thirdparty\rexglue-sdk-src\thirdparty\stb'
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { $stb = Join-Path (Resolve-Path (Join-Path $repo '..\..\..')).Path 'thirdparty\rexglue-sdk-src\thirdparty\stb' }
if (-not (Test-Path (Join-Path $stb 'stb_image.h'))) { throw "stb_image.h not found ($stb)" }
$flags += @("-I$stb", "-I$(Join-Path (Split-Path $stb -Parent) 'xxHash')", '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-Wno-deprecated-declarations')
$sources += @(Get-Content (Join-Path $repo 'tools\fview\session_deps.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { Join-Path $repo $_.Trim() })
# S101 lane 47 (review D4): a changed flag set (a new -I / -D) rebuilds every object: the stamp holds a hash of the flags
$flagHash = [BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes(($flags -join ' ')))).Replace('-', '')
$stamp = Join-Path $objects 'flags.sha1'
if (-not (Test-Path $stamp) -or (Get-Content -Raw $stamp).Trim() -ne $flagHash) {
  Get-ChildItem $objects -Filter '*.o' | Remove-Item -Force -Confirm:$false
  Set-Content -Path $stamp -Value $flagHash
}
$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$sources | ForEach-Object -ThrottleLimit 12 -Parallel {
  $source = $_
  $object = Join-Path $using:objects (($source.Substring($using:repo.Length + 1) -replace '[\\/:]', '_') + '.o')
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
    $text = & $using:clang @using:flags -c $source -o $object 2>&1
    if ($LASTEXITCODE -ne 0) { ($using:failed).Add("$source`n$($text -join "`n")") } elseif ($text) { Write-Host ($text -join "`n") }
  }
}
if ($failed.Count) { $failed | ForEach-Object { Write-Host $_ }; throw "$($failed.Count) file(s) failed to compile" }

$exe = Join-Path $out 'rplay.exe'
$objectFiles = @($sources | ForEach-Object { Join-Path $objects (($_.Substring($repo.Length + 1) -replace '[\\/:]', '_') + '.o') })   # (only this build's sources: an old object of a removed file is not linked)
$missingObjects = @($objectFiles | Where-Object { -not (Test-Path $_) })
if ($missingObjects.Count) { throw "missing objects: $($missingObjects -join ', ')" }
# S101 lane 163: no object is newer than the exe -> nothing to link. Relinking an unchanged tree failed with 'permission denied' whenever another
# process (a gate lane, a viewer) had the exe open.
if ((Test-Path $exe) -and -not @($objectFiles | Where-Object { (Get-Item $_).LastWriteTimeUtc -gt (Get-Item $exe).LastWriteTimeUtc }).Count) { Write-Host "up to date $exe"; exit 0 }
& $clang -fuse-ld=lld -g @objectFiles -luser32 -lgdi32 -ld3d11 -ldxgi -ldwmapi -lole32 -luuid -lshell32 -o $exe
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Write-Host "built $exe"
