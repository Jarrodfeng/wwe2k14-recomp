# S101 lane 127: Android (NDK clang, arm64) syntax-only compile of game-exe sources from a WORKTREE. Writes nothing, takes no run lock,
# starts no game and touches no device: it only runs `ninja -t commands` of the main checkout's Android game build (out\android\game-arm64)
# and `clang -fsyntax-only` with those flags, the worktree's src first on the include path, the PCH dropped.
# A source that has no statement in that build yet (added after its last configure) borrows main.cpp's flags (the line says so).
#   pwsh -NoProfile -File tools\android\wt_syntax_check_game.ps1 -Worktree <path> [-Files main.cpp,renderer_scene\arena_ours.cpp]
#   -Files default: every src\ file in CMakeLists.txt's WWE2K14_SOURCES.   Exit code = number of files with an error.
param([Parameter(Mandatory = $true)][string]$Worktree, [string[]]$Files = @())
$main = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$main\tools\android\env.ps1" *> $null
. "$main\tools\env.ps1" *> $null
$wtf = (Resolve-Path $Worktree).Path.Replace('\', '/')
$mainf = $main.Replace('\', '/')
$Files = $Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ }
if (-not $Files) {
  $block = (Get-Content -Raw -LiteralPath "$Worktree\CMakeLists.txt") -replace '(?s)^.*?set\(WWE2K14_SOURCES', '' -replace '(?s)\n\).*$', ''
  $Files = [regex]::Matches($block, '(?m)^\s+(src/[\w/]+\.cpp)') | ForEach-Object { $_.Groups[1].Value }
}
$cmds = & $Ninja -C $AndroidDirs.Game -t commands wwe2k14 2>&1
$template = $cmds | Where-Object { $_ -match '[\\/]main\.cpp(\s|$)' -and $_ -match ' -c ' } | Select-Object -First 1
if (-not $template) { "no Android compile command for main.cpp in $($AndroidDirs.Game)"; exit 99 }
$fail = 0
foreach ($f in $Files) {
  $rel = $f.Replace('\', '/'); if ($rel -notmatch '^src/') { $rel = "src/$rel" }
  $cmd = $cmds | Where-Object { $_ -match ('[\\/]' + [regex]::Escape($rel) + '(\s|$)') -and $_ -match ' -c ' } | Select-Object -First 1
  $note = ''
  if (-not $cmd) { $cmd = $template; $note = ' (no statement in the Android build yet: main.cpp flags)' }
  $src = ([regex]::Match($cmd, '-c (\S+)')).Groups[1].Value
  $cmd = $cmd.Replace("-I$mainf/src", "-I$wtf/src -I$mainf/src")
  $cmd = $cmd -replace '-MD -MT \S+ -MF \S+', '' -replace ' -o \S+', ''
  $cmd = $cmd -replace '-Xclang -include-pch -Xclang \S+', '' -replace '-Xclang -include -Xclang \S+', ''
  $cmd = $cmd.Replace("-c $src", "-fsyntax-only $wtf/$rel")
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $out = cmd /c "$cmd 2>&1"
  $code = $LASTEXITCODE
  "${rel}: exit $code after $([int]$sw.Elapsed.TotalSeconds) s$note"
  if ($code -ne 0) { $fail++; $out | Where-Object { $_ -match 'error' } | Select-Object -First 20 }
}
exit $fail
