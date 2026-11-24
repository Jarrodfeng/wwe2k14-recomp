# S106 lane C5: the planted test of `runlock.ps1 run -Alone` (docs\research\s106\C5.md "Planted test", steps 1-7; step 8 is alone_replay.py).
#   pwsh -NoProfile -File tools\harness\alone_selftest.ps1        (about 2 minutes, 0 game runs, no real run lock, no real admit state)
# Isolated like tools\owned\test_runlock_build.ps1: a scratch copy of tools\runlock.ps1 (+ harness\runlock_alone.ps1) with its OWN lock folder and swapped
# names (games = wwe2k14_fake_game, compilers = wwe2k14_fake_cc), an own admit state folder (WWE_ADMIT_DIR, WWE_ADMIT_TEST under %TEMP%), the pause file
# WWE_ALONE_FILE in the scratch lock folder, and THIS checkout's out\tools\admit\admit.exe. Holders block on a named event (no polling) and are killed.
# Prints PASS / FAIL per check; exit 1 on any FAIL. Each UNSAFE arm (a plant) must FAIL its detector, or the test says so.
# entry: Enter-RunLock -Alone (tools\runlock.ps1, via `runlock.ps1 run -Alone`) and admit.exe hold / run (attempt, admit_wait)
param([string]$Scratch =(Join-Path ([System.IO.Path]::GetTempPath()) ('alone_selftest_' + $PID)), [string]$ToolsDir = '', [string]$Only = '')   # S106 C5F: -ToolsDir = a folder holding runlock.ps1 + harness\runlock_alone.ps1 (an OLD copy proves the new steps fail on it); -Only new = steps U, 8-12 only
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $ToolsDir) { $ToolsDir = Join-Path $repo 'tools' }
$runOld = ($Only -ne 'new')
$admitExe = Join-Path $repo 'out\tools\admit\admit.exe'
if (-not (Test-Path $admitExe) -or (Get-Item $admitExe).LastWriteTime -lt (Get-Item (Join-Path $repo 'tools\admit\admit.cpp')).LastWriteTime) {   # built here, never through admit
    & pwsh -NoProfile -File (Join-Path $repo 'tools\admit\build_admit.ps1'); if ($LASTEXITCODE -ne 0) { throw 'admit build failed' }
}
if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
$lockDir = Join-Path $Scratch 'lock'
$admitDir = Join-Path $Scratch 'admit'
New-Item -ItemType Directory -Force $lockDir, $admitDir, (Join-Path $Scratch 'harness') | Out-Null
Set-Content -Path (Join-Path $admitDir 'fake_mem.txt') -Value '100 100'
# --- the scratch copy of runlock.ps1: its own folder, fake names (each substitution must happen) ---
$copy = Join-Path $Scratch 'runlock.ps1'
$text = Get-Content -Raw (Join-Path $ToolsDir 'runlock.ps1')
$real = "'C:\Users\Jarrod\wwe2k14-recomp\out\harness\runlock'"
if (-not $text.Contains($real)) { throw 'lock dir literal not found in runlock.ps1' }
$text = $text.Replace($real, "'$lockDir'")
if (-not $text.Contains("@('wwe2k14')")) { throw 'game names literal not found in runlock.ps1' }
$text = $text.Replace("@('wwe2k14')", "@('wwe2k14_fake_game')")
$m = [regex]::Match($text, '(?m)^\$script:RunLockCompilerNames = @\([^)]*\)')
if (-not $m.Success) { throw 'compiler names line not found in runlock.ps1' }
$text = $text.Replace($m.Value, "`$script:RunLockCompilerNames = @('wwe2k14_fake_cc')")
Set-Content -Path $copy -Value $text -NoNewline
Copy-Item (Join-Path $ToolsDir 'harness\runlock_alone.ps1') (Join-Path $Scratch 'harness\runlock_alone.ps1')
$aloneFile = Join-Path $lockDir 'alone.lock'
foreach ($v in 'WWE_NO_ADMIT', 'WWE_ADMIT_CONFIG', 'WWE2K14_RUNLOCK_TOKEN', 'WWE_BUILD_PARALLEL') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
$env:WWE_ADMIT_DIR = $admitDir; $env:WWE_ADMIT_TEST = '1'; $env:WWE_ADMIT_MODE = 'enforce'; $env:WWE_ALONE_FILE = $aloneFile; $env:WWE_ADMIT_EXE = $admitExe
$fails = 0; $started = New-Object System.Collections.Generic.List[object]
function Check([string]$what, [bool]$ok, [string]$detail = '') { if ($ok) { "PASS  $what" } else { $script:fails++; "FAIL  $what  $detail" } }
# --- helpers ---
# A body the runlock child runs: writes a marker (it started), optionally signals S and blocks on G (named events, no polling).
Set-Content -Path (Join-Path $Scratch 'body.ps1') -Value @'
param([string]$Scratch, [string]$Id, [string]$Hold = '')
Set-Content -Path (Join-Path $Scratch "marker_$Id.txt") -Value (Get-Date).ToString('HH:mm:ss.fff')
if ($Hold) {
    $s = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\wwe_alone_${Id}_S")
    $g = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\wwe_alone_${Id}_G")
    [void]$s.Set(); [void]$g.WaitOne(300000)
}
exit 0
'@
function New-Ev([string]$Id) {
    @{ S = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\wwe_alone_${Id}_S"); G = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\wwe_alone_${Id}_G") }
}
function Start-RL([string[]]$RlArgs, [string]$Id, [switch]$Hold) {
    $body = @('-NoProfile', '-File', (Join-Path $Scratch 'body.ps1'), $Scratch, $Id) + $(if ($Hold) { @('hold') } else { @() })
    $a = @('-NoProfile', '-File', $copy, 'run') + $RlArgs + @('--', 'pwsh') + $body
    $p = Start-Process -FilePath pwsh -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch "$Id.out") -RedirectStandardError (Join-Path $Scratch "$Id.err")
    $started.Add($p); return $p
}
function Get-Out([string]$Id) { (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch "$Id.out")) + (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch "$Id.err")) }
function Test-Marker([string]$Id) { Test-Path (Join-Path $Scratch "marker_$Id.txt") }
function Start-Pipe([string]$Exe, [string[]]$ArgList) {   # a child whose stdin / stdout we hold (closing stdin ends it)
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($a in $ArgList) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi); $started.Add($p); return $p
}
function Stop-Tree($p) { if ($p -and -not $p.HasExited) { & taskkill.exe /F /T /PID $p.Id *> $null } }
function Get-History { if (Test-Path (Join-Path $lockDir 'history.tsv')) { @(Import-Csv -Delimiter "`t" (Join-Path $lockDir 'history.tsv')) } else { @() } }
$interop = Join-Path $repo 'tools\admit\lock_interop.py'
$admitDirSlot = Join-Path $admitDir 'cc.tool.0'

