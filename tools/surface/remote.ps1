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

function Invoke-Remote([string]$Code) {
  $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Code))
  ssh -o BatchMode=yes -o ConnectTimeout=10 $HostName "powershell -NoProfile -EncodedCommand $enc"
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
    $vals = @($BenchArgs -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ } |
              ForEach-Object { "'" + ($_ -replace "'", "''") + "'" })
    $value = if ($vals) { $vals -join ',' } else { "''" }
    Invoke-Remote ("Set-Content C:\wwe\bench_args.txt -Value @($value); " +
                   "Remove-Item C:\wwe\bench_status.txt -ErrorAction SilentlyContinue; Start-ScheduledTask -TaskName wwe_bench; " +
                   "Start-Sleep 5; 'task: ' + (Get-ScheduledTask wwe_bench).State; " +
                   "'status: ' + (Get-Content C:\wwe\bench_status.txt -ErrorAction SilentlyContinue); " +
                   "'args: ' + ((Get-Content C:\wwe\bench_args.txt) -join ' ')")
  }
  'status' {
    Invoke-Remote @'
'status: ' + (Get-Content C:\wwe\bench_status.txt -ErrorAction SilentlyContinue)
'task: ' + (Get-ScheduledTask wwe_bench -ErrorAction SilentlyContinue).State
if (Test-Path C:\wwe\bench_transcript.txt) { Get-Content C:\wwe\bench_transcript.txt -Tail 12 }
'@
  }
  'fetch' {
    $zip = "$(Invoke-Remote '(Get-ChildItem C:\wwe\out -Filter surface_bench_*.zip | Sort-Object LastWriteTime | Select-Object -Last 1).FullName')".Trim()
    if (-not $zip) { throw 'no surface_bench_*.zip in C:\wwe\out' }
    $dest = if ($To) { $To } else { Join-Path $R 'out\s80\surface' }
    New-Item -ItemType Directory -Force $dest | Out-Null
    scp -q -o BatchMode=yes "${HostName}:$($zip -replace '\\', '/')" "$dest\"
    if ($LASTEXITCODE) { throw "scp failed ($LASTEXITCODE)" }
    Join-Path $dest (Split-Path $zip -Leaf)
  }
  'unregister' { Invoke-Remote 'Unregister-ScheduledTask -TaskName wwe_bench -Confirm:$false; "task wwe_bench deleted"' }
}
