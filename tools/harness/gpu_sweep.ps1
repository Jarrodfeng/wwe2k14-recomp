# Session 94 (idea 14, builder "sweep"): the GPU-family sweep. "Works on NVIDIA, black on Mali" caught before testers do.
#
# ANSWERS: does the TRUE NATIVE picture (tools\native\owned_check.ps1, 720p quick route) still come out right when the renderer PRETENDS
# to be each phone / iGPU family - its device limits, its missing formats and features, and the quirks-table rules that family's vendor
# id + driver version fire. Per family it runs owned_check -Only720 -Validation: the ON arm = TRUE NATIVE (--wwe2k14_d3d_owned=true) with
# the family's emulation flags (tools\android\emulate\profiles_<family>.tsv); the OFF arm = the plain PC reference (NOT emulated), so a
# black or wrong picture in the emulated arm is a shot difference. (Filtering gaps like RG16 point-sampling can show as small tolerance
# differences too: read the sheet in out\owned_check\sheets\ before believing a WARN / FAIL.)
#
# HOW TO CALL (in-process, from PowerShell; never `pwsh -File`):
#   & .\tools\harness\gpu_sweep.ps1                                  all 5 families (~2 runs each, ~10 s a run + validation)
#   & .\tools\harness\gpu_sweep.ps1 -Families mali_g57,adreno_720    some
#   & .\tools\harness\gpu_sweep.ps1 -Exe out\harness\bisect\exe\sdk_abc123\wwe2k14.exe   another build (passed to owned_check)
#   & .\tools\harness\gpu_sweep.ps1 -DryRun                          print the owned_check command lines only, start nothing
#   -OffFrom RUN    reuse this run (e.g. the main owned_check's 720p OFF arm) as every family's reference: 5 families = 5 runs.
#                   Without it the FIRST family runs ON + OFF and the others reuse that OFF run (owned_check -Only720 -ReuseOff RUN):
#                   5 families = 6 runs. (The OFF arm is the plain PC and identical for every family.)
#   -NoValidation   without Vulkan validation (faster; the validation column then says "-")
# Families: mali_g57, mali_g720_r49, adreno_720, powervr_bxm, intel_gen9 (profile files below). Profile format = run_profiles.ps1's:
# a line "NAME<TAB>FLAGS", '#' lines are comments. Values that come from docs\research\gpu_matrix_s93.md instead of a real device log
# are marked so in the profile file's header.
# THE RUN LOCK: owned_check -> run_list holds it (shared slots); this script takes no lock of its own. Families run one after the other.
# OUTPUT: a table (family | verdict | quirks fired | missing formats | validation errors | log), also in out\gpu_sweep\<stamp>\sweep.md
# with each family's full owned_check output next to it (<family>.txt). Exit 0 = every family PASS or WARN, 1 = a FAIL / NO VERDICT, 2 = bad call.
param([string[]]$Families = @('mali_g57', 'mali_g720_r49', 'adreno_720', 'powervr_bxm', 'intel_gen9'),
      [string]$Exe = '', [switch]$DryRun, [switch]$NoValidation,
      [string]$OffFrom = '',
      [string]$ExtraOn = '')   # Session 94 M1: flags added to every family's ON arm (the milestone's switches)   # a run number: the plain-PC 720p OFF arm of an earlier owned_check (same exe / route); every family reuses it