try {
  if ($runOld) {
    # ---- 1. the S105 fault (run 098): a SHARED holder lives; an alone request must wait for it ----
    $e1 = New-Ev 'h1'
    $hold1 = Start-RL @('-Shared', '-Name', 'fake_shared') 'h1' -Hold
    Check '1 setup: the fake shared holder started' ($e1.S.WaitOne(90000))
    $plant = Start-RL @('-Shared', '-Name', 'plant_shared') 'p1'          # the UNSAFE arm: -Alone swapped for a SHARED request
    $plantBeside = $plant.WaitForExit(30000) -and (Test-Marker 'p1')
    Check '1 plant: a SHARED request starts beside the holder (the detector can fail)' $plantBeside
    $alone1 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_1') 'a1'
    $beside = $alone1.WaitForExit(6000) -or (Test-Marker 'a1')
    Check '1a -Alone does not start while the shared holder lives' (-not $beside) "marker $(Test-Marker 'a1')"
    $e1.G.Set() | Out-Null
    Check '1b it starts once the holder releases' ($alone1.WaitForExit(90000) -and $alone1.ExitCode -eq 0 -and (Test-Marker 'a1')) "exit $($alone1.ExitCode): $(Get-Out 'a1')"
    Check '1c its log says `alone: yes` and the end line `held`' ((Get-Out 'a1') -match 'alone: yes \(games 0, compiles 0' -and (Get-Out 'a1') -match 'alone: held') (Get-Out 'a1')
    $h = Get-History
    $rel = $h | Where-Object { $_.event -eq 'release' -and $_.name -eq 'fake_shared' } | Select-Object -First 1
    $acq = $h | Where-Object { $_.event -eq 'acquire' -and $_.name -eq 'alone_1' } | Select-Object -First 1
    Check '1d the history: the alone acquire is after the holder release and has games 0' ($rel -and $acq -and ([datetime]$acq.time -ge [datetime]$rel.time) -and $acq.games -eq '0') "rel $($rel.time) acq $($acq.time) games $($acq.games)"

    # ---- 2. a fake admit grant (a live admit slot) keeps an alone request out; once it ends the next one starts ----
    $fake = Start-Pipe 'py' @('-I', $interop, 'hold', $admitDirSlot)
    Check '2 setup: the fake admit slot is held' ($fake.StandardOutput.ReadLine() -eq 'locked')
    $a2 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_2') 'a2'
    $done = $a2.WaitForExit(90000)
    $o2 = Get-Out 'a2'
    Check '2a the alone request is refused: exit 3, the NO line names cc.tool.0 and its pid, nothing started' ($done -and $a2.ExitCode -eq 3 -and $o2 -match "alone: NO after \d+ s \(games 0; compiles 1: admit cc\.tool\.0 pid \d+" -and -not (Test-Marker 'a2')) "exit $($a2.ExitCode): $o2"
    Check '2b the refusal is in the history' ((Get-History | Where-Object { $_.event -eq 'alone_refused' -and $_.name -eq 'alone_2' }).Count -eq 1)
    $fake.StandardInput.Close(); [void]$fake.WaitForExit(30000)
    $a2b = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_2b') 'a2b'
    Check '2c with the holder gone the next alone request starts' ($a2b.WaitForExit(90000) -and $a2b.ExitCode -eq 0 -and (Get-Out 'a2b') -match 'alone: yes') (Get-Out 'a2b')

    # ---- 3. a fake compiler process: the same NO line; one that appears during the run makes the end line `broken` ----
    $cc = Join-Path $Scratch 'wwe2k14_fake_cc.exe'
    Copy-Item (Join-Path $env:SystemRoot 'System32\PING.EXE') $cc   # (a console-less hidden process that just lives ~2 min; choice.exe exits at once without a console)
    $ccp = Start-Process -FilePath $cc -ArgumentList @('-n', '120', '127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'cc1.out')
    Check '3 setup: the fake compiler process lives' (-not $ccp.HasExited -and @(Get-Process -Name 'wwe2k14_fake_cc' -ErrorAction SilentlyContinue).Count -ge 1)
    $a3 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_3') 'a3'
    $d3 = $a3.WaitForExit(90000)
    Check '3a a fake compiler process is named in the NO line (exit 3, nothing started)' ($d3 -and $a3.ExitCode -eq 3 -and (Get-Out 'a3') -match 'compiles 1: wwe2k14_fake_cc\.exe' -and -not (Test-Marker 'a3')) (Get-Out 'a3')
    Stop-Tree $ccp; [void]$ccp.WaitForExit(30000)
    $e3 = New-Ev 'h3'
    $a3b = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_3b') 'h3' -Hold
    Check '3b alone: yes with no compiler' ($e3.S.WaitOne(90000))
    $ccp2 = Start-Process -FilePath $cc -ArgumentList @('-n', '120', '127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'cc2.out')
    $seen = $false
    for ($k = 0; $k -lt 60 -and -not $seen; $k++) {   # the sampler looks every 2 s: a bounded marker wait on its own file
        $t = Get-ChildItem $lockDir -Filter 'alone_*.tsv' -ErrorAction SilentlyContinue | ForEach-Object { Get-Content -Raw $_.FullName -ErrorAction SilentlyContinue }
        if ($t -match 'wwe2k14_fake_cc') { $seen = $true } else { Start-Sleep -Milliseconds 500 }
    }
    Check '3c the sampler saw the compiler that appeared during the run' $seen
    $e3.G.Set() | Out-Null
    $d3b = $a3b.WaitForExit(60000)
    Check '3d the end line says `broken` and names it (readout not judged)' ($d3b -and (Get-Out 'h3') -match 'alone: broken at \d\d:\d\d:\d\d \(wwe2k14_fake_cc\.exe pid \d+' -and (Get-Out 'h3') -match 'readout not judged') (Get-Out 'h3')
    Stop-Tree $ccp2

    # ---- 4. the pause blocks admit; S109 GATE: the paused wait counts toward the bound, so both asks are REFUSED naming the run; ending the pause grants ----
    $e4 = New-Ev 'h4'
    $a4 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_4') 'h4' -Hold
    Check '4 setup: the alone run holds the pause' ($e4.S.WaitOne(90000))
    $hp = Start-Pipe $admitExe @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '2')
    $rd = $hp.StandardOutput.ReadLineAsync()
    $rp = Start-Pipe $admitExe @('run', '--kind', 'cc.tool', '--timeout', '2', '--', 'cmd', '/c', 'exit 0')
    Check '4a a paused hold is REFUSED at its 2 s bound, naming the alone run' ($rd.Wait(15000) -and $rd.Result -like 'refused REFUSED after *' -and $rd.Result -match "held off by an alone run '") "got '$(if ($rd.IsCompleted) { $rd.Result })'"
    Check '4b ... and the paused run-form is REFUSED (exit 75), its command never runs' ($rp.WaitForExit(15000) -and $rp.ExitCode -eq 75)
    $hp.StandardInput.Close(); [void]$hp.WaitForExit(30000)
    $e4.G.Set() | Out-Null
    [void]$a4.WaitForExit(60000)
    $hp4 = Start-Pipe $admitExe @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '30')
    $rd4 = $hp4.StandardOutput.ReadLineAsync()
    Check '4c with the pause ended a hold is granted ("granted 1 held")' ($rd4.Wait(30000) -and $rd4.Result -eq 'granted 1 held') "got '$(if ($rd4.IsCompleted) { $rd4.Result })'"
    $hp4.StandardInput.Close(); [void]$hp4.WaitForExit(30000)

    # ---- 5. crash release: a killed holder frees the pause at once; a waiting admit is granted; the next EXCLUSIVE taker takes over ----
    $e5 = New-Ev 'h5'
    $a5 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_5') 'h5' -Hold
    Check '5 setup: the alone run holds the pause' ($e5.S.WaitOne(90000))
    $hp5 = Start-Pipe $admitExe @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '30')
    $rd5 = $hp5.StandardOutput.ReadLineAsync()
    Check '5a a waiting hold is not granted while the holder lives' (-not $rd5.Wait(2500))
    Stop-Tree $a5; [void]$a5.WaitForExit(30000)
    Check '5b after taskkill /F /T the waiting admit is granted within 10 s' ($rd5.Wait(10000) -and $rd5.Result -like 'granted*') "got '$(if ($rd5.IsCompleted) { $rd5.Result })'"
    $tr = Start-Pipe 'py' @('-I', $interop, 'try', $aloneFile)
    Check '5c lock_interop try alone.lock reads free' ($tr.StandardOutput.ReadLine() -eq 'free')
    $n5 = Start-RL @('-Name', 'next_5') 'n5'
    Check '5d the next EXCLUSIVE taker takes over the stale lock' ($n5.WaitForExit(90000) -and $n5.ExitCode -eq 0 -and (Get-Out 'n5') -match 'stale lock') (Get-Out 'n5')
    $hp5.StandardInput.Close(); [void]$hp5.WaitForExit(30000)

    # ---- 6. an expired pause (the lock is held, the record's until has passed): a hung holder cannot hold compiles forever ----
    $fs = [System.IO.FileStream]::new($aloneFile, 'OpenOrCreate', 'ReadWrite', [System.IO.FileShare]'ReadWrite, Delete', 1)
    try {
        $fs.Lock(0, 1)
        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $b = [System.Text.Encoding]::UTF8.GetBytes("{`"pid`":$PID,`"token`":`"expired`",`"name`":`"hung`",`"since`":$($now - 900),`"until`":$($now - 30)}")
        $fs.SetLength(1); $fs.Position = 1; $fs.Write($b, 0, $b.Length); $fs.Flush($true)
        $hp6 = Start-Pipe $admitExe @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '30')
        Check '6 an expired pause is ignored (granted at once)' ($hp6.StandardOutput.ReadLineAsync().Wait(15000))
        $hp6.StandardInput.Close(); [void]$hp6.WaitForExit(30000)
        # and the same record with a future until DOES hold it off (the detector for 6 can fail)
        $b = [System.Text.Encoding]::UTF8.GetBytes("{`"pid`":$PID,`"token`":`"live`",`"name`":`"live`",`"since`":$now,`"until`":$($now + 600)}")
        $fs.SetLength(1); $fs.Position = 1; $fs.Write($b, 0, $b.Length); $fs.Flush($true)
        $hp6b = Start-Pipe $admitExe @('hold', '--kind', 'cc.tool', '--want', '1', '--min', '1', '--timeout', '30')
        Check '6 plant: the same pause with a future until holds it off' (-not $hp6b.StandardOutput.ReadLineAsync().Wait(3000))
        $hp6b.StandardInput.Close(); [void]$hp6b.WaitForExit(30000)   # (the stdin of a still-waiting admit: it ends it)
    } finally { $fs.Dispose() }
    Stop-Tree $hp6b

    # ---- 7. a wrapper under a pause (S109 GATE): the ask is REFUSED loudly at admit's bound (exit 75 / a throw), never `not held` (E5's unseen compiles) ----
    $old = Join-Path $Scratch 'old'; New-Item -ItemType Directory -Force $old | Out-Null
    # The OLD wrappers (the plant: the unsafe direction) = the new files with the S109 refusal turned back into the pre-GATE fall-open (`not held`, the full want).
    $newPy = Get-Content -Raw (Join-Path $repo 'tools\admit\admit_py.py')
    $oldPyText = $newPy.Replace('        raise Refused(refused)', '        refused = None   # (plant: the pre-GATE fall-open)')
    if ($oldPyText -eq $newPy) { throw 'could not make the old (pre-GATE) admit_py.py: the refusal raise was not found' }
    Set-Content -Path (Join-Path $old 'admit_py.py') -Value $oldPyText -NoNewline
    Copy-Item (Join-Path $repo 'tools\admit\admit.json') $old; Copy-Item (Join-Path $repo 'tools\admit\tool_build_parallel.json') $old
    $newPs = Get-Content -Raw (Join-Path $repo 'tools\admit\Admit.ps1')
    $oldPsText = [regex]::Replace($newPs, '(?m)^  throw "\[admit\] \$Kind build of .*$', '  Write-Host "[admit] $Want compiles at once (not held: $refusal)"; return $Want')
    if ($oldPsText -eq $newPs) { throw 'could not make the old (pre-GATE) Admit.ps1: the refusal throw was not found' }
    Copy-Item (Join-Path $repo 'tools\admit\admit.json') $old
    Set-Content -Path (Join-Path $old 'Admit.ps1') -Value $oldPsText -NoNewline
    Copy-Item (Join-Path $repo 'tools\admit\tool_build_parallel.json') $old -ErrorAction SilentlyContinue
    Set-Content -Path (Join-Path $Scratch 'wrap.py') -Value @'
import sys, time
sys.path.insert(0, sys.argv[1])
import admit_py
admit_py.GRACE_S = 1
with admit_py.hold('cc.tool', 1, timeout_s=1, interactive=True) as n:
    print('WHY', n.why, flush=True)
'@
    Set-Content -Path (Join-Path $Scratch 'wrap.ps1') -Value @'
param($Ps1)
. $Ps1
$n = Request-BuildParallel -Want 1 -TimeoutS 1 -Interactive
Release-BuildParallel
'@
    $env:WWE_ADMIT_GRACE_S = '1'
    $fs = [System.IO.FileStream]::new($aloneFile, 'OpenOrCreate', 'ReadWrite', [System.IO.FileShare]'ReadWrite, Delete', 1)
    $w = @{}
    try {
        $fs.Lock(0, 1)
        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $b = [System.Text.Encoding]::UTF8.GetBytes("{`"pid`":$PID,`"token`":`"w7`",`"name`":`"wrapper test`",`"since`":$now,`"until`":$($now + 600)}")
        $fs.SetLength(1); $fs.Position = 1; $fs.Write($b, 0, $b.Length); $fs.Flush($true)
        foreach ($which in 'new_py', 'old_py', 'new_ps', 'old_ps') {
            $argl = switch ($which) { 'new_py' { @('-I', (Join-Path $Scratch 'wrap.py'), (Join-Path $repo 'tools\admit')) } 'old_py' { @('-I', (Join-Path $Scratch 'wrap.py'), $old) }
                'new_ps' { @('-NoProfile', '-File', (Join-Path $Scratch 'wrap.ps1'), (Join-Path $repo 'tools\admit\Admit.ps1')) } 'old_ps' { @('-NoProfile', '-File', (Join-Path $Scratch 'wrap.ps1'), (Join-Path $old 'Admit.ps1')) } }
            $exe = if ($which -like '*_py') { 'py' } else { 'pwsh' }
            $w[$which] = Start-Process -FilePath $exe -ArgumentList $argl -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch "w_$which.out") -RedirectStandardError (Join-Path $Scratch "w_$which.err")
            $started.Add($w[$which])
        }
        foreach ($k in @($w.Keys)) { [void]$w[$k].WaitForExit(30000) }   # admit answers at its 1 s bound while the pause is still held
        $allDone = -not (@($w.Values) | Where-Object { -not $_.HasExited })
    } finally { $fs.Dispose() }   # the pause ends here
    foreach ($k in $w.Keys) { [void]$w[$k].WaitForExit(60000) }
    function Get-W($k) { (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch "w_$k.out")) + (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch "w_$k.err")) }
    Check '7 plant (python): the OLD (pre-GATE) wrapper falls open: `not held`, unseen compiles (the detector can fail)' ((Get-W 'old_py') -match 'WHY not held') (Get-W 'old_py')
    Check '7 plant (PowerShell): the OLD wrapper falls open the same way' ((Get-W 'old_ps') -match 'not held') (Get-W 'old_ps')
    Check '7a every wrapper answered while the pause was still held (admit''s bound, not the pause, ends the wait)' $allDone
    Check '7b admit_py.hold is REFUSED under the pause (exit 75, the run named), never `not held`' ($w['new_py'].ExitCode -eq 75 -and (Get-W 'new_py') -match "REFUSED after .*held off by an alone run 'wrapper test'" -and (Get-W 'new_py') -notmatch 'not held') (Get-W 'new_py')
    Check '7c Request-BuildParallel throws the refusal under the pause, never `not held`' ($w['new_ps'].ExitCode -ne 0 -and (Get-W 'new_ps') -match 'REFUSED after ' -and (Get-W 'new_ps') -notmatch 'not held') (Get-W 'new_ps')
  }
  # ======== S106 C5F (docs\research\s106\review_C5.md): B2 and notes 2, 3, 4, 5, 9, 10 ========
  # ---- U. the end line is `held` only for a sampler that is proven alive (B2): a pure unit over Format-RunLockAloneLine ----
  . (Join-Path $ToolsDir 'harness\runlock_alone.ps1')   # functions only
  function New-FakeAlone([hashtable]$Over) {
      $st = [hashtable]::Synchronized(@{ Stop = $false; Samples = 3; ThreadSamples = 3; Broken = $null; Last = (Get-Date); Err = '' })
      foreach ($k in $Over.Keys) { $st[$k] = $Over[$k] }
      [pscustomobject]@{ Alone = @{ State = $st; Since = (Get-Date).AddSeconds(-10); Async = $null } }
  }
  Check 'U1 a sampler with samples from its thread and a fresh last sample says `held`' ((Format-RunLockAloneLine (New-FakeAlone @{}) -Final) -match '^alone: held ') (Format-RunLockAloneLine (New-FakeAlone @{}) -Final)
  foreach ($case in @(@('U2 the sampler thread never took a sample (the caller`s final look is not one)', @{ ThreadSamples = 0; Samples = 1 }),
                      @('U3 the sampler recorded an error', @{ Err = 'boom' }),
                      @('U4 the sampler died: its last sample is 30 s old', @{ Last = (Get-Date).AddSeconds(-30) }))) {
      $l = Format-RunLockAloneLine (New-FakeAlone $case[1]) -Final
      Check ($case[0] + ': the line is unverified (readout not judged), never held') ($l -match '^alone: unverified \(' -and $l -match 'readout not judged' -and $l -notmatch 'alone: held') $l
  }
  $l = Format-RunLockAloneLine (New-FakeAlone @{ ScanErrMax = 3; ScanFails = 3; ScanErr = 'boom' }) -Final
  Check 'U6 C5FF N4: 3 scan failures in a row -> unverified (readout not judged)' ($l -match '^alone: unverified \(' -and $l -match '3 scans in a row') $l
  $l = Format-RunLockAloneLine (New-FakeAlone @{ ScanErrMax = 1; ScanFails = 1; ScanErr = 'boom'; TsvFails = 5; TsvErr = 'disk' }) -Final
  Check 'U7 C5FF N4: one transient scan failure and tsv write errors -> still held' ($l -match '^alone: held ') $l
  $brk = New-FakeAlone @{ Broken = @{ At = (Get-Date); Last = (Get-Date); Items = @{ 'cc:1' = @{ Kind = 'cc'; Text = 'clang++.exe pid 1'; Key = 'cc:1'; Name = 'clang++.exe' } } } }
  Check 'U5 a sighting still says `broken`' ((Format-RunLockAloneLine $brk -Final) -match '^alone: broken at ') (Format-RunLockAloneLine $brk -Final)

  # ---- 8. the live B2 arms: a sampler that cannot start / throws on every scan / dies is `unverified` on the real run ----
  foreach ($mode in 'nostart', 'throws') {
      $env:WWE_ALONE_TEST = $mode
      $p8 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', "alone_8_$mode") "a8$mode"
      [void]$p8.WaitForExit(90000)
      Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue
      Check ("8 sampler planted '$mode': the end line is unverified, never held") ((Get-Out "a8$mode") -match 'alone: unverified \(' -and (Get-Out "a8$mode") -notmatch 'alone: held') (Get-Out "a8$mode")
  }
  $env:WWE_ALONE_TEST = 'die'
  $e8 = New-Ev 'h8'
  $p8d = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_8_die') 'h8' -Hold
  Check '8 setup: the alone run with a dying sampler is up' ($e8.S.WaitOne(90000))
  Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 8   # past the 6 s the verdict allows a sampler to be silent (the thread took one sample and ended)
  $e8.G.Set() | Out-Null
  [void]$p8d.WaitForExit(60000)
  Check '8 sampler planted `die`: the end line is `unverified` and says the sampler stopped' ((Get-Out 'h8') -match 'alone: unverified \(' -and (Get-Out 'h8') -notmatch 'alone: held') (Get-Out 'h8')

  # ---- 9. note 2: a throw during the drain in a LONG-LIVED caller gives back the EXCLUSIVE lock and the pause ----
  Set-Content -Path (Join-Path $Scratch 'thrower.ps1') -Value @'
