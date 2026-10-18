<#
.SYNOPSIS
    Unattended benchmark runner (Session 6 harness, spec B5 in docs/optimization_candidates.md with the reviewer's
    corrections): interleaved A/B runs of one input script, a manifest per run, a watchdog, then a summary.

.DESCRIPTION
    For every run, in order:
      1. Refuses to start if a wwe2k14 process is already running.
      2. Mirrors the frozen user data (saves + shader cache) into out\bench\userdata_work with robocopy /MIR, so
         every run starts from the same save and cache (-ColdCache: out\bench\userdata_coldcache, empty cache).
      3. Launches wwe2k14.exe with WorkingDirectory = the exe folder (frames/timeline/captures go to <exe>\logs and
         <exe>\captures) and ONE argument string:
           --user_data_root=<work> --cache_root=<work>\cache --wwe2k14_timeline=true
           --wwe2k14_input_script=<absolute script> <-ExtraArgs> <the arm's flags>
      4. Finds the run number from the new wwe2k14_NNN.log. -Perf starts tools\perf_sample.ps1 -Out logs\perf_NNN.csv.
      5. Watchdog: no growth of frames_NNN.csv for -StallSeconds (30), no frames file after -StartupSeconds, or the
         run going past -MaxMinutes -> the game is killed and the run marked invalid. An NVIDIA driver error during
         the run (System log, nvlddmkm event 153; B8) also makes it invalid.
      6. Writes out\bench\runs\<exe folder>_<hash>\run_NNN.json: run number, label, arm, flags, script (+ SHA-256),
         SHA-256 of the exe and every rex*.dll, start time, duration, exit code, valid flag + reason, save files the
         run changed, driver events. One subfolder per logs folder, because every exe folder numbers its runs from
         001 (snapshot folders out\bench\<label>\ would otherwise overwrite each other's manifests).
    At the end prints `bench_report.py summary` for the runs (variants classified, arms compared per variant).
    -Expect 'tron_343=1562:3 tron_324 match' is passed on as --expect (scenes the route must reach, B7).

    Arms: -Arms 'A=--x=1;B=--x=0' (name=flags; flags may hold several --name=value words). -Repeat N runs every arm
    N times interleaved: -Order ABAB gives A B A B ..., ABBA gives A B B A A B ... (cancels slow drift).
    Without -Arms there is one arm named 'run' with no extra flags.

    Testing the runner without the game: -Exe <stand-in exe> -BenchDir <scratch folder with userdata_frozen>
    -ProcessName <stand-in name> keeps the real out\bench folders and a running game untouched.

    Nothing here is committed: user data, logs and manifests all live under out\ (gitignored). The user data folders
    hold game shader microcode and saves, so they must never be copied into the repo.

.EXAMPLE
    pwsh -File tools\bench\run_bench.ps1 -Script tools\input_scripts\bench_lesnar_bryan_quick.txt -Label c1 -Arms 'A=--clear_memory_page_state=true;B=--clear_memory_page_state=false' -Repeat 3 -Order ABBA -MaxMinutes 6

.EXAMPLE
    pwsh -File tools\bench\run_bench.ps1 -Script tools\input_scripts\bench_lesnar_bryan.txt -Label aa -Repeat 4 -Expect 'tron_343=1562:3 tron_324=2271:3 match' -DryRun
#>
param(
    [Parameter(Mandatory = $true)] [string]$Script,
    [string]$Label = 'bench',
    [string]$Arms = '',
    [int]$Repeat = 1,
    [ValidateSet('ABAB', 'ABBA')] [string]$Order = 'ABAB',
    [string]$ExtraArgs = '',
    [switch]$ColdCache,
    [double]$MaxMinutes = 10,
    [int]$StallSeconds = 30,
    [int]$StartupSeconds = 90,
    [switch]$Perf,
    [switch]$WarmUp,
    [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\wwe2k14.exe',
    [string]$BenchDir = '',
    [string]$ProcessName = 'wwe2k14',
    [string]$Expect = '',
    [switch]$NoSummary,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bench = if ($BenchDir) { $BenchDir } else { Join-Path $repo 'out\bench' }
$work = Join-Path $bench 'userdata_work'
$source = Join-Path $bench ($(if ($ColdCache) { 'userdata_coldcache' } else { 'userdata_frozen' }))
$failPatterns = @('\[wwe2k14\]\[crash\]', 'device lost', 'Failed to await', 'script FAILED')

function Fail([string]$msg) { Write-Host "ABORT: $msg"; if ($script:RunLockHandle) { Exit-RunLock $script:RunLockHandle }; exit 1 }

function Quote-Arg([string]$a) { if ($a -match '\s') { '"' + $a + '"' } else { $a } }

function Get-Sha256([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# First 8 hex digits of the SHA-256 of a string (names the manifest subfolder of a logs folder).
function Get-TextSha8([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text)) } finally { $sha.Dispose() }
    -join ($bytes[0..3] | ForEach-Object { $_.ToString('x2') })
}

# Size of a file another process is appending to, read through a shared handle (directory entries can lag).
function Get-LiveLength([string]$path) {
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
        try { return $fs.Length } finally { $fs.Dispose() }
    } catch { return -1 }
}

function Get-GameProcesses { @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) }

# --- checks -------------------------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Exe)) { Fail "no exe at $Exe" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$exeDir = Split-Path -Parent $Exe
$logsDir = Join-Path $exeDir 'logs'
# Every exe folder numbers its runs from 001, so manifests get one subfolder per logs folder. bench_report.py finds
# a run's manifest by its logs_dir (and start time), so the subfolder name only has to be unique.
$runsKey = ((Split-Path -Leaf $exeDir) -replace '[^A-Za-z0-9_.-]', '_') + '_' + (Get-TextSha8 $logsDir.ToLowerInvariant())
$runsDir = Join-Path (Join-Path $bench 'runs') $runsKey
$scriptPath = (Resolve-Path -LiteralPath $Script -ErrorAction SilentlyContinue)
if (-not $scriptPath) { $scriptPath = Resolve-Path -LiteralPath (Join-Path $repo $Script) -ErrorAction SilentlyContinue }
if (-not $scriptPath) { Fail "input script not found: $Script" }
$scriptPath = $scriptPath.Path
if (-not (Test-Path -LiteralPath $source)) {
    Fail "no frozen user data at $source (Phase 0: copy Documents\wwe2k14 there; never into the repo)"
}
if ($Repeat -lt 1) { Fail '-Repeat must be at least 1' }

# --- arms and schedule --------------------------------------------------------------------------------------------
$armList = @()
if ($Arms.Trim()) {
    foreach ($part in ($Arms -split ';')) {
        if (-not $part.Trim()) { continue }
        $i = $part.IndexOf('=')
        if ($i -lt 1) { Fail "bad arm '$part' (want NAME=flags, e.g. A=--x=1)" }
        $armList += [pscustomobject]@{ Name = $part.Substring(0, $i).Trim(); Flags = $part.Substring($i + 1).Trim() }
    }
} else {
    $armList += [pscustomobject]@{ Name = 'run'; Flags = '' }
}
$names = @($armList | ForEach-Object Name)
if (($names | Select-Object -Unique).Count -ne $names.Count) { Fail "arm names must be unique: $($names -join ', ')" }

$schedule = @()
if ($WarmUp) { $schedule += [pscustomobject]@{ Arm = $armList[0]; WarmUp = $true } }
for ($r = 0; $r -lt $Repeat; $r++) {
    $round = @($armList)
    if ($Order -eq 'ABBA' -and ($r % 2) -eq 1) { [array]::Reverse($round) }
    foreach ($a in $round) { $schedule += [pscustomobject]@{ Arm = $a; WarmUp = $false } }
}

function Build-Args($arm) {
    $parts = @(
        "--user_data_root=$work",
        "--cache_root=$(Join-Path $work 'cache')",
        '--wwe2k14_timeline=true',
        "--wwe2k14_input_script=$scriptPath"
    ) | ForEach-Object { Quote-Arg $_ }
    $line = ($parts -join ' ')
    if ($ExtraArgs.Trim()) { $line += ' ' + $ExtraArgs.Trim() }
    if ($arm.Flags) { $line += ' ' + $arm.Flags }
    $line
}

Write-Host ("run_bench: {0} run(s), label '{1}', order {2}, script {3}" -f $schedule.Count, $Label, $Order, $scriptPath)
Write-Host ("  user data: {0} -> {1}; watchdog: stall {2} s, startup {3} s, max {4} min" -f $source, $work,
            $StallSeconds, $StartupSeconds, $MaxMinutes)
Write-Host ("  logs: {0}; manifests: {1}" -f $logsDir, $runsDir)
$k = 0
foreach ($s in $schedule) {
    $k++
    Write-Host ("  {0,2}. arm {1}{2}: {3}" -f $k, $s.Arm.Name, $(if ($s.WarmUp) { ' (warm-up)' } else { '' }), (Build-Args $s.Arm))
}
if ($DryRun) { Write-Host 'dry run: nothing launched'; exit 0 }

New-Item -ItemType Directory -Force -Path $runsDir | Out-Null
New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
$pwshExe = (Get-Process -Id $PID).Path
$repoCommit = (& git -C $repo rev-parse --short HEAD 2>$null)
$sdkCommit = (& git -C (Join-Path $repo 'thirdparty\rexglue-sdk-src') rev-parse --short HEAD 2>$null)
$scriptSha = Get-Sha256 $scriptPath
$results = @()

# Session 8: the run lock (tools\runlock.ps1) is held for the whole schedule; released at the summary, on Fail, and
# by the finally below on any other error or Ctrl+C (so an in-process call can't leave it held in a long-lived shell).
. (Join-Path $PSScriptRoot '..\runlock.ps1')
$script:RunLockHandle = Enter-RunLock -Name "run_bench $($schedule.Count) runs"
try {

# --- runs ---------------------------------------------------------------------------------------------------------
$index = 0
foreach ($s in $schedule) {
    $index++
    $arm = $s.Arm
    $running = Get-GameProcesses
    if ($running.Count -gt 0) {
        Fail "a $ProcessName process is already running (PID $($running[0].Id)); close it first. Runs so far: $(($results | ForEach-Object run) -join ' ')"
    }

    # Same save and shader cache for every run.
    & robocopy $source $work /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "robocopy $source -> $work failed (exit $LASTEXITCODE)" }
    $saveBefore = @{}
    Get-ChildItem -LiteralPath $work -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notlike (Join-Path $work 'cache') + '*' } |
        ForEach-Object { $saveBefore[$_.FullName.Substring($work.Length + 1)] = Get-Sha256 $_.FullName }

    $exeSha = Get-Sha256 $Exe
    $dlls = [ordered]@{}
    Get-ChildItem -LiteralPath $exeDir -Filter 'rex*.dll' -File | Sort-Object Name |
        ForEach-Object { $dlls[$_.Name] = Get-Sha256 $_.FullName }

    $argLine = Build-Args $arm
    $before = @(Get-ChildItem -LiteralPath $logsDir -Filter 'wwe2k14_*.log' -File -ErrorAction SilentlyContinue | ForEach-Object Name)
    $start = Get-Date
    Write-Host ''
    Write-Host ("[{0}/{1}] {2} arm {3}{4}: starting" -f $index, $schedule.Count, $start.ToString('HH:mm:ss'), $arm.Name,
                $(if ($s.WarmUp) { ' (warm-up)' } else { '' }))
    $p = $null
    $perfProc = $null
    $num = $null
    $valid = $true
    $reason = ''
    $killed = $false
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $argLine -WorkingDirectory $exeDir -PassThru
        $null = $p.Handle  # keeps the exit code readable after the process ends
        $null = Add-RunLockKillOnExit $p   # Session 8: if this script is killed, Windows kills the game too

        $framesPath = $null
        $lastLen = -1
        $lastGrowth = Get-Date
        while ($true) {
            if ($p.HasExited) { break }
            $now = Get-Date
            $elapsed = ($now - $start).TotalSeconds
            if (-not $num) {
                $new = Get-ChildItem -LiteralPath $logsDir -Filter 'wwe2k14_*.log' -File -ErrorAction SilentlyContinue |
                    Where-Object { $before -notcontains $_.Name -and $_.Name -match '^wwe2k14_(\d+)\.log$' } |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1
                if ($new) {
                    $num = ($new.Name -replace '^wwe2k14_(\d+)\.log$', '$1')
                    $framesPath = Join-Path $logsDir "frames_$num.csv"
                    Write-Host "  run $num (log $($new.Name))"
                    if ($Perf) {
                        $perfOut = Join-Path $logsDir "perf_$num.csv"
                        $perfArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $repo 'tools\perf_sample.ps1')`" -Out `"$perfOut`" -WaitForStart 30"
                        $perfProc = Start-Process -FilePath $pwshExe -ArgumentList $perfArgs -WindowStyle Hidden -PassThru
                    }
                }
            }
            if ($elapsed -gt $MaxMinutes * 60) { $reason = "timeout: still running after $MaxMinutes min"; break }
            if (-not $num -or -not (Test-Path -LiteralPath $framesPath)) {
                if ($elapsed -gt $StartupSeconds) { $reason = "no frames file after $StartupSeconds s"; break }
            } else {
                $len = Get-LiveLength $framesPath
                if ($len -ne $lastLen) { $lastLen = $len; $lastGrowth = $now }
                elseif (($now - $lastGrowth).TotalSeconds -gt $StallSeconds) {
                    $reason = "hang: frames_$num.csv did not grow for $StallSeconds s"; break
                }
            }
            Start-Sleep -Milliseconds 1000
        }
        if (-not $p.HasExited) {
            $killed = $true
            $valid = $false
            Write-Host "  WATCHDOG: $reason; killing PID $($p.Id)"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $null = $p.WaitForExit(15000)
        }
    } finally {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    $p.WaitForExit()
    $end = Get-Date
    $exitCode = $p.ExitCode
    # perf_sample.ps1 follows the first wwe2k14 process it finds; make sure it ends with this run.
    if ($perfProc -and -not $perfProc.HasExited) {
        if (-not $perfProc.WaitForExit(5000)) { Stop-Process -Id $perfProc.Id -Force -ErrorAction SilentlyContinue }
    }

    if (-not $num) {
        $new = Get-ChildItem -LiteralPath $logsDir -Filter 'wwe2k14_*.log' -File -ErrorAction SilentlyContinue |
            Where-Object { $before -notcontains $_.Name -and $_.Name -match '^wwe2k14_(\d+)\.log$' } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($new) { $num = ($new.Name -replace '^wwe2k14_(\d+)\.log$', '$1') }
    }
    $logPath = if ($num) { Join-Path $logsDir "wwe2k14_$num.log" } else { $null }
    $timelinePath = if ($num) { Join-Path $logsDir "timeline_$num.csv" } else { $null }
    $framesPath = if ($num) { Join-Path $logsDir "frames_$num.csv" } else { $null }

    # Validity: watchdog, exit code, hard-fail lines in the log, timeline present.
    $hard = @()
    if ($logPath -and (Test-Path -LiteralPath $logPath)) {
        foreach ($pat in $failPatterns) {
            $hit = Select-String -LiteralPath $logPath -Pattern $pat -List -ErrorAction SilentlyContinue
            if ($hit) { $hard += $pat.Replace('\', '') }
        }
    }
    if (-not $num) { $valid = $false; $reason = ($reason, 'no new wwe2k14_NNN.log' | Where-Object { $_ }) -join '; ' }
    if (-not $killed -and $exitCode -ne 0) { $valid = $false; $reason = ($reason, "exit code $exitCode" | Where-Object { $_ }) -join '; ' }
    if ($hard.Count) { $valid = $false; $reason = ($reason, "log: $($hard -join ', ')" | Where-Object { $_ }) -join '; ' }
    if ($num -and -not (Test-Path -LiteralPath $timelinePath)) { $valid = $false; $reason = ($reason, 'no timeline file' | Where-Object { $_ }) -join '; ' }
    # B8 hard fail: an NVIDIA driver error while the game ran (System log, provider nvlddmkm, event 153). Other
    # nvlddmkm events are only recorded. "No events" and "no such provider" come back as errors too.
    $driverEvents = @()
    $driverCheck = 'ok'
    try {
        $driverEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'nvlddmkm'; StartTime = $start; EndTime = $end.AddSeconds(5) } -ErrorAction Stop)
    } catch {
        if ($_.FullyQualifiedErrorId -notlike 'NoMatching*') { $driverCheck = "failed: $($_.Exception.Message)" }
    }
    $tdr = @($driverEvents | Where-Object { $_.Id -eq 153 })
    if ($tdr.Count) { $valid = $false; $reason = ($reason, "nvlddmkm event 153 x$($tdr.Count) (GPU driver error)" | Where-Object { $_ }) -join '; ' }
    if ($driverCheck -ne 'ok') { Write-Host "  WARNING: driver event check $driverCheck" }
    if ($s.WarmUp) { $reason = ($reason, 'warm-up run' | Where-Object { $_ }) -join '; ' }

    $saveChanged = @()
    Get-ChildItem -LiteralPath $work -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notlike (Join-Path $work 'cache') + '*' } |
        ForEach-Object {
            $rel = $_.FullName.Substring($work.Length + 1)
            if ($saveBefore[$rel] -ne (Get-Sha256 $_.FullName)) { $saveChanged += $rel }
        }

    $manifest = [ordered]@{
        run = $num
        label = $Label
        arm = $arm.Name
        flags = $arm.Flags
        extra_args = $ExtraArgs
        args = $argLine
        warmup = [bool]$s.WarmUp
        script = $scriptPath
        script_sha256 = $scriptSha
        exe = $Exe
        exe_sha256 = $exeSha
        dlls_sha256 = $dlls
        repo_commit = $repoCommit
        sdk_commit = $sdkCommit
        userdata_source = $source
        cold_cache = [bool]$ColdCache
        perf = [bool]$Perf
        start = $start.ToString('o')
        duration_s = [math]::Round(($end - $start).TotalSeconds, 1)
        exit_code = $exitCode
        valid = $valid
        reason = $reason
        killed_by_watchdog = $killed
        logs_dir = $logsDir
        log = $logPath
        frames = $framesPath
        timeline = $timelinePath
        save_files_changed = $saveChanged
        gpu_driver_events = @($driverEvents | ForEach-Object { '{0} {1}' -f $_.Id, $_.TimeCreated.ToString('o') })
        gpu_driver_check = $driverCheck
        host = $env:COMPUTERNAME
    }
    $manifestName = if ($num) { "run_$num.json" } else { "run_unknown_$($start.ToString('yyyyMMdd_HHmmss')).json" }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runsDir $manifestName) -Encoding utf8
    $results += [pscustomobject]$manifest
    Write-Host ("  run {0} arm {1}: {2:N1} s, exit {3}, {4}{5}{6}" -f $num, $arm.Name, ($end - $start).TotalSeconds, $exitCode,
                $(if ($valid) { 'valid' } else { 'INVALID' }), $(if ($reason) { " ($reason)" } else { '' }),
                $(if ($saveChanged.Count) { "; changed: $($saveChanged -join ', ')" } else { '' }))
}

} finally { if ($script:RunLockHandle) { Exit-RunLock $script:RunLockHandle; $script:RunLockHandle = $null } }

# --- summary ------------------------------------------------------------------------------------------------------
Write-Host ''
Write-Host 'run   arm        valid  seconds  reason'
foreach ($r in $results) {
    Write-Host ("{0,-5} {1,-10} {2,-6} {3,7}  {4}" -f $r.run, $r.arm, $r.valid, $r.duration_s, $r.reason)
}
$nums = @($results | Where-Object { $_.run -and -not $_.warmup } | ForEach-Object run)  # warm-ups are discarded
if (-not $NoSummary -and $nums.Count) {
    $py = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
    $pyArgs = @()
    if ($py -eq 'py') { $pyArgs += '-3' }
    $pyArgs += @((Join-Path $PSScriptRoot 'bench_report.py'), 'summary') + $nums + @('--logs', $logsDir, '--manifests', $runsDir)
    if ($Expect.Trim()) { $pyArgs += @('--expect', $Expect.Trim()) }
    Write-Host ''
    & $py @pyArgs
}
