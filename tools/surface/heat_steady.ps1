# Session 102 lane S0-2: the steady / not steady rule, in PowerShell 5.1 for the bench on the Surface (which has no Python).
# The rule and its numbers live in tools\surface\heat_rules.py (read its header); this is the same rule, and
# tools\tests\heat_s02_test.py checks both give the same verdict on the same files, running THIS file under powershell.exe 5.1.
# Dot-source it:  . .\heat_steady.ps1
#   Get-HeatVerdict -Csv <power.csv> [-PrevCsv <previous step's power.csv>]
#       -> @{ Steady; Reason ('steady' | 'not steady: ...' [+ ' | vs previous step: ...']); Retryable (a HEAT reason: throttle / flag change / drift) }
#   Clear-StaleStepFiles <step folder>      deletes power.csv, counters.txt, heat_verdict.txt, threads.csv of an earlier run; returns the names removed
#   Test-HeatRetry -Verdict -StepName -IsRetry -ExitCode -TimedOut     -> $true only for a heat reason on a first try of a non-A/B step of a game that ended cleanly
$script:HeatWarmupS = 10; $script:HeatMinRows = 20; $script:HeatLimitDrop = 5.0; $script:HeatDrift = 0.20; $script:HeatPkgDrift = 0.35
$script:HeatInv = [System.Globalization.CultureInfo]::InvariantCulture

# One number, or $null: unparsable text and NaN / inf are not readings (review F1; 5.1 parses "NaN" and "Infinity", pwsh 7 also "nan").
function ConvertTo-HeatNumber($v) {
  $d = 0.0
  if ($v -eq $null -or [string]$v -eq '') { return $null }
  if (-not [double]::TryParse([string]$v, [System.Globalization.NumberStyles]::Float, $script:HeatInv, [ref]$d)) { return $null }
  if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { return $null }
  return $d
}
function Get-HeatColumn($rows, [string]$name) {
  $out = New-Object System.Collections.Generic.List[double]
  foreach ($r in $rows) { $d = ConvertTo-HeatNumber $r.$name; if ($d -ne $null) { $out.Add($d) } }
  return ,$out.ToArray()
}
function Get-HeatMean([double[]]$x, [int]$from, [int]$to) { if ($to -le $from) { return $null }; $s = 0.0; for ($i = $from; $i -lt $to; $i++) { $s += $x[$i] }; return $s / ($to - $from) }
function Get-HeatMedian([double[]]$x) { $s = @($x | Sort-Object); $n = $s.Count; if ($n % 2) { return $s[[int][math]::Floor($n / 2)] }; return 0.5 * ($s[$n / 2 - 1] + $s[$n / 2]) }
# Rows after the warm-up. A row whose t_s is not a number (a header row repeated mid-file, review F2) is dropped, never thrown on.
function Get-HeatRows([string]$Csv) {
  $all = @(Import-Csv $Csv)
  return @($all | Where-Object { $t = ConvertTo-HeatNumber $_.t_s; $t -ne $null -and $t -ge $script:HeatWarmupS })
}
function New-HeatVerdict([bool]$Steady, [string]$Reason, [bool]$Retryable = $false) { return [pscustomobject]@{ Steady = $Steady; Reason = $Reason; Retryable = $Retryable } }

function Get-HeatVsPrevious([string]$Csv, [string]$PrevCsv) {
  if (-not $PrevCsv -or -not (Test-Path $PrevCsv)) { return '' }
  $parts = @()
  $a = Get-HeatRows $Csv; $b = Get-HeatRows $PrevCsv
  $pa = Get-HeatColumn $a 'cpu_perf'; $pb = Get-HeatColumn $b 'cpu_perf'
  if ($pa.Count -and $pb.Count) { $ma = Get-HeatMean $pa 0 $pa.Count; $mb = Get-HeatMean $pb 0 $pb.Count; if ($ma -and $mb) { $parts += ('cpu_perf {0:+0;-0;+0} %' -f (100 * ($ma / $mb - 1))) } }
  $ka = Get-HeatColumn $a 'rapl_pkg'; $kb = Get-HeatColumn $b 'rapl_pkg'
  if ($ka.Count -and $kb.Count) { $ma = Get-HeatMean $ka 0 $ka.Count; $mb = Get-HeatMean $kb 0 $kb.Count; if ($ma -and $mb) { $parts += ('package {0:+0;-0;+0} %' -f (100 * ($ma / $mb - 1))) } }
  if ($parts.Count) { return 'vs previous step: ' + ($parts -join ', ') }
  return ''
}

