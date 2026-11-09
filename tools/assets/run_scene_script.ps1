# Runs one hidden viewer on a script file with a hard timeout; stdout (one JSON reply per line) goes to <script>.out.txt.
# Usage: pwsh -NoProfile -File tools\assets\run_scene_script.ps1 <script.jsonl> [-TimeoutSec 120]
param([Parameter(Mandatory)][string]$Script, [int]$TimeoutSec = 120)
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$exe = Join-Path $repo 'out\tools\wwe2k14_assets.exe'
$main = (Resolve-Path (Join-Path $repo '..\..\..')).Path
$game = Join-Path $main 'game_data\extracted'
$out = "$Script.out.txt"
$p = Start-Process -FilePath $exe -ArgumentList @('--game', $game, '--script', (Resolve-Path $Script).Path, '--exit', '--hidden') -RedirectStandardOutput $out -RedirectStandardError "$Script.err.txt" -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Host "TIMEOUT after $TimeoutSec s" }
Write-Host "exit $($p.ExitCode); output: $out"
