# Syncs the Claude Desktop Code-tab sidebar cards (local_*.json) between two Claude accounts, both ways:
# a card missing on one side is copied over, and a card on both sides takes the newer copy.
# The chats themselves live in %USERPROFILE%\.claude\projects and are shared already; only the cards differ.
#
# Run with the Claude Desktop app fully quit (it runs Claude itself, so a session cannot do this for you):
#   pwsh -File tools\harness\copy_desktop_sessions.ps1            (sync)
#   pwsh -File tools\harness\copy_desktop_sessions.ps1 -DryRun    (only show what it would do; app may stay open)
# Never deletes; backs up both folders to out\backup\desktop_sessions_<time>\ first.
# Works the same whichever account you used last. With more than two accounts, pass -A and -B.

param(
    [switch]$DryRun,
    [string]$A = "",
    [string]$B = ""
)
$ErrorActionPreference = 'Stop'
$root = "$env:LOCALAPPDATA\Claude-Data\claude_pzs8sxrjxfjjc\claude-code-sessions"

if (-not $A -or -not $B) {
    $dirs = @(Get-ChildItem $root -Directory | ForEach-Object { Get-ChildItem $_.FullName -Directory } |
        Where-Object { $_.Name -notin 'backlog', 'waiting-input' })
    if ($dirs.Count -lt 2) { Write-Host "Only one account folder found. Sign in with the other account and open the Code tab once, then run this again."; exit 1 }
    if ($dirs.Count -gt 2) { Write-Host "More than two account folders; pass -A and -B with two of:"; $dirs | ForEach-Object { "  " + $_.FullName }; exit 1 }
    $A = $dirs[0].FullName; $B = $dirs[1].FullName
}

function Get-Cards($dir) {
    $h = @{}
    Get-ChildItem $dir -Filter 'local_*.json' -File | ForEach-Object { $h[$_.Name] = $_ }
    return $h
}
$cardsA = Get-Cards $A
$cardsB = Get-Cards $B
Write-Host "Account 1: $($cardsA.Count) cards in $A"
Write-Host "Account 2: $($cardsB.Count) cards in $B"

# Plan: list of (from file, to dir)
$plan = @()
foreach ($name in $cardsA.Keys) {
    $fa = $cardsA[$name]
    if (-not $cardsB.ContainsKey($name)) { $plan += , @($fa, $B) }
    elseif ($fa.LastWriteTime -gt $cardsB[$name].LastWriteTime) { $plan += , @($fa, $B) }
}
foreach ($name in $cardsB.Keys) {
    $fb = $cardsB[$name]
    if (-not $cardsA.ContainsKey($name)) { $plan += , @($fb, $A) }
    elseif ($fb.LastWriteTime -gt $cardsA[$name].LastWriteTime) { $plan += , @($fb, $A) }
}
$toA = @($plan | Where-Object { $_[1] -eq $A }).Count
$toB = @($plan | Where-Object { $_[1] -eq $B }).Count
Write-Host "To account 1: $toA   To account 2: $toB"
if ($plan.Count -eq 0) { Write-Host "Already in sync."; exit 0 }
if ($DryRun) { Write-Host "Dry run: nothing copied."; exit 0 }

if (Get-Process -Name 'claude' -ErrorAction SilentlyContinue) { Write-Host "Claude is still running. Quit the Desktop app fully (tray icon too) and any terminal claude, then run again."; exit 1 }

$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$bk = Join-Path $PSScriptRoot "..\..\out\backup\desktop_sessions_$stamp"
New-Item -ItemType Directory -Force "$bk\account1", "$bk\account2" | Out-Null
Copy-Item "$A\*" "$bk\account1" -Recurse
Copy-Item "$B\*" "$bk\account2" -Recurse
Write-Host "Backup: $((Resolve-Path $bk).Path)"

foreach ($p in $plan) { Copy-Item $p[0].FullName (Join-Path $p[1] $p[0].Name) -Force }
Write-Host "Synced $($plan.Count) cards. Both accounts' sidebars now hold every session."