function Get-HeatVerdict([string]$Csv, [string]$PrevCsv = '') {
  if (-not (Test-Path $Csv)) { return New-HeatVerdict $false "not steady: refused: $Csv missing" }
  $v = Get-HeatVerdictCore $Csv
  $vp = Get-HeatVsPrevious $Csv $PrevCsv
  if ($vp) { $v.Reason = $v.Reason + ' | ' + $vp }
  return $v
}

function Get-HeatVerdictCore([string]$Csv) {
  $rows = Get-HeatRows $Csv
  if ($rows.Count -lt $script:HeatMinRows) { return New-HeatVerdict $false "not steady: too short ($($rows.Count) rows after the $($script:HeatWarmupS) s warm-up, need $($script:HeatMinRows))" }
  $perf = Get-HeatColumn $rows 'cpu_perf'
  if ($perf.Count -lt $script:HeatMinRows) { return New-HeatVerdict $false 'not steady: no heat record (no cpu_perf column)' }
  $lim = Get-HeatColumn $rows 'perf_limit'
  if ($lim.Count) {
    $drop = (Get-HeatMedian $lim) - ($lim | Measure-Object -Minimum).Minimum
    if ($drop -gt $script:HeatLimitDrop) { return New-HeatVerdict $false ('not steady: perf_limit fell {0:F0} points below its median (throttled)' -f $drop) $true }
  }
  $flv = Get-HeatColumn $rows 'perf_limit_flags'   # (a function's ,array output must be assigned before it is piped)
  $fl = @($flv | Sort-Object -Unique)
  if ($fl.Count -gt 1) { return New-HeatVerdict $false "not steady: perf_limit_flags changed ($($fl -join ', '))" $true }
  $h = [int][math]::Floor($perf.Count / 2)
  $a = Get-HeatMean $perf 0 $h; $b = Get-HeatMean $perf $h $perf.Count
  if ($a -and [math]::Abs($b / $a - 1.0) -gt $script:HeatDrift) { return New-HeatVerdict $false ('not steady: cpu_perf drifted {0:+0;-0} % first to second half (band +-{1:F0} %)' -f (100 * ($b / $a - 1)), (100 * $script:HeatDrift)) $true }
  $pk = Get-HeatColumn $rows 'rapl_pkg'
  if ($pk.Count -ge $script:HeatMinRows) {
    $h = [int][math]::Floor($pk.Count / 2)
    $a = Get-HeatMean $pk 0 $h; $b = Get-HeatMean $pk $h $pk.Count
    if ($a -and [math]::Abs($b / $a - 1.0) -gt $script:HeatPkgDrift) { return New-HeatVerdict $false ('not steady: package power drifted {0:+0;-0} % (band +-{1:F0} %)' -f (100 * ($b / $a - 1)), (100 * $script:HeatPkgDrift)) $true }
  }
  return New-HeatVerdict $true 'steady'
}

# Review F4: thread_busy.ps1 APPENDS (a second header breaks tools\perf\thread_scenes.py), power_log.ps1 appends too: an earlier run of the same
# step folder must not leave any of its files behind.
function Clear-StaleStepFiles([string]$Dest) {
  $removed = @()
  foreach ($f in 'power.csv', 'counters.txt', 'heat_verdict.txt', 'threads.csv') {
    $p = Join-Path $Dest $f
    if (Test-Path $p) { Remove-Item -Force $p; $removed += $f }
  }
  return ,$removed
}

# Review F3: repeat only for a heat reason, once, never for an A/B arm (the A B B A pairing needs one run per slot) or a game that crashed / timed out.
function Test-HeatRetry($Verdict, [string]$StepName, [bool]$IsRetry, $ExitCode, [bool]$TimedOut) {
  if ($Verdict.Steady -or -not $Verdict.Retryable -or $IsRetry -or $TimedOut) { return $false }
  if ($ExitCode -ne 0) { return $false }
  if ($StepName -like 'ab_*' -or $StepName -eq 'abwarm') { return $false }
  return $true
}
