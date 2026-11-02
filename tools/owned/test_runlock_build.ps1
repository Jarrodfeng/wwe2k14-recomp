# Isolated test of the BUILD class in tools\runlock.ps1 (Session 82). Copies the script into a scratch folder with its
# own lock directory (no interference with the real run lock) and plays scenarios with real child processes.
# Prints PASS/FAIL per scenario; exit 1 on any FAIL.   pwsh -NoProfile -File tools\owned\test_runlock_build.ps1
param([string]$Scratch = (Join-Path ([System.IO.Path]::GetTempPath()) 'runlock_build_test'))
$ErrorActionPreference = 'Stop'
$src = 'C:\Users\Jarrod\wwe2k14-recomp\tools\runlock.ps1'
if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
New-Item -ItemType Directory -Force $Scratch | Out-Null
$lockDir = Join-Path $Scratch 'lock'
$copy = Join-Path $Scratch 'runlock.ps1'
$text = Get-Content -Raw $src
$text = $text.Replace("'C:\Users\Jarrod\wwe2k14-recomp\out\harness\runlock'", "'$lockDir'").Replace("@('wwe2k14')", "@('wwe2k14_nonexistent_test')")
if ($text -notmatch [regex]::Escape($lockDir)) { throw 'lock dir not substituted' }
Set-Content -Path $copy -Value $text -NoNewline
$fails = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if ($ok) { "PASS  $what" } else { $script:fails++; "FAIL  $what  $detail" }
}
# Starts "runlock run <mode> -Name N -- pwsh -c <body>" in the background; the body appends 'N start/end' stamps to a trace file.
$trace = Join-Path $Scratch 'trace.txt'
function Start-Hold([string]$name, [string]$mode, [double]$seconds) {
    # (Several processes appending to one file at the same instant lose lines: one stamp file per hold, tr_<name>.txt.)
    $stamp = "function Stamp(`$w) { for (`$i = 0; `$i -lt 40; `$i++) { try { Add-Content '$Scratch\tr_$name.txt' (('{0:HH:mm:ss.fff} $name ' -f (Get-Date)) + `$w) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 25 } } }; "
    $body = $stamp + "Stamp 'start'; Start-Sleep -Milliseconds $([int]($seconds * 1000)); Stamp 'end'"
    $a = @('-NoProfile', '-File', $copy, 'run') + $(if ($mode) { @($mode) } else { @() }) + @('-Name', $name, '--', 'pwsh', '-NoProfile', '-Command', $body)
    Start-Process -FilePath pwsh -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Scratch "$name.err") -RedirectStandardOutput (Join-Path $Scratch "$name.out")
}
function Get-Stamp([string]$name, [string]$what) {
    $l = Get-Content "$Scratch\tr_$name.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -match " $name $what$" } | Select-Object -First 1
    if ($l) { [datetime]::ParseExact(($l -split ' ')[0], 'HH:mm:ss.fff', $null) } else { $null }
}
function Wait-All($procs) { foreach ($p in $procs) { $p | Wait-Process -Timeout 120 } }
function Reset-Trace { if (Test-Path $trace) { Remove-Item $trace -Force }; Remove-Item "$Scratch\tr_*.txt" -Force -ErrorAction SilentlyContinue }

# 1. two builds at once, a third waits for a free build slot
Reset-Trace
$p = @((Start-Hold 'b1' '-Build' 6), (Start-Hold 'b2' '-Build' 6)); Start-Sleep -Milliseconds 1500
$p += (Start-Hold 'b3' '-Build' 1); Wait-All $p
$b1s = Get-Stamp b1 start; $b2s = Get-Stamp b2 start; $b1e = Get-Stamp b1 end; $b2e = Get-Stamp b2 end; $b3s = Get-Stamp b3 start
Check '1a two builds run at the same time' ($b1s -and $b2s -and $b2s -lt $b1e -and $b1s -lt $b2e)
Check '1b a third build waits for a slot' ($b3s -and $b3s -ge ([datetime]::Compare($b1e, $b2e) -lt 0 ? $b1e : $b2e).AddMilliseconds(-100)) "b3 start $b3s b1 end $b1e b2 end $b2e"

# 2. builds coexist with a shared hold
Reset-Trace
$p = @((Start-Hold 's1' '-Shared' 7)); Start-Sleep -Milliseconds 1500; $p += (Start-Hold 'b1' '-Build' 5); $p += (Start-Hold 's2' '-Shared' 2); Wait-All $p
$s1e = Get-Stamp s1 end; $b1s = Get-Stamp b1 start; $b1e = Get-Stamp b1 end; $s2s = Get-Stamp s2 start
Check '2a a build starts while a shared hold is held' ($b1s -and $b1s -lt $s1e)
Check '2b a shared hold starts while a build is held' ($s2s -and $s2s -lt $b1e)

