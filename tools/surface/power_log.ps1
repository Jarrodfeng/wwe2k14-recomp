# Session 95 (warm-60 work): the Surface's power log. Samples Windows performance counters once a second while the game
# runs (or for -Seconds) into a wide CSV, ON the Surface, installing nothing: the CPU package's RAPL power counters
# (Energy Meter: PKG / PP0 cores / PP1 GPU / DRAM), CPU utility and frequency, the thermal zones' temperature and
# passive limit, throttle reasons / performance limit flags where the machine has them, and the 3D engine's busy share.
# GPU frequency: Windows has no counter for it (checked at the first sample: counters.txt lists every counter set whose
# name says intel / gpu / freq / power / energy / thermal / throttle). The game's gpu_mhz CSV column is empty on Windows
# because src\frame_stats.h GpuClockMhz() returns -1 under _WIN32 (the kgsl / devfreq sysfs reader is Android only).
#   powershell -NoProfile -ExecutionPolicy Bypass -File power_log.ps1 -Out <dir>\power.csv [-WaitForStart 120]
#   ... -Seconds 60 -NoGame      a test sample without a game running
# Columns: time (HH:mm:ss.fff, the Surface's clock = the run log's), t_s, then one column per counter instance
# (rapl_pkg, rapl_pp0, rapl_pp1, rapl_dram: Energy Meter Power, milliwatts (idle test: PKG 1277, PP0 743, DRAM 543);
# rapl_*_pwh: the cumulative Energy counters in picowatt-hours; cpu_utility, cpu_perf, cpu_freq_mhz, perf_limit,
# perf_limit_flags (no thermal-zone temperature on this machine, see below),
# gpu3d_pct (sum of the 3D engines' utilisation, all processes), self_cpu_ms (this logger's own CPU time that second)).
# Hard timeout -MaxSeconds (default 3600). Read on the PC: tools\surface\power_read.py.
param([Parameter(Mandatory)][string]$Out, [int]$WaitForStart = 120, [int]$Seconds = 0, [switch]$NoGame,
      [int]$MaxSeconds = 3600, [switch]$NoGpuEngine)
$ErrorActionPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$dir = Split-Path -Parent $Out

# What the machine has (once): the counter sets worth knowing about, and whether each wanted counter exists.
$sets = @(Get-Counter -ListSet * | Where-Object { $_.CounterSetName -match 'intel|gpu|freq|power|energy|thermal|throttle|processor' })
$lines = foreach ($s in $sets) { "SET $($s.CounterSetName): $(@($s.Counter | ForEach-Object { $_ -replace '^\\[^\\]+\\', '' }) -join ' | ')" }
$lines | Set-Content (Join-Path $dir 'counters.txt')

# wanted: column prefix, counter path
$wanted = @(
  @('rapl_pkg',  '\Energy Meter(RAPL_Package0_PKG)\Power'),
  @('rapl_pp0',  '\Energy Meter(RAPL_Package0_PP0)\Power'),
  @('rapl_pp1',  '\Energy Meter(RAPL_Package0_PP1)\Power'),
  @('rapl_dram', '\Energy Meter(RAPL_Package0_DRAM)\Power'),
  @('cpu_utility', '\Processor Information(_Total)\% Processor Utility'),
  @('cpu_perf',    '\Processor Information(_Total)\% Processor Performance'),
  @('cpu_freq_mhz', '\Processor Information(_Total)\Processor Frequency'),
  @('perf_limit',       '\Processor Information(_Total)\% Performance Limit'),
  @('perf_limit_flags', '\Processor Information(_Total)\Performance Limit Flags'),
  # Energy: cumulative picowatt-hours (exact mean power over any window = delta energy / delta time).
  @('rapl_pkg_pwh',  '\Energy Meter(RAPL_Package0_PKG)\Energy'),
  @('rapl_pp0_pwh',  '\Energy Meter(RAPL_Package0_PP0)\Energy'),
  @('rapl_pp1_pwh',  '\Energy Meter(RAPL_Package0_PP1)\Energy'),
  @('rapl_dram_pwh', '\Energy Meter(RAPL_Package0_DRAM)\Energy')
  # (Thermal Zone Information: the Surface Pro 6 lists the set but has no instance, every path fails with 0xC0000BC4 / 0x800007D1,
  # and WMI MSAcpi_ThermalZoneTemperature says "Not supported" - no temperature without installing something. perf_limit /
  # perf_limit_flags are the throttle record.)
)
if (-not $NoGpuEngine) { $wanted += ,@('gpu3d_pct', '\GPU Engine(*engtype_3D)\Utilization Percentage') }
$paths = @(); $prefix = @{}
foreach ($w in $wanted) {
  $probe = Get-Counter -Counter $w[1] -ErrorAction SilentlyContinue
  if ($probe) { $paths += $w[1]; $prefix[$w[1].ToLower()] = $w[0]; $prefix[($w[1].ToLower() -replace '\([^)]*\)', '(')] = $w[0] } else { "MISSING $($w[1])" | Add-Content (Join-Path $dir 'counters.txt') }
}
if (-not $paths.Count) { 'no counters readable'; exit 1 }

if (-not $NoGame) {
  $deadline = (Get-Date).AddSeconds($WaitForStart)
  while (-not (Get-Process wwe2k14) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
  $game = Get-Process wwe2k14 | Select-Object -First 1
  if (-not $game) { 'no wwe2k14 process'; exit 1 }
}
$me = Get-Process -Id $PID
$t0 = Get-Date; $prevSelf = $me.TotalProcessorTime.TotalMilliseconds
$cols = $null; $buf = [System.Text.StringBuilder]::new(); $wrote = $false
function Col-Name($s) {
  $p = $s.Path.ToLower() -replace '^\\\\[^\\]+', ''     # strip \\COMPUTER
  if ($prefix.ContainsKey($p)) { return $prefix[$p] }
  $pp = $p -replace '\([^)]*\)', '('
  if ($prefix.ContainsKey($pp)) { return $prefix[$pp] }
  return $null
}
foreach ($once in 1) {
  Get-Counter -Counter $paths -SampleInterval 1 -Continuous | ForEach-Object {
    $now = Get-Date
    $vals = @{}
    foreach ($s in $_.CounterSamples) {
      $c = Col-Name $s
      if (-not $c) { continue }
      if ($c -eq 'gpu3d_pct') { $vals[$c] = [double]$vals[$c] + $s.CookedValue } else { $vals[$c] = $s.CookedValue }
    }
    $self = $me.TotalProcessorTime.TotalMilliseconds; $me.Refresh()
    $vals['self_cpu_ms'] = $self - $prevSelf; $prevSelf = $self
    if (-not $cols) {
      $cols = @($vals.Keys | Sort-Object)
      [void]$buf.AppendLine('time,t_s,' + ($cols -join ','))
    }
    $row = @($now.ToString('HH:mm:ss.fff'), [math]::Round(($now - $t0).TotalSeconds, 3)) +
           @($cols | ForEach-Object { if ($vals.ContainsKey($_)) { '{0:F2}' -f $vals[$_] } else { '' } })
    [void]$buf.AppendLine($row -join ',')
    $buf.ToString() | Add-Content $Out -NoNewline; [void]$buf.Clear()
    $el = ($now - $t0).TotalSeconds
    if ($el -ge $MaxSeconds) { break }
    if ($Seconds -gt 0 -and $el -ge $Seconds) { break }
    if (-not $NoGame) { $game.Refresh(); if ($game.HasExited) { break } }
  }
}
"done: $Out"
