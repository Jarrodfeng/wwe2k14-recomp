# Session 103 (KBM): bare Shift / Control / Alt keyboard binds, offline. Host clang, 0 game runs.
#   pwsh -NoProfile -File tools\input\run_kbm_bind_test.ps1
# Takes the SDK header (include/rex/input/mnk/bind_match.h) from the committed SDK patch 118 (the tree under test needs no SDK
# checkout state), virtual_key.h from the SDK source, and the launcher's keyboard_binds.h from this tree.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'tools\env.ps1') | Out-Null
$patch = Join-Path $root 'patches\rexglue-sdk\full\118-session103-mnk-bare-modifiers.diff'
$inc = Join-Path $root 'out\kbm\sdk_inc'
$hdr = Join-Path $inc 'rex\input\mnk\bind_match.h'
New-Item -ItemType Directory -Force (Split-Path $hdr) | Out-Null
# the new-file hunk of the patch: lines after "+++ b/include/rex/input/mnk/bind_match.h" and its "@@", up to the next "diff --git"
$lines = Get-Content $patch
$start = [array]::IndexOf($lines, '+++ b/include/rex/input/mnk/bind_match.h')
if ($start -lt 0) { throw 'bind_match.h is not in patch 118' }
$out = New-Object System.Collections.Generic.List[string]
for ($i = $start + 2; $i -lt $lines.Count; $i++) {
  if ($lines[$i].StartsWith('diff --git')) { break }
  if ($lines[$i].StartsWith('+')) { $out.Add($lines[$i].Substring(1)) }
}
[System.IO.File]::WriteAllLines($hdr, $out)
# virtual_key.h: the main SDK checkout when it is there (a worktree has no thirdparty folder), else the main repo's
$vk = $null
foreach ($base in @($root, 'C:\Users\Jarrod\wwe2k14-recomp')) {
  $p = Join-Path $base 'thirdparty\rexglue-sdk-src\include'
  if (Test-Path (Join-Path $p 'rex\ui\virtual_key.h')) { $vk = $p; break }
}
if (-not $vk) { throw 'virtual_key.h not found (thirdparty\rexglue-sdk-src\include)' }
$exe = Join-Path $root 'out\kbm\kbm_bind_test.exe'
& clang++ -w -std=gnu++20 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN "-I$inc" "-I$vk" (Join-Path $PSScriptRoot 'kbm_bind_test.cpp') -luser32 -o $exe
if ($LASTEXITCODE) { throw 'build failed' }
& $exe
exit $LASTEXITCODE
