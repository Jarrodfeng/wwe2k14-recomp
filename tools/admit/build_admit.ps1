# Builds out\tools\admit\admit.exe from tools\admit\admit.cpp (lane CQA; docs\agents\admit.md). One compile, clang++ by absolute path (the
# folder tools\env.ps1 puts first as $LlvmBin); this build never goes through admit itself. No SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\admit\build_admit.ps1
# A running admit.exe cannot be overwritten but can be renamed. The swap is one ReplaceFile call (File.Replace: the old exe is renamed aside and
# the new one renamed in by the file system). If that fails, the OLD admit.exe is left in place (a working queue beats a missing one), the new
# build stays as admit.new.exe, and the build fails loudly. Callers still fall open when the exe is missing (docs\agents\admit.md).
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repo 'out\tools\admit'
New-Item -ItemType Directory -Force $outDir | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path $clang)) { throw "clang++ not found at $clang (tools\env.ps1 `$LlvmBin)" }
$src = Join-Path $repo 'tools\admit\admit.cpp'
$new = Join-Path $outDir 'admit.new.exe'
$exe = Join-Path $outDir 'admit.exe'
$flags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-D_CRT_SECURE_NO_WARNINGS',
           '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-fms-runtime-lib=static', '-fuse-ld=lld')
& $clang @flags $src -o $new
if ($LASTEXITCODE -ne 0) { throw 'admit build failed' }
if (Test-Path $exe) {
  $aside = Join-Path $outDir ("admit.old.{0}" -f [DateTime]::UtcNow.Ticks)
  try { [System.IO.File]::Replace($new, $exe, $aside) }
  catch { throw "could not replace $exe ($($_.Exception.Message)): the old admit.exe is left in place; the new build is $new" }
} else {
  [System.IO.File]::Move($new, $exe)
}
Get-ChildItem -LiteralPath $outDir -Filter 'admit.old.*' -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { } }
Write-Host "built $exe"
