# One cached compile through the shared compile cache (tools\cxx_cache.ps1): the proof's driver and a handy way to try it by hand.
#   pwsh -NoProfile -File tools\cxx_cc.ps1 -Root <tree root> -Src <a.cpp> -Obj <a.o> [-FlagsFile <one flag per line>]
# -Root is the tree the cache treats as "the worktree" (its basedir); the helper itself is always this checkout's. Prints the helper's one-line report.
param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj, [string]$FlagsFile)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) { . (Join-Path $PSScriptRoot 'env.ps1') | Out-Null }
$repo = $Root; $Flags = if ($FlagsFile) { @(Get-Content -LiteralPath $FlagsFile) } else { @() }
. (Join-Path $PSScriptRoot 'cxx_cache.ps1')
$Flags = ConvertTo-CxxRel $Flags; $Src = @(ConvertTo-CxxRel $Src)[0]; $Obj = @(ConvertTo-CxxRel $Obj)[0]   # as the build scripts do: repo-relative paths
$log = & $cxx0 @cxxRest @Flags -c $Src -o $Obj 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw 'compile failed' }
Show-CxxCache
