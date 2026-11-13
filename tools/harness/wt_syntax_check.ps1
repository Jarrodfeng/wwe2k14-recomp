# Syntax-only compile of game-exe sources from a WORKTREE (S101). The game build itself cannot run from a worktree (tools\build.ps1
# builds the main checkout), so builders check their edits with the main checkout's real flags (ninja -t commands), the worktree's
# src first on the include path. Nothing is built or written to out\.
# Usage: pwsh -NoProfile -File tools\harness\wt_syntax_check.ps1 -Worktree <path> [-Files main.cpp,live.cpp]
param(
  [Parameter(Mandatory = $true)][string]$Worktree,
  [string[]]$Files = @('main.cpp')
)
$main = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$main\tools\env.ps1" *> $null
$cmds = & ninja -C "$main\out\build\win-amd64-release" -t commands wwe2k14 2>&1
$wtf = (Resolve-Path $Worktree).Path.Replace('\', '/')
$fail = 0
$Files = $Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ }   # pwsh -File passes "a,b" as one string
foreach ($f in $Files) {
  $leaf = [IO.Path]::GetFileName($f)
  $cmd = $cmds | Where-Object { $_ -match ('[\\/]' + [regex]::Escape($leaf) + '(\s|$)') -and $_ -match ' -c ' } | Select-Object -First 1
  if (-not $cmd) { "${f}: no compile command found in the main build (is it part of the wwe2k14 target?)"; $fail++; continue }
  $src = ([regex]::Match($cmd, '-c (\S+)')).Groups[1].Value
  $rel = $src.Replace('\', '/') -replace '^C:/Users/Jarrod/wwe2k14-recomp/', ''
  $wtsrc = "$wtf/$rel"
  $cmd = $cmd.Replace('-IC:/Users/Jarrod/wwe2k14-recomp/src', "-I$wtf/src -IC:/Users/Jarrod/wwe2k14-recomp/src")
  $cmd = $cmd -replace '-MD -MT \S+ -MF \S+', ''
  $cmd = $cmd -replace ' -o \S+', ''
  $cmd = $cmd.Replace("-c $src", "-fsyntax-only $wtsrc")
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $out = cmd /c "$cmd 2>&1"
  $code = $LASTEXITCODE
  "${rel}: exit $code after $([int]$sw.Elapsed.TotalSeconds) s"
  if ($code -ne 0) { $fail++; $out | Where-Object { $_ -match 'error' } | Select-Object -First 40 }
}
exit $fail