param([string]$Copy, [string]$Scratch)
. $Copy
$script:RunLockCompilerNames = @($null)   # the plant: Get-Process -Name @($null) throws inside the drain
$res = 'no throw'
try { $h = Enter-RunLock -Name 'thrower' -TimeoutMinutes 1 -Mode Exclusive -Alone -AloneDrainSeconds 5 -Quiet } catch { $res = "threw: $($_.Exception.GetType().Name)" }
Set-Content -Path (Join-Path $Scratch 'thrower_result.txt') -Value $res
$s = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', 'Local\wwe_alone_th9_S')
$g = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', 'Local\wwe_alone_th9_G')
[void]$s.Set(); [void]$g.WaitOne(300000)
'@
  $e9 = New-Ev 'th9'
  $th = Start-Process -FilePath pwsh -ArgumentList @('-NoProfile', '-File', (Join-Path $Scratch 'thrower.ps1'), $copy, $Scratch) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'th9.out') -RedirectStandardError (Join-Path $Scratch 'th9.err')
  $started.Add($th)
  Check '9 setup: the drain threw in the long-lived caller (and it lives on)' ($e9.S.WaitOne(90000) -and -not $th.HasExited -and (Get-Content -Raw (Join-Path $Scratch 'thrower_result.txt')) -match 'threw') (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch 'thrower_result.txt'))
  $sh9 = Start-RL @('-Shared', '-TimeoutMinutes', '0.4', '-Name', 'after_throw') 'n9'
  Check '9a a SHARED request is granted at once after the throw (the exclusive lock was given back)' ($sh9.WaitForExit(60000) -and $sh9.ExitCode -eq 0 -and (Test-Marker 'n9')) "exit $($sh9.ExitCode): $(Get-Out 'n9')"
  $tr9 = Start-Pipe 'py' @('-I', $interop, 'try', $aloneFile)
  Check '9b the pause file is free after the throw' ($tr9.StandardOutput.ReadLine() -eq 'free')
  $e9.G.Set() | Out-Null; [void]$th.WaitForExit(30000)

  # ---- 10. note 3: own = the alone run's tree + its ancestors; a compiler that is a SIBLING (another child of the outer holder) makes the drain say NO ----
  $cc10 = Join-Path $Scratch 'wwe2k14_fake_cc.exe'
  if (-not (Test-Path $cc10)) { Copy-Item (Join-Path $env:SystemRoot 'System32\PING.EXE') $cc10 }
  Set-Content -Path (Join-Path $Scratch 'outer10.ps1') -Value @'
