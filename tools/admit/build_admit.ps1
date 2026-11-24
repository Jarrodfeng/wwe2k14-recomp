# Builds out\tools\admit\admit.exe from tools\admit\admit.cpp (lane CQA; docs\agents\admit.md). One compile, clang++ by absolute path (the
# folder tools\env.ps1 puts first as $LlvmBin). No SDK, no game, no run lock.
#   pwsh -NoProfile -File tools\admit\build_admit.ps1             build now (by hand)
#   pwsh -NoProfile -File tools\admit\build_admit.ps1 -IfStale    S109 GATE: build only when the exe's source stamp differs from admit.cpp
# The stamp: admit.exe.sha256 beside the exe = the SHA-256 of the admit.cpp bytes it was built from (written after a successful swap). The two callers
# (tools\admit\admit_py.py, tools\admit\Admit.ps1) compare it with the MAIN checkout's admit.cpp before every ask and run -IfStale on a difference, so a
# merged change runs from the first ask after the merge (S108 ran an exe "STALE" by file time, and any real change waited for a hand install).
# This rebuild-on-ask stands in for a Python port of the gate (the structural answer once the exe's speed has no production caller: `admit run`).
# -IfStale: one rebuilder at a time (an exclusive open of build.lock, waited for at most 120 s, the stamp read again under it). Its own compile is
# admitted by the EXISTING exe with one bounded ask (cc.tool, --timeout 10, killed at 45 s); only `granted N held` builds. Never a wait on memory: not
# admitted = the old exe stays, one loud line, exit 3. The callers then use the old exe.
# A running admit.exe cannot be overwritten but can be renamed. The swap is one ReplaceFile call (File.Replace: the old exe is renamed aside and
# the new one renamed in by the file system). If that fails, the OLD admit.exe is left in place (a working queue beats a missing one), the new
# build stays as admit.new.exe, and the build fails loudly. Callers still fall open when the exe is missing (docs\agents\admit.md).
param([switch]$IfStale)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repo 'out\tools\admit'
New-Item -ItemType Directory -Force $outDir | Out-Null
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
$src = Join-Path $repo 'tools\admit\admit.cpp'
$new = Join-Path $outDir 'admit.new.exe'
$exe = Join-Path $outDir 'admit.exe'
$stamp = "$exe.sha256"

function Get-SrcHash { (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash.ToLowerInvariant() }
function Test-Current {
  if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $stamp)) { return $false }
  return ([IO.File]::ReadAllText($stamp).Trim() -eq (Get-SrcHash))
}
function Invoke-Build {
  if (-not (Test-Path $clang)) { throw "clang++ not found at $clang (tools\env.ps1 `$LlvmBin)" }
  $hash = Get-SrcHash   # the bytes about to be compiled
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
  [IO.File]::WriteAllText($stamp, "$hash`n")
  Get-ChildItem -LiteralPath $outDir -Filter 'admit.old.*' -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { } }
  Write-Host "built $exe (admit.cpp sha256 $($hash.Substring(0, 12)))"
}
function Request-OwnAdmission {
  # -> $true only for `granted N held` from the existing exe; a missing exe (first install) builds without asking
  if (-not (Test-Path -LiteralPath $exe)) { return $true }
  $psi = [Diagnostics.ProcessStartInfo]::new($exe)
  foreach ($a in @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '10', '--pid', "$PID", '--detach', '--max-s', '300')) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.CreateNoWindow = $true
  $p = [Diagnostics.Process]::Start($psi)
  $read = $p.StandardOutput.ReadToEndAsync()
  if (-not $p.WaitForExit(45000)) { try { $p.Kill($true) } catch { }; $script:AdmitWhy = 'no answer in 45 s'; return $false }
  $out = if ($read.Wait(5000)) { [string]$read.Result } else { '' }
  if ($out -match '(?m)^granted \d+ held\s*$') { return $true }
  $script:AdmitWhy = (($out -split "`n" | Where-Object { $_ -match '\S' }) | Select-Object -First 1)
  if (-not $script:AdmitWhy) { $script:AdmitWhy = "exit $($p.ExitCode)" }
  return $false
}

if (-not $IfStale) { Invoke-Build; exit 0 }
if (Test-Current) { exit 0 }
$lockPath = Join-Path $outDir 'build.lock'
$lock = $null
$deadline = [DateTime]::UtcNow.AddSeconds(120)
while (-not $lock) {
  try { $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
  catch [System.IO.IOException] {
    if ([DateTime]::UtcNow -gt $deadline) { Write-Host "[admit] admit.exe is older than admit.cpp and another rebuild held $lockPath for 120 s: keeping the old admit.exe"; exit 3 }
    Start-Sleep -Milliseconds 500
  }
}
$asked = $false
try {
  if (Test-Current) { exit 0 }   # the rebuilder before us built it
  $script:AdmitWhy = ''
  $asked = Test-Path -LiteralPath $exe
  if (-not (Request-OwnAdmission)) {
    Write-Host "[admit] admit.exe is older than admit.cpp; its rebuild was not admitted ($($script:AdmitWhy)): keeping the old admit.exe until a later ask"
    exit 3
  }
  Write-Host "[admit] admit.exe is older than admit.cpp ($src): rebuilding it (one clang compile)"
  try { Invoke-Build } catch { Write-Host "[admit] rebuilding admit.exe FAILED ($($_.Exception.Message)): keeping the old admit.exe"; exit 3 }
  exit 0
} finally {
  if ($asked) { try { $null = & $exe release --pid $PID --kind cc.tool 2>$null } catch { } }   # a forced / narrowed / held slot from the ask goes back
  $lock.Dispose()
}
