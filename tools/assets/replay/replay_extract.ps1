# Frame-replay harness, stage S2 (Session 98): runs tools\assets\replay\rdc_extract.py in RenderDoc's qrenderdoc for each
# capture, with a hard timeout, then tools\assets\replay\replay_index.py. 0 game runs (captures only).
#   & .\tools\assets\replay\replay_extract.ps1                         every rp_*.rdc in out\s98\replay: scan, index, dump, index
#   & .\tools\assets\replay\replay_extract.ps1 -Names rp_ringin -Steps dump,index
# Output per capture: out\s98\replay\<name>\ (scan.json, keys.json, dump.json, index.json, tex\, rt\, pass\) and
# out\s98\replay\index_summary.md. All of it is game imagery / data: out\ only, never committed.
param([string]$Dir = 'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay', [string[]]$Names = @(),
      [string[]]$Steps = @('scan', 'index', 'dump', 'index'), [int]$Minutes = 20, [int]$Parallel = 2)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$qrd = 'C:\Program Files\RenderDoc\qrenderdoc.exe'
if (-not (Test-Path -LiteralPath $qrd)) { "replay_extract: RenderDoc is not installed at $qrd"; exit 2 }
$caps = @(Get-ChildItem -LiteralPath $Dir -Filter 'rp_*.rdc' -File | Sort-Object Name)
if ($Names.Count) { $caps = @($caps | Where-Object { $n = $_.BaseName -replace '_frame\d+$', ''; $Names -contains $n }) }
if (-not $caps.Count) { "replay_extract: no captures in $Dir"; exit 2 }

function Start-Rd([System.IO.FileInfo]$cap, [string]$mode) {
    $name = $cap.BaseName -replace '_frame\d+$', ''
    $out = Join-Path $Dir $name
    $null = New-Item -ItemType Directory -Force -Path $out
    foreach ($f in @("FAILED_$mode", $(if ($mode -eq 'scan') { 'scan.json' } else { 'dump.json' }))) {
        $p = Join-Path $out $f; if (Test-Path -LiteralPath $p) { [System.IO.File]::Delete($p) }
    }
    $env:RDX_RDC = $cap.FullName; $env:RDX_OUT = $out; $env:RDX_SHADERS = Join-Path $Dir 'shaders'; $env:RDX_MODE = $mode
    $env:RDX_KEYS = Join-Path $out 'keys.json'; $env:RDX_HERE = $here
    $p = Start-Process -FilePath $qrd -ArgumentList @('--python', (Join-Path $here 'rdc_extract.py')) -PassThru
    [pscustomobject]@{ Proc = $p; Name = $name; Out = $out; Mode = $mode; Start = Get-Date }
}

foreach ($step in $Steps) {
    if ($step -eq 'index') {
        py -I (Join-Path $here 'replay_index.py') --dir $Dir @($caps | ForEach-Object { $_.BaseName -replace '_frame\d+$', '' })
        continue
    }
    $queue = [System.Collections.Generic.Queue[object]]::new(); $caps | ForEach-Object { $queue.Enqueue($_) }
    $running = @()
    while ($queue.Count -or $running.Count) {
        while ($queue.Count -and $running.Count -lt $Parallel) { $running += Start-Rd $queue.Dequeue() $step }
        Start-Sleep -Seconds 3
        foreach ($r in @($running)) {
            $done = $r.Proc.HasExited
            if (-not $done -and ((Get-Date) - $r.Start).TotalMinutes -gt $Minutes) {
                Stop-Process -Id $r.Proc.Id -Force -ErrorAction SilentlyContinue; "[$($r.Name)] $step TIMEOUT after $Minutes min"; $done = $true
            }
            if ($done) {
                $secs = [int]((Get-Date) - $r.Start).TotalSeconds
                $ok = Test-Path -LiteralPath (Join-Path $r.Out $(if ($step -eq 'scan') { 'scan.json' } else { 'dump.json' }))
                "[$($r.Name)] $step $(if ($ok) { 'ok' } else { 'FAILED (see extract_' + $step + '.log)' }) in $secs s"
                $running = @($running | Where-Object { $_ -ne $r })
            }
        }
    }
}