param([string]$Copy, [string]$Scratch, [string]$Cc)
$c = Start-Process -FilePath $Cc -ArgumentList @('-n', '120', '127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'cc10.out')
$o = & pwsh -NoProfile -File $Copy run -Alone -AloneDrainSeconds 3 -Name inner10 -- pwsh -NoProfile -Command 'exit 0' 2>&1
Set-Content -Path (Join-Path $Scratch 'inner10.txt') -Value ((($o | ForEach-Object { "$_" }) -join "`n") + "`nexit=$LASTEXITCODE")
Stop-Process -Id $c.Id -Force -ErrorAction SilentlyContinue
'@
  $o10 = Start-Process -FilePath pwsh -ArgumentList @('-NoProfile', '-File', $copy, 'run', '-Name', 'outer10', '--', 'pwsh', '-NoProfile', '-File', (Join-Path $Scratch 'outer10.ps1'), $copy, $Scratch, $cc10) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'o10.out') -RedirectStandardError (Join-Path $Scratch 'o10.err')
  $started.Add($o10)
  [void]$o10.WaitForExit(120000)
  $i10 = Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch 'inner10.txt')
  Check '10 a compiler that is a sibling of the alone run (another child of the outer holder) is NOT own: exit 3, `alone: NO`, it is named' ($i10 -match 'exit=3' -and $i10 -match 'alone: NO' -and $i10 -match 'wwe2k14_fake_cc') $i10
  # the control (can fail the other way): a compiler that IS the alone run's own child is own: the end line is `held`, never `broken`
  $p10 = Start-Process -FilePath pwsh -ArgumentList @('-NoProfile', '-File', $copy, 'run', '-Alone', '-AloneDrainSeconds', '3', '-Name', 'own10', '--', $cc10, '-n', '5', '127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'own10.out') -RedirectStandardError (Join-Path $Scratch 'own10.err')
  $started.Add($p10)
  [void]$p10.WaitForExit(90000)
  $r10 = (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch 'own10.out')) + (Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch 'own10.err'))
  Check '10b the alone run''s OWN compiler child is not a sighting: `alone: held`, not `broken`' ($r10 -match 'alone: held' -and $r10 -notmatch 'alone: broken') $r10

  # ---- 5/11. note 5 and note 4: the yes line says what admit.exe does; the pause passes admit's machine lock once before the drain ----
  $y = Get-Out 'a1'
  Check '5 the yes line prints `pause until` and whether the installed admit.exe honours the pause' ([bool]($r10 -match 'alone: yes \(.*pause until \d\d:\d\d:\d\d.*admit\.exe honours the pause')) $r10
  $fakeOld = Join-Path $Scratch 'old_admit.exe'
  Copy-Item (Join-Path $env:SystemRoot 'System32\PING.EXE') $fakeOld   # answers `capabilities` with an error: what an admit.exe without the pause does
  $env:WWE_ADMIT_EXE = $fakeOld
  $p5 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_5old') 'a5old'
  [void]$p5.WaitForExit(90000)
  $env:WWE_ADMIT_EXE = $admitExe
  Check '5a with an admit.exe that does not know the pause the yes line says it does NOT honour it' ((Get-Out 'a5old') -match 'alone: yes \(.*admit\.exe does NOT honour the pause') (Get-Out 'a5old')
  foreach ($arm in 'plant', 'real') {
      if ($arm -eq 'plant') { $env:WWE_ALONE_TEST = 'nobarrier' } else { Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue }
      $hold11 = Start-Pipe 'py' @('-I', $interop, 'hold', (Join-Path $admitDir 'admit.lock'))   # an admit grant in flight: the machine lock is held
      Check "11 setup ($arm): the fake grant holds admit's machine lock" ($hold11.StandardOutput.ReadLine() -eq 'locked')
      $b11 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', "alone_11_$arm") "b11$arm"
      $seen = $b11.WaitForExit(40000) -or (Test-Marker "b11$arm")
      $acq = Get-History | Where-Object { $_.event -eq 'acquire' -and $_.name -eq "alone_11_$arm" } | Select-Object -First 1
      $mk = Join-Path $Scratch "marker_b11$arm.txt"
      $gap = if ($acq -and (Test-Path $mk)) { ((Get-Item $mk).LastWriteTime - [datetime]$acq.time).TotalSeconds } else { -1 }
      Set-Variable -Name "gap11$arm" -Value $gap
      $hold11.StandardInput.Close(); [void]$hold11.WaitForExit(30000); [void]$b11.WaitForExit(60000)
  }
  Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue
  Check '11 plant: with the barrier off the run starts within ~2 s of taking the pause although a grant holds admit''s lock (the detector can fail)' ($gap11plant -ge 0 -and $gap11plant -lt 4) "gap $gap11plant s"
  Check '11a the barrier waits for the grant in flight (up to 5 s) before the drain: the run starts >= 4 s after the pause, and later than the plant' ($gap11real -ge 4 -and $gap11real -gt $gap11plant + 1.5) "gap real $gap11real s, plant $gap11plant s"
  # S106 C5FF N5: the yes line says when the barrier gave up (a grant may be in flight and not on disk yet); a free admit lock says nothing
  Check '11b N5 the barrier that timed out says so on the yes line (the fake grant held admit''s lock past 5 s)' ((Get-Out 'b11real') -match 'alone: yes \(.*barrier timed out') (Get-Out 'b11real')
  Check '11c N5 control: with admit''s lock free (own10) the yes line does not say it' ($r10 -match 'alone: yes' -and $r10 -notmatch 'barrier timed out') $r10

  # ---- 12. C5FF R1: ensure_lock(alone=True) starts exactly ONE child (hard bound 60 s; the probe refuses to go past 4 starts, so the old fault ends in a count) ----
  $s12 = Join-Path $Scratch 'ens12'; New-Item -ItemType Directory -Force $s12 | Out-Null
  $rlPyDir = Join-Path $ToolsDir 'harness'
  $p12 = Start-Process -FilePath py -ArgumentList @('-I', (Join-Path $repo 'tools\harness\runlock_alone_unit.py'), '--probe', $s12, $copy, $lockDir, $rlPyDir) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $s12 'out.txt') -RedirectStandardError (Join-Path $s12 'err.txt')
  $started.Add($p12)
  $done12 = $p12.WaitForExit(60000)
  if (-not $done12) { Stop-Tree $p12 }
  $n12 = if (Test-Path (Join-Path $s12 'starts.txt')) { @(Get-Content (Join-Path $s12 'starts.txt')).Count } else { 0 }
  $ran12 = if (Test-Path (Join-Path $s12 'ran.txt')) { (Get-Content -Raw (Join-Path $s12 'ran.txt')) } else { '' }
  Check '12 R1 ensure_lock(alone=True) under a scratch lock dir: the script starts exactly twice (the caller and ONE child), the child continues' ($done12 -and $n12 -eq 2 -and $ran12 -match '^ran \d+' -and $ran12 -notmatch 'BOUND') "done $done12 starts $n12 ran '$ran12' err: $(Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $s12 'err.txt'))"

  # ---- 13. C5FF N2: a STOPPED pipeline (Ctrl+C: finally runs, catch does not) in a long-lived caller gives back the EXCLUSIVE lock and the pause ----
  $cc13 = Join-Path $Scratch 'wwe2k14_fake_cc.exe'
  if (-not (Test-Path $cc13)) { Copy-Item (Join-Path $env:SystemRoot 'System32\PING.EXE') $cc13 }
  $ccp13 = Start-Process -FilePath $cc13 -ArgumentList @('-n', '120', '127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'cc13.out')   # busy for the whole drain: the stop lands inside it
  Set-Content -Path (Join-Path $Scratch 'stopper.ps1') -Value @'
param([string]$Copy, [string]$Scratch, [string]$AloneFile)
$ps = [powershell]::Create()
[void]$ps.AddScript(". '$Copy'; Enter-RunLock -Name 'stopper' -TimeoutMinutes 1 -Mode Exclusive -Alone -AloneDrainSeconds 120 -Quiet")
$a = $ps.BeginInvoke()
for ($k = 0; $k -lt 300 -and ((Get-Item -LiteralPath $AloneFile -ErrorAction SilentlyContinue).Length -le 1); $k++) { Start-Sleep -Milliseconds 100 }   # the pause record is written: the drain is next
Start-Sleep -Milliseconds 1500
$ps.Stop()
Set-Content -Path (Join-Path $Scratch 'stopper_result.txt') -Value 'stopped'
$s = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', 'Local\wwe_alone_st13_S')
$g = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', 'Local\wwe_alone_st13_G')
[void]$s.Set(); [void]$g.WaitOne(300000)
'@
  $e13 = New-Ev 'st13'
  $st = Start-Process -FilePath pwsh -ArgumentList @('-NoProfile', '-File', (Join-Path $Scratch 'stopper.ps1'), $copy, $Scratch, $aloneFile) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch 'st13.out') -RedirectStandardError (Join-Path $Scratch 'st13.err')
  $started.Add($st)
  Check '13 setup: the caller stopped its own pipeline inside the drain (and lives on)' ($e13.S.WaitOne(90000) -and -not $st.HasExited) "$(Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $Scratch 'st13.err'))"
  $sh13 = Start-RL @('-Shared', '-TimeoutMinutes', '0.4', '-Name', 'after_stop') 'n13'
  Check '13a a SHARED request is granted at once after the stop (the exclusive lock was given back)' ($sh13.WaitForExit(60000) -and $sh13.ExitCode -eq 0 -and (Test-Marker 'n13')) "exit $($sh13.ExitCode): $(Get-Out 'n13')"
  $tr13 = Start-Pipe 'py' @('-I', $interop, 'try', $aloneFile)
  Check '13b the pause file is free after the stop' ($tr13.StandardOutput.ReadLine() -eq 'free')
  $e13.G.Set() | Out-Null; [void]$st.WaitForExit(30000)
  Stop-Tree $ccp13

  # ---- 14. C5FF N3: the WWE_ALONE_TEST hooks act only with WWE_ADMIT_TEST=1 (a stray variable must not blind a real run) ----
  $keepAdmitTest = $env:WWE_ADMIT_TEST
  Remove-Item Env:WWE_ADMIT_TEST -ErrorAction SilentlyContinue
  $env:WWE_ALONE_TEST = 'nostart'
  $e14 = New-Ev 'h14'
  $p14 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', 'alone_14') 'h14' -Hold
  [void]$e14.S.WaitOne(90000)
  Start-Sleep -Seconds 3   # (the sampler's first scan)
  $e14.G.Set() | Out-Null
  [void]$p14.WaitForExit(60000)
  Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue
  $env:WWE_ADMIT_TEST = $keepAdmitTest
  Check '14 N3 WWE_ALONE_TEST=nostart without WWE_ADMIT_TEST=1 does nothing: the end line is `held`' ((Get-Out 'h14') -match 'alone: held' -and (Get-Out 'h14') -notmatch 'unverified') (Get-Out 'h14')

  # ---- 15. C5FF N4: one transient scan error (or a tsv write error) does not make the run unverified ----
  foreach ($mode in 'throw_once', 'tsvbad') {
      $env:WWE_ALONE_TEST = $mode
      $e15 = New-Ev "h15$mode"
      $p15 = Start-RL @('-Alone', '-AloneDrainSeconds', '3', '-Name', "alone_15_$mode") "h15$mode" -Hold
      [void]$e15.S.WaitOne(90000)
      Remove-Item Env:WWE_ALONE_TEST -ErrorAction SilentlyContinue
      Start-Sleep -Seconds 5   # the sampler's first scan (hit by the plant), another at ~2 s, ~4 s
      $e15.G.Set() | Out-Null
      [void]$p15.WaitForExit(60000)
      Check "15 N4 '$mode': a single transient failure leaves the end line held" ((Get-Out "h15$mode") -match 'alone: held' -and (Get-Out "h15$mode") -notmatch 'unverified') (Get-Out "h15$mode")
  }
} finally {
    foreach ($p in $started) { try { if (-not $p.HasExited) { & taskkill.exe /F /T /PID $p.Id *> $null } } catch { } }
    Remove-Item Env:WWE_ADMIT_GRACE_S -ErrorAction SilentlyContinue
}
''
if ($fails) { "alone_selftest: $fails FAIL"; exit 1 }
'alone_selftest: all PASS'
exit 0
