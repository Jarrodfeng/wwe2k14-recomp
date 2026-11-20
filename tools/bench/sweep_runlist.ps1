# S105 C1: run a mode-sweep run list (route<TAB>extra flags per line; '#' lines skipped) through run_list.ps1, -Parallel N per batch.
#   & .\tools\bench\sweep_runlist.ps1 -List out\s105\c1\runlist.txt -Exe out\build\win-amd64-release-off\wwe2k14.exe -Base '--a=1 --b=2' [-Parallel 3] [-Only 1,4,7]
# Base flags and the line's flags are MERGED BY NAME (the line wins), so no switch is ever passed twice (LESSONS: a repeated string switch makes
# CLI11 drop every later switch). Relative paths in a flag value that start with game_data\ or out\ are made absolute (LESSONS: relative flag
# paths resolve against the exe folder). Prints one line per run (run number, route, verdict) and writes <List>.runs.tsv (index, route, run, line).
# Game runs: each line is one game run (rule 7: the caller logs the question first).
param([Parameter(Mandatory)][string]$List, [Parameter(Mandatory)][string]$Exe, [string]$Base = '', [int]$Parallel = 3, [int[]]$Only = @(),
      [int]$WaitSeconds = 900)
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$PSScriptRoot\run_pass.ps1"   # S105 H: Test-RunLinePass
Set-Location $R
function Split-Flags([string]$s) {
  $map = [ordered]@{}
  foreach ($t in ($s -split '\s+' | Where-Object { $_ })) {
    $k = ($t -split '=', 2)[0]
    $v = if ($t.Contains('=')) { ($t -split '=', 2)[1] } else { $null }
    if ($v -and ($v -match '^(game_data|out)\\')) { $v = Join-Path $R $v }
    $map[$k] = $v
  }
  return $map
}
function Join-Flags($map) { ($map.Keys | ForEach-Object { if ($null -ne $map[$_]) { "$_=$($map[$_])" } else { $_ } }) -join ' ' }
$lines = @(Get-Content $List | Where-Object { $_.Trim() -and -not $_.StartsWith('#') })
$jobs = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($Only.Count -and ($Only -notcontains ($i + 1))) { continue }
  $parts = $lines[$i] -split "`t", 2
  $m = Split-Flags $Base
  $lm = Split-Flags $(if ($parts.Count -gt 1) { $parts[1] } else { '' })
  foreach ($k in $lm.Keys) { $m[$k] = $lm[$k] }
  $jobs += [pscustomobject]@{ Index = $i + 1; Route = $parts[0].Trim(); Flags = (Join-Flags $m) }
}
$out = "$List.runs.tsv"
for ($b = 0; $b -lt $jobs.Count; $b += $Parallel) {
  $batch = @($jobs[$b..([Math]::Min($b + $Parallel, $jobs.Count) - 1)])
  "== batch $([int]($b / $Parallel) + 1): lines $(($batch | ForEach-Object Index) -join ', ') $(Get-Date -Format HH:mm:ss)"
  $res = & .\tools\bench\run_list.ps1 -Exe $Exe -Scripts @($batch | ForEach-Object Route) -ScriptExtras @($batch | ForEach-Object Flags) -Parallel $batch.Count -WaitSeconds $WaitSeconds 2>&1
  $runLines = @($res | Where-Object { "$_" -match '^\S+ -> wwe2k14_\d+' })
  foreach ($j in $batch) {
    $leaf = Split-Path -Leaf $j.Route
    $hit = $runLines | Where-Object { "$_".StartsWith("$leaf ->") } | Select-Object -First 1
    $runLines = @($runLines | Where-Object { $_ -ne $hit })
    $run = if ("$hit" -match 'wwe2k14_(\d+)') { $Matches[1] } else { '?' }
    $verdict = if ((Test-RunLinePass $hit)) { 'PASS' } elseif ($hit) { 'not PASS' } else { 'NO RUN LINE' }
    "  line $($j.Index) $leaf -> run $run $verdict"
    "$($j.Index)`t$($j.Route)`t$run`t$verdict`t$($j.Flags)" | Add-Content $out
  }
  $res | Where-Object { "$_" -match 'checks:|^ABORT|STALE|refus' } | ForEach-Object {   # S105: a run_list ABORT (stale exe) was hidden
 "  $_".Substring(0, [Math]::Min(220, "  $_".Length)) }
}
"== done $(Get-Date -Format HH:mm:ss); table $out"
