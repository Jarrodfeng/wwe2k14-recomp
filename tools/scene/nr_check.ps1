# S103 NR: runs the not-ready unit (out\tools\nr_unit.exe, built by tools\scene\build_rig.ps1) on run 29's frame_000622.rsc (out\s103\run29_case.zip of the MAIN checkout), the planted old rule must FAIL.
#   pwsh -NoProfile -File tools\scene\nr_check.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$common = (& git -C $repo rev-parse --git-common-dir).Trim()
$main = Split-Path (Resolve-Path $common).Path -Parent
$zip = Join-Path $main 'out\s103\run29_case.zip'
if (-not (Test-Path $zip)) { throw "missing $zip" }
$dir = Join-Path $repo 'out\s103\NR'
New-Item -ItemType Directory -Force $dir | Out-Null
$rsc = Join-Path $dir 'frame_000622.rsc'
if (-not (Test-Path $rsc)) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $z = [System.IO.Compression.ZipFile]::OpenRead($zip)
  try {
    $e = $z.Entries | Where-Object { $_.FullName -eq 'capture/scene/frame_000622.rsc' } | Select-Object -First 1
    if (-not $e) { throw 'frame_000622.rsc is not in the case zip' }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $rsc, $true)
  } finally { $z.Dispose() }
}
$exe = Join-Path $repo 'out\tools\nr_unit.exe'
& $exe --record $rsc
if ($LASTEXITCODE -ne 0) { throw 'nr_unit failed' }
& $exe --record $rsc --plant old_rule | Out-Null
if ($LASTEXITCODE -ne 1) { throw "the planted old rule must exit 1 exactly (a failed check), got $LASTEXITCODE" }   # F6: a crash (any other code) is not a catch
Write-Host 'PASS nr_check (the planted old rule fails as it must)'