# 3. exclusive excluded by a build, build excluded by exclusive
Reset-Trace
$p = @((Start-Hold 'b1' '-Build' 5)); Start-Sleep -Milliseconds 1500; $p += (Start-Hold 'x1' '' 2); Wait-All $p
$b1e = Get-Stamp b1 end; $x1s = Get-Stamp x1 start
Check '3a an exclusive hold waits for the build to end' ($x1s -and $x1s -ge $b1e.AddMilliseconds(-100)) "x1 start $x1s b1 end $b1e"
Reset-Trace
$p = @((Start-Hold 'x1' '' 5)); Start-Sleep -Milliseconds 1500; $p += (Start-Hold 'b1' '-Build' 1); Wait-All $p
$x1e = Get-Stamp x1 end; $b1s = Get-Stamp b1 start
Check '3b a build waits for an exclusive hold to end' ($b1s -and $b1s -ge $x1e.AddMilliseconds(-100)) "b1 start $b1s x1 end $x1e"

# 4. a build does not overtake a queued exclusive waiter (no starvation of timed runs)
Reset-Trace
$p = @((Start-Hold 'b1' '-Build' 5)); Start-Sleep -Milliseconds 1200
$p += (Start-Hold 'x1' '' 1); Start-Sleep -Milliseconds 1200; $p += (Start-Hold 'b2' '-Build' 1); Wait-All $p
$x1s = Get-Stamp x1 start; $x1e = Get-Stamp x1 end; $b2s = Get-Stamp b2 start; $b1e = Get-Stamp b1 end
Check '4 queued exclusive goes before a later build' ($x1s -and $b2s -and $x1s -lt $b2s -and $b2s -ge $x1e.AddMilliseconds(-100)) "x1 $x1s..$x1e b2 $b2s b1 end $b1e"

# 5. unchanged: 3 shared at once, the 4th waits; exclusive after shared
Reset-Trace
$p = @(1..3 | ForEach-Object { Start-Hold "s$_" '-Shared' 5 }); Start-Sleep -Milliseconds 2000; $p += (Start-Hold 's4' '-Shared' 1); Wait-All $p
$ends = @(1..3 | ForEach-Object { Get-Stamp "s$_" 'end' }) | Sort-Object; $s4s = Get-Stamp s4 start
Check '5 shared: 4th waits for a slot (unchanged)' ($s4s -and $s4s -ge $ends[0].AddMilliseconds(-100)) "s4 start $s4s first end $($ends[0])"

