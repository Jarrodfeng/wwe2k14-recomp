# build_watchdog.ps1 (QTF review C1): kills a build that overran. Started by build_rig.ps1 (pwsh -File) with the build's pid, its start time (ticks) and the limit.
# It holds a process handle (GetProcessById + WaitForExit), so a reused pid is never touched, and a start-time check refuses a different process.
# On expiry it kills ONLY clang++ / clang / lld / ninja descendants of the target (each verified to be newer than its parent), then the target itself
# (a dedicated `pwsh -File` process). Never taskkill /T on anything, never a shell it was not started for.
param([int]$TargetPid, [long]$StartTicks, [int]$Secs)
$ErrorActionPreference = 'SilentlyContinue'
$t = [Diagnostics.Process]::GetProcessById($TargetPid)
if (-not $t -or $t.StartTime.Ticks -ne $StartTicks) { exit 0 }
if ($t.WaitForExit($Secs * 1000)) { exit 0 }
$names = 'clang++.exe', 'clang.exe', 'clang-cl.exe', 'clang-cpp.exe', 'lld-link.exe', 'ld.lld.exe', 'ninja.exe'
$all = @(Get-CimInstance Win32_Process)
$todo = New-Object System.Collections.Queue
$todo.Enqueue([pscustomobject]@{ Id = $TargetPid; Started = $t.StartTime })
$victims = @()
while ($todo.Count) {
  $cur = $todo.Dequeue()
  foreach ($c in ($all | Where-Object { $_.ParentProcessId -eq $cur.Id -and $_.CreationDate -ge $cur.Started })) {
    if ($c.Name -eq 'sccache.exe') { continue }   # QTF: a shared cache server is never walked into and never killed
    $todo.Enqueue([pscustomobject]@{ Id = $c.ProcessId; Started = $c.CreationDate })
    if ($names -contains $c.Name) { $victims += $c }
  }
}
foreach ($v in $victims) { Stop-Process -Id $v.ProcessId -Force }
$t.Kill()
