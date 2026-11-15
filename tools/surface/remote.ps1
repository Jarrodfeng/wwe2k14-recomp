# Session 80 (HD 520 arc, item 3): drives the Surface bench from the PC over SSH (host alias "surface"). The game must
# run in Jarrod's desktop session, not SSH's hidden service session, so a scheduled task 'wwe_bench' (interactive logon:
# runs only while he is logged in, no password stored; Jarrod's OK 2026-11-01) starts C:\wwe\run_bench.ps1 there.
#   pwsh -NoProfile -File tools\surface\remote.ps1 session        who is logged in (the task needs his session)
#   ... register                                                    create the task
#   ... start -BenchArgs 'Only=passes,frames;ColdDriver=true'       write C:\wwe\bench_args.txt, start the task
#   ... status                                                      bench_status.txt, the task, the transcript's end
#   ... fetch [-To out\s80\surface]                                 copy the newest C:\wwe\out\surface_bench_*.zip here
#   ... unregister                                                  delete the task (at the end of the session)
param([Parameter(Mandatory, Position = 0)][ValidateSet('session', 'register', 'start', 'status', 'fetch', 'unregister')]
      [string]$Action, [string]$BenchArgs = '', [string]$To = '', [string]$HostName = 'surface')
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

. (Join-Path $PSScriptRoot 'wer_lib.ps1')   # S102 TS1: Invoke-Timed. Every ssh / scp here is its own process with a hard timeout (a bare ssh call can hang the job runner for good).
# Limits: start / status / session / register 60 s, fetch 600 s (a ~460 MB zip), unless WWE_REMOTE_TIMEOUT_SEC says otherwise (the planted test sets 3).
# WWE_REMOTE_FAKE=<script> swaps ssh / scp for that script (tools\surface\wer_fake_ssh.ps1; WWE_REMOTE_FAKE_MODE its first argument): the offline test only.
$script:ShortSec = if ($env:WWE_REMOTE_TIMEOUT_SEC) { [int]$env:WWE_REMOTE_TIMEOUT_SEC } else { 60 }
$script:FetchSec = if ($env:WWE_REMOTE_TIMEOUT_SEC) { [int]$env:WWE_REMOTE_TIMEOUT_SEC } else { 600 }
if ($env:WWE_REMOTE_FAKE) {
  $fk = @('-NoProfile', '-File', $env:WWE_REMOTE_FAKE, $(if ($env:WWE_REMOTE_FAKE_MODE) { $env:WWE_REMOTE_FAKE_MODE } else { 'hang' }))
  $script:WerSshExe = 'pwsh'; $script:WerSshPre = $fk; $script:WerScpExe = 'pwsh'; $script:WerScpPre = $fk
}
# Returns the output lines. Throws "ssh timed out after N s (killed)" / "ssh failed (exit E)" so the caller can say so; a non-zero exit of the REMOTE command is not an error here
# (ssh reports it as that exit code: only 255 = the connection itself failed is treated as a failure), as before.
function Invoke-Remote([string]$Code, [int]$TimeoutSec = $script:ShortSec) {
  $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Code))
  $r = Invoke-Timed $script:WerSshExe (@($script:WerSshPre) + $script:SshBase + @($HostName, "powershell -NoProfile -EncodedCommand $enc")) -TimeoutSec $TimeoutSec
  if ($r.TimedOut) { throw "ssh to $HostName timed out after $TimeoutSec s (killed)" }
  if ($r.Exit -eq 255 -or $r.Exit -lt 0) { throw "ssh to $HostName failed (exit $($r.Exit)): $(($r.Err -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1))" }
  @($r.Out -split "`r?`n" | Where-Object { $_ -ne '' })
}
function Invoke-Scp([string[]]$ScpArgs, [int]$TimeoutSec) {
  $r = Invoke-Timed $script:WerScpExe (@($script:WerScpPre) + @('-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10') + $ScpArgs) -TimeoutSec $TimeoutSec
  if ($r.TimedOut) { throw "scp timed out after $TimeoutSec s (killed)" }
  if ($r.Exit -ne 0) { throw "scp failed (exit $($r.Exit)): $(($r.Err -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1))" }
}

switch ($Action) {
  'session' { Invoke-Remote 'whoami; Get-CimInstance Win32_ComputerSystem | ForEach-Object { "desktop user: " + $_.UserName }' }
  'register' {
    Invoke-Remote @'
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File C:\wwe\run_bench.ps1'
$pr = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Hours 4)
Register-ScheduledTask -TaskName wwe_bench -Action $a -Principal $pr -Settings $s -Force | Select-Object TaskName, State | Format-List
'@
  }
  'start' {
    # S102 main (2026-11-14 23:05): a multi-arm job's args (6 arms of flags) encoded into the command made ~17,000 characters, past the
    # Surface shell's ~8,191 limit: the start failed silently and the runner then trusted the previous job's "done". The args now travel
    # as a file (scp), one line each, so the command stays short whatever the job size.
    $argFile = Join-Path ([IO.Path]::GetTempPath()) ("wwe_bench_args_" + $PID + ".txt")
    [IO.File]::WriteAllLines($argFile, [string[]]@($BenchArgs -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }))
    try { Invoke-Scp @($argFile, "${HostName}:C:/wwe/bench_args.txt") $script:ShortSec }
    catch { throw "start: copying bench_args.txt to the Surface failed: $($_.Exception.Message)" }
    finally { Remove-Item $argFile -ErrorAction SilentlyContinue }
    Invoke-Remote ("Remove-Item C:\wwe\bench_status.txt -ErrorAction SilentlyContinue; Start-ScheduledTask -TaskName wwe_bench; " +
                   "Start-Sleep 5; 'task: ' + (Get-ScheduledTask wwe_bench).State; " +
                   "'status: ' + (Get-Content C:\wwe\bench_status.txt -ErrorAction SilentlyContinue); " +
                   "'args: ' + ((Get-Content C:\wwe\bench_args.txt) -join ' ')")
  }
  'status' {
    # S102 TS1: a status that cannot be read (ssh timed out / failed) is "status: unknown (why)", exit 0: the runner's poll loop treats it as "try again next poll", never as done / failed.
    try {
      Invoke-Remote @'
'status: ' + (Get-Content C:\wwe\bench_status.txt -ErrorAction SilentlyContinue)
'task: ' + (Get-ScheduledTask wwe_bench -ErrorAction SilentlyContinue).State
if (Test-Path C:\wwe\bench_transcript.txt) { Get-Content C:\wwe\bench_transcript.txt -Tail 12 }
'@
    } catch { "status: unknown ($($_.Exception.Message))" }
  }
  'fetch' {
    $zip = "$(Invoke-Remote '(Get-ChildItem C:\wwe\out -Filter surface_bench_*.zip | Sort-Object LastWriteTime | Select-Object -Last 1).FullName')".Trim()
    if (-not $zip) { throw 'no surface_bench_*.zip in C:\wwe\out' }
    $dest = if ($To) { $To } else { Join-Path $R 'out\s80\surface' }
    New-Item -ItemType Directory -Force $dest | Out-Null
    Invoke-Scp @("${HostName}:$($zip -replace '\\', '/')", "$dest\") $script:FetchSec
    Join-Path $dest (Split-Path $zip -Leaf)
  }
  'unregister' { Invoke-Remote 'Unregister-ScheduledTask -TaskName wwe_bench -Confirm:$false; "task wwe_bench deleted"' }
}
