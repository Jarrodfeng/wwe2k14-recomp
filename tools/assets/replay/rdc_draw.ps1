# Frame replay (Session 98): dumps whole draws (constants, textures, post-VS, the target before / after) from a RenderDoc capture
# through rdc_draw.py (qrenderdoc --python). No game is started. Hard timeout -Minutes (default 10).
#   & .\tools\assets\replay\rdc_draw.ps1 -Rdc out\s98\replay\rp_ringin_frame9452.rdc -Out out\s98\replay\s3s4\rp_ringin\eyes -ICounts 1516 [-PsMd5 a90a5f43,c9c6b597]
#   & .\tools\assets\replay\rdc_draw.ps1 -Rdc <rdc> -Out <dir> -List      every draw's event / index count / shader md5s (draws_all.json)
param([Parameter(Mandatory)][string]$Rdc, [Parameter(Mandatory)][string]$Out, [string]$ICounts = '', [string]$PsMd5 = '', [switch]$List,
      [int]$Minutes = 10, [string]$Pixels = '', [string]$PhEvent = '')
$rdDir = 'C:\Program Files\RenderDoc'
$Rdc = [System.IO.Path]::GetFullPath($Rdc)
$Out = [System.IO.Path]::GetFullPath($Out)
$null = New-Item -ItemType Directory -Force -Path $Out
Remove-Item "$Out\DONE" -ErrorAction SilentlyContinue
$env:RDC = $Rdc; $env:OUT = $Out; $env:ICOUNTS = $ICounts; $env:PSMD5 = $PsMd5; $env:PIXELS = $Pixels; $env:PH_EVENT = $PhEvent; $env:LIST = $(if ($List) { '1' } else { '0' })
$py = Join-Path $PSScriptRoot 'rdc_draw.py'
$p = Start-Process -FilePath "$rdDir\qrenderdoc.exe" -ArgumentList @('--python', $py) -PassThru
$deadline = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $deadline -and -not (Test-Path "$Out\DONE") -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Get-Content "$Out\rdc_draw.log" -ErrorAction SilentlyContinue | Select-Object -Last 12
if (-not (Test-Path "$Out\DONE")) { "rdc_draw: timed out or crashed ($Out)"; exit 4 }
exit 0