# 6. re-entry: a child of a build hold re-enters it; exclusive inside a build throws; shared inside a build re-enters
$inner = Join-Path $Scratch 'inner.ps1'
@"
. '$copy'
`$h = Enter-RunLock -Name 'child-build' -Mode Build -Quiet
Add-Content '$trace' ('child-build reentrant=' + `$h.Reentrant + ' mode=' + `$h.Mode)
try { `$h2 = Enter-RunLock -Name 'child-shared' -Mode Shared -Quiet; Add-Content '$trace' ('child-shared NO-THROW mode=' + `$h2.Mode) } catch { Add-Content '$trace' 'child-shared threw' }
try { Enter-RunLock -Name 'child-excl' -Mode Exclusive -Quiet | Out-Null; Add-Content '$trace' 'child-excl NO-THROW' } catch { Add-Content '$trace' 'child-excl threw' }
"@ | Set-Content $inner
Reset-Trace
$a = @('-NoProfile', '-File', $copy, 'run', '-Build', '-Name', 'parent', '--', 'pwsh', '-NoProfile', '-File', $inner)
Start-Process pwsh -ArgumentList $a -Wait -WindowStyle Hidden -RedirectStandardError (Join-Path $Scratch 'parent.err') -RedirectStandardOutput (Join-Path $Scratch 'parent.out')
$t = (Get-Content $trace -ErrorAction SilentlyContinue) -join ' | '
Check '6a child re-enters the build hold' ($t -match 'child-build reentrant=True mode=Build') $t
Check '6b a shared request inside a build THROWS (a build runs no games)' ($t -match 'child-shared threw') $t
Check '6c an exclusive request inside a build throws' ($t -match 'child-excl threw') $t

# 7. status shows the build slots; the history logs mode build
Reset-Trace
$p = @((Start-Hold 'b1' '-Build' 4)); Start-Sleep -Milliseconds 1800
$st = (& pwsh -NoProfile -File $copy status) -join "`n"
Wait-All $p
Check '7a status lists the live build hold' ($st -match "build1: 'b1'") $st
$hist = Get-Content (Join-Path $lockDir 'history.tsv') -ErrorAction SilentlyContinue
Check '7b history has build acquire/release rows' (@($hist | Where-Object { $_ -match "`tbuild`t" }).Count -ge 2)
$rep = (& pwsh -NoProfile -File $copy report -Hours 1 -NoLogs) -join "`n"
Check '7c report runs and counts build holds' ($rep -match 'build') $rep.Substring(0, [Math]::Min(600, $rep.Length))

# 9. a killed build holder is taken over (no release, the process is gone)
Reset-Trace
$k1 = Start-Hold 'k1' '-Build' 30; $k2 = Start-Hold 'k2' '-Build' 30; Start-Sleep -Milliseconds 3500
$kids = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($k1.Id)" | ForEach-Object ProcessId)
Stop-Process -Id $k1.Id -Force; foreach ($c in $kids) { Stop-Process -Id $c -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800
$k3 = Start-Hold 'k3' '-Build' 1; $k3 | Wait-Process -Timeout 60
$k3s = Get-Stamp k3 start
Check '9a a third build takes over the slot of a killed build holder' ($k3s -and -not (Get-Stamp k2 end)) "k3 start $k3s"
$st = (& pwsh -NoProfile -File $copy status) -join "`n"
Check '9b status shows the survivor only' ($st -match "build\d: 'k2'") $st
Stop-Process -Id $k2.Id -Force -ErrorAction SilentlyContinue
foreach ($c in @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($k2.Id)" | ForEach-Object ProcessId)) { Stop-Process -Id $c -Force -ErrorAction SilentlyContinue }
Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | Where-Object { $_.CommandLine -match [regex]::Escape($Scratch) -and $_.CommandLine -match "Name', 'k|Name k" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 1
& pwsh -NoProfile -File $copy clear-stale | Out-Null

# 10. a shared waiter queued behind a queued build does not wait for a build to end
Reset-Trace
$p = @((Start-Hold 'b1' '-Build' 8), (Start-Hold 'b2' '-Build' 8)); Start-Sleep -Milliseconds 2500
$p += (Start-Hold 'b3' '-Build' 1); Start-Sleep -Milliseconds 1200; $p += (Start-Hold 's1' '-Shared' 1); Wait-All $p
$s1s = Get-Stamp s1 start; $b1e = Get-Stamp b1 end; $b2e = Get-Stamp b2 end
Check '10 a shared hold queued behind a queued build starts while both builds still run' ($s1s -and $s1s -lt $b1e -and $s1s -lt $b2e) "s1 start $s1s b1 end $b1e b2 end $b2e"

# 11. 3 shared + 2 builds all at once
Reset-Trace
$p = @(1..3 | ForEach-Object { Start-Hold "s$_" '-Shared' 9 }) + @(1..2 | ForEach-Object { Start-Hold "b$_" '-Build' 9 }); Wait-All $p
$starts = @('s1', 's2', 's3', 'b1', 'b2' | ForEach-Object { Get-Stamp $_ 'start' })
$ends = @('s1', 's2', 's3', 'b1', 'b2' | ForEach-Object { Get-Stamp $_ 'end' })
$ok = (@($starts | Where-Object { $_ }).Count -eq 5) -and (($starts | Sort-Object -Descending | Select-Object -First 1) -lt ($ends | Sort-Object | Select-Object -First 1))
Check '11 three shared holds and two builds run at the same time' $ok "starts $($starts -join ' ') ends $($ends -join ' ')"

# 12. Python ensure_lock(build=True): a copy of runlock.py pointed at the scratch lock dir
$pyCopy = Join-Path $Scratch 'runlock.py'
$pytext = (Get-Content -Raw 'C:\Users\Jarrod\wwe2k14-recomp\tools\harness\runlock.py').Replace("RUNLOCK_PS1 = os.path.join(REPO, 'tools', 'runlock.ps1')", "RUNLOCK_PS1 = r'$copy'").Replace("LOCK_DIR = os.path.join(REPO, 'out', 'harness', 'runlock')", "LOCK_DIR = r'$lockDir'")
if ($pytext -notmatch [regex]::Escape($lockDir)) { throw 'python copy: lock dir not substituted' }
Set-Content -Path $pyCopy -Value $pytext -NoNewline
$child = Join-Path $Scratch 'pychild.py'
Set-Content -Path $child -Value @"
import sys
sys.path.insert(0, r'$Scratch')
import runlock
mode = sys.argv[1]
if mode == 'shared':
    runlock.ensure_lock('pychild', shared=True)
elif mode == 'build':
    runlock.ensure_lock('pychild', build=True)
else:
    runlock.ensure_lock('pychild')
print('child-' + mode + ' returned held=' + runlock.held_by_us())
"@
$parent = Join-Path $Scratch 'pyparent.py'
Set-Content -Path $parent -Value @"
import subprocess, sys
sys.path.insert(0, r'$Scratch')
import runlock
runlock.ensure_lock('pyparent', build=True)
print('parent held=' + runlock.held_by_us())
for m in ('build', 'shared', 'excl'):
    r = subprocess.run([sys.executable, r'$child', m], capture_output=True, text=True)
    print('child-%s exit=%d %s' % (m, r.returncode, r.stdout.strip()))
"@
$po = (& py -I $parent 2>&1) -join ' | '
Check '12a ensure_lock(build=True) takes a build hold' ($po -match 'parent held=build') $po
Check '12b a build child re-enters it' ($po -match 'child-build exit=0 .*held=build') $po
Check '12c a shared child inside a build exits 1' ($po -match 'child-shared exit=1') $po
Check '12d an exclusive child inside a build exits 1' ($po -match 'child-excl exit=1') $po

# 8. clear-stale / leftover: nothing left behind
$left = (& pwsh -NoProfile -File $copy status) -join "`n"
Check '8 no holder left at the end' ($left -match 'mode: free') $left
"failures: $fails"
exit $(if ($fails) { 1 } else { 0 })