$root = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $root
$emu = "$root\tools\android\emulate"
# family -> (profile file, profile name)
$map = [ordered]@{
  mali_g57      = @("$emu\profiles_mali_g57.tsv", 'mali_g57')
  mali_g720_r49 = @("$emu\profiles_mali_g720_r49.tsv", 'mali_g720_r49')
  adreno_720    = @("$emu\profiles_a720.tsv", 'a720_stock')
  powervr_bxm   = @("$emu\profiles_powervr_bxm.tsv", 'powervr_bxm')
  intel_gen9    = @("$emu\profiles_intel_gen9.tsv", 'intel_gen9')
}
$Families = @($Families | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($f in $Families) { if (-not $map.Contains($f)) { "gpu_sweep: unknown family '$f' (known: $($map.Keys -join ', '))"; exit 2 } }

function Get-ProfileFlags([string]$file, [string]$name) {
  foreach ($line in Get-Content $file) {
    if ($line -match '^\s*#' -or -not $line.Trim()) { continue }
    $parts = $line -split "`t", 2
    if ($parts[0] -eq $name) { return $parts[1].Trim() }
  }
  throw "profile '$name' not found in $file"
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = "$root\out\gpu_sweep\$stamp"
$logDir = if ($Exe) { Join-Path (Split-Path -Parent $Exe) 'logs' } else { "$root\out\build\win-amd64-release\logs" }
$rows = @()
$offRun = ''
$bad = $false
if (-not $DryRun) { New-Item -ItemType Directory -Force $outDir | Out-Null }

foreach ($fam in $Families) {
  $flags = Get-ProfileFlags $map[$fam][0] $map[$fam][1]
  $onFlags = "--wwe2k14_d3d_owned=true $flags" + $(if ($ExtraOn) { " $ExtraOn" } else { "" })
  $ocArgs = @{ Only720 = $true; OnFlags = $onFlags }
  if (-not $NoValidation) { $ocArgs.Validation = $true }
  if ($Exe) { $ocArgs.Exe = $Exe }
  $reuseRun = if ($OffFrom) { $OffFrom } else { $offRun }
  if ($reuseRun) { $ocArgs.ReuseOff = $reuseRun }
  $cmd = "& $root\tools\native\owned_check.ps1 -Only720" + $(if (-not $NoValidation) { ' -Validation' }) +
         $(if ($Exe) { " -Exe '$Exe'" }) + $(if ($reuseRun) { " -ReuseOff '$reuseRun'" }) + " -OnFlags '$onFlags'"
  if ($DryRun -and -not $reuseRun) { $offRun = '<OFF run of the first family>' }
  if ($DryRun) { "[$fam] $cmd"; ''; continue }

  "[$fam] running owned_check (720p, emulated ON arm vs plain PC OFF arm) ..."
  $text = @(& "$root\tools\native\owned_check.ps1" @ocArgs 2>&1 | ForEach-Object { "$_" })
  $text | Set-Content "$outDir\$fam.txt"
  $verdict = 'NO VERDICT'
  foreach ($l in $text) {
    if ($l -match '^OWNED VERDICT (\w+)') { $verdict = $Matches[1] }
    elseif ($l -match '^NO VERDICT') { $verdict = 'NO VERDICT' }
  }
  # the ON arm is the first run printed by run_list
  $onRun = ''
  $seen = @()
  foreach ($l in $text) { if ($l -match ' -> wwe2k14_(\d+)') { $seen += $Matches[1] } }
  if ($seen) { $onRun = $seen[0] }   # (the OFF arm, when it ran, is the second line)
  if (-not $offRun -and $seen.Count -ge 2) { $offRun = $seen[1]; "[$fam] OFF reference = run $offRun (the next families reuse it)" }
  $log = if ($onRun) { Join-Path $logDir "wwe2k14_$onRun.log" } else { '' }
  $fired = @(); $missing = @(); $windows = @(); $valid = '-'
  if ($log -and (Test-Path $log)) {
    $lines = Get-Content $log
    foreach ($l in $lines) {
      if ($l -match '\[quirks\] (\w+): (\w+=\S+) \(' -and $l -notmatch 'NOT APPLIED') { $fired += "$($Matches[1])" }
      elseif ($l -match '\[quirks\] (formats): (.+?) \[matrix') { $missing += $Matches[2] }
      if ($l -match 'Vulkan: emulating formats (\d+-\d+|\d+) without features (0x[0-9A-F]+)') { $missing += "fmt $($Matches[1]) -$($Matches[2])" }
      if ($l -match '\[storage-window\] (on|off) \(([^)]*)\)') { $windows += "$($Matches[1]) ($($Matches[2]))" }
    }
    $fired = @($fired | Select-Object -Unique)
    $windows = @($windows | Select-Object -Unique)
  }
  foreach ($l in $text) { if ($l -match 'validation: (\d+) errors') { $valid = $Matches[1] } }
  if ($verdict -in 'FAIL', 'NO VERDICT') { $bad = $true }
  $rows += [pscustomobject]@{ Family = $fam; Verdict = $verdict; Quirks = ($fired -join ', '); Missing = ($missing -join '; ');
                              Windows = ($windows -join '; '); Validation = $valid; Log = $log }
  "[$fam] -> $verdict"
}
if ($DryRun) { exit 0 }

$md = @('# GPU sweep ' + $stamp, '', 'TRUE NATIVE owned_check (720p, quick route) with the renderer pretending to be each family; OFF arm = plain PC reference.', '',
        '| family | verdict | quirks fired | missing formats | storage window | validation errors | log |', '|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
  $md += "| $($r.Family) | $($r.Verdict) | $($r.Quirks) | $($r.Missing) | $($r.Windows) | $($r.Validation) | $($r.Log) |"
}
$md | Set-Content "$outDir\sweep.md"
''
$rows | Format-Table Family, Verdict, Quirks, Missing, Validation, Log -AutoSize -Wrap | Out-String -Width 220
"written: $outDir\sweep.md"
exit $(if ($bad) { 1 } else { 0 })
