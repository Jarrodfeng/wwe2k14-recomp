# Session 77 (review of the stage A tools): flag lists for game runs, combined so a later flag REPLACES an earlier
# one of the same name. A repeated --flag is a CLI11 parse error in the SDK's cvar::Init that drops every startup
# flag registered after it - user_data_root too, so the run would boot on the real save instead of the frozen mirror
# (found by the comparer skeptic: stageA.flags' det_serial_vblank_timeout_ms plus controls.ps1's timeout control).
# Dot-source it:  . "$PSScriptRoot\flags.ps1"
#   Get-StageAFlags                   -> the flags of tools\xdet\stageA.flags, one string per flag
#   Merge-Flags $earlier $later       -> $earlier without the names $later sets, then $later (strings or arrays)
#   Join-Flags (Merge-Flags ...)      -> one space-separated string (run_list.ps1 -Extra)

function Split-Flags($flags) {
  if ($null -eq $flags) { return @() }
  $out = @()
  foreach ($f in @($flags)) {
    if ("$f".Trim()) { $out += @("$f".Trim() -split '\s+(?=--)' | Where-Object { $_ }) }
  }
  return $out
}

function Get-FlagName([string]$flag) {
  if ($flag -match '^--([^=\s]+)') { return $Matches[1] }
  return $null
}

function Get-StageAFlags([string]$File = 'stageA.flags') {
  # (Session 77: -File played.flags = the "as played" config, Jarrod 2026-10-31 ~11:10.)
  @(Get-Content (Join-Path $PSScriptRoot $File) | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
    ForEach-Object { $_.Trim() })
}

function Merge-Flags($earlier, $later) {
  # (@(): a function returning one element hands back the bare string, and $b[0] would be its first character -
  # run 4885 lost its only flag that way.)
  $a = @(Split-Flags $earlier)
  $b = @(Split-Flags $later)
  $names = @{}
  foreach ($f in $b) { $n = Get-FlagName $f; if ($n) { $names[$n] = $true } }
  $kept = @($a | Where-Object { $n = Get-FlagName $_; -not ($n -and $names.ContainsKey($n)) })
  # Within the later list itself, the last of a name wins too.
  $seen = @{}
  $tail = @()
  for ($i = $b.Count - 1; $i -ge 0; $i--) {
    $n = Get-FlagName $b[$i]
    if ($n -and $seen.ContainsKey($n)) { continue }
    if ($n) { $seen[$n] = $true }
    $tail = @($b[$i]) + $tail
  }
  return @($kept + $tail)
}

function Join-Flags($flags) { return (@($flags) -join ' ') }
