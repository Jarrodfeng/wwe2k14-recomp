# S102 TS1: list the Windows kernel-event (GPU watchdog) reports newer than a time. Runs ON the Surface (sent over ssh stdin by wer_reports.ps1) or on the PC against a
# fake layout (the unit test, tools\surface\wer_reports_selftest.py). READ-ONLY on the report folders; with -Stage it copies the dumps to that folder.
#   -Since   local time (Surface clock) as the PC saw it: 'yyyy-MM-ddTHH:mm:ss'. -PcNow = the PC's clock at the call, so the Surface-vs-PC offset is removed.
#   -Roots   WER folders (default ReportQueue + ReportArchive); each Kernel_* / *LiveKernel* subfolder is one report (Report.wer + a WATCHDOG-*.dmp).
#   -Lkr     C:\Windows\LiveKernelReports (dumps outside WER; skipped when the same dump name sits in a report folder at the same time).
#   -Stage   when given, copy each reported dump there as <guid8>_<name>; -Max caps the copies (all reports are still listed).
# Output, one line each:  NOW|<Surface clock>   REP|<code>|<event time local>|<dump name>|<report folder>|<staged file or ->   (WARN|text on a read problem)
# The event time is Report.wer's EventTime (a FILETIME): the folder's own write time is NOT the event (a boot-time requeue stamps a whole queue within seconds).
param([string]$Since = '', [string]$PcNow = '', [string[]]$Roots = @('C:\ProgramData\Microsoft\Windows\WER\ReportQueue', 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive'),
      [string]$Lkr = 'C:\Windows\LiveKernelReports', [string]$Stage = '', [int]$Max = 12)
$ErrorActionPreference = 'Continue'
$Roots = @($Roots | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # `pwsh -File` passes -Roots a,b as one string
$fmt = 'yyyy-MM-ddTHH:mm:ss'
$now = Get-Date
"NOW|$($now.ToString($fmt))"
$cut = [datetime]::MinValue
if ($Since) {
  $cut = [datetime]::ParseExact($Since, $fmt, $null)
  if ($PcNow) { $cut = $cut + ($now - [datetime]::ParseExact($PcNow, $fmt, $null)) }   # PC time -> Surface time
}
$seen = @{}; $script:copied = 0
function Emit($code, $t, $dumpName, $dir, $dumpPath) {
  if ($t -lt $cut) { return }
  $key = "$code|$($t.ToString($fmt))|$dumpName"
  $st = '-'
  if ($Stage -and $dumpPath -and (Test-Path -LiteralPath $dumpPath) -and $script:copied -lt $Max) {
    try {
      New-Item -ItemType Directory -Force $Stage | Out-Null
      $g = ($dir -replace '^.*_', '').Substring(0, [Math]::Min(8, ($dir -replace '^.*_', '').Length))
      $st = "${g}_$dumpName"; Copy-Item -LiteralPath $dumpPath -Destination (Join-Path $Stage $st) -Force; $script:copied++
    } catch { "WARN|copy $dumpName failed: $($_.Exception.Message)"; $st = '-' }
  }
  $seen[$dumpName + '|' + $t.ToString($fmt)] = 1
  "REP|$code|$($t.ToString($fmt))|$dumpName|$dir|$st"
}
foreach ($r in $Roots) {
  if (-not (Test-Path -LiteralPath $r)) { continue }
  foreach ($d in (Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(Kernel_|.*LiveKernel)' })) {
    try {
      $wer = Join-Path $d.FullName 'Report.wer'
      $code = ($d.Name -split '_')[1]; $t = $d.LastWriteTime
      if (Test-Path -LiteralPath $wer) {
        $txt = Get-Content -LiteralPath $wer -ErrorAction Stop
        $m = $txt | Select-String -Pattern '^EventTime=(\d+)' | Select-Object -First 1
        if ($m) { $t = [datetime]::FromFileTime([int64]$m.Matches[0].Groups[1].Value) }
        $c = $txt | Select-String -Pattern '^Sig\[0\]\.Value=(\w+)' | Select-Object -First 1
        if ($c) { $code = $c.Matches[0].Groups[1].Value }
      } else { "WARN|no Report.wer in $($d.Name) (folder time used)" }
      $dumps = @(Get-ChildItem -LiteralPath $d.FullName -Filter *.dmp -ErrorAction SilentlyContinue)
      if ($dumps.Count -eq 0) { Emit $code $t '-' $d.Name $null }
      foreach ($f in $dumps) { Emit $code $t $f.Name $d.Name $f.FullName }
    } catch { "WARN|$($d.Name): $($_.Exception.Message)" }
  }
}
if (Test-Path -LiteralPath $Lkr) {
  foreach ($f in (Get-ChildItem -LiteralPath $Lkr -Recurse -Filter *.dmp -ErrorAction SilentlyContinue)) {
    $dup = $false
    foreach ($k in $seen.Keys) { if ($k.StartsWith($f.Name + '|')) { $dup = $true } }
    if (-not $dup) { Emit ('lkr_' + $f.Directory.Name) $f.LastWriteTime $f.Name ('LiveKernelReports\' + $f.Directory.Name) $f.FullName }
  }
}
