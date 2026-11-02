# Dumps one pass's inputs and real output from a RenderDoc capture (rdc_dump_pass.py through qrenderdoc.exe) for the offline pass
# tests. No game is started. RenderDoc 1.46 at C:\Program Files\RenderDoc (its first launch may show a popup: ask Jarrod once).
#   & .\tools\native\compute_test\dump_pass.ps1 -Rdc out\texdiff\<stamp>\on.rdc -Draw -3 [-Out out\native\compute_test\dump_x]
#   & .\tools\native\compute_test\dump_pass.ps1 -Rdc <rdc> -Ps 74f6590fb8 [-Nth 0]      the Nth draw using that pixel shader (texdiff's hashes)
#   & .\tools\native\compute_test\dump_pass.ps1 -Rdc <rdc> -Event 10854
# Writes manifest.json + raw .bin files into -Out (default out\native\compute_test\dump_<time>); prints the manifest.
param([Parameter(Mandatory)][string]$Rdc, [string]$Draw = '', [string]$Ps = '', [int]$Nth = 0, [string]$Event = '', [string]$Out = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$rdDir = 'C:\Program Files\RenderDoc'
if (-not (Test-Path "$rdDir\qrenderdoc.exe")) { "dump_pass: RenderDoc is not installed at $rdDir"; exit 2 }
$Rdc = [System.IO.Path]::GetFullPath((Join-Path $R $Rdc))
if (-not (Test-Path $Rdc)) { "dump_pass: no capture $Rdc"; exit 2 }
if (-not $Out) { $Out = "$R\out\native\compute_test\dump_$((Get-Date).ToString('HHmmss'))" }
$Out = [System.IO.Path]::GetFullPath((Join-Path $R $Out))
$null = New-Item -ItemType Directory -Force -Path $Out
Remove-Item "$Out\manifest.json", "$Out\FAILED" -ErrorAction SilentlyContinue
$env:RDC = $Rdc; $env:OUT = $Out; $env:DRAW = $Draw; $env:PS = $Ps; $env:NTH = "$Nth"; $env:EVENT = $Event
if (-not $Draw) { Remove-Item Env:DRAW }
if (-not $Ps) { Remove-Item Env:PS }
if (-not $Event) { Remove-Item Env:EVENT }
$p = Start-Process -FilePath "$rdDir\qrenderdoc.exe" -ArgumentList @('--python', "$R\tools\native\compute_test\rdc_dump_pass.py") -PassThru
$deadline = (Get-Date).AddMinutes(8)
while ((Get-Date) -lt $deadline -and -not (Test-Path "$Out\manifest.json") -and -not (Test-Path "$Out\FAILED") -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path "$Out\manifest.json")) { "dump_pass: no manifest ($Out\dump.log)"; Get-Content "$Out\dump.log" -ErrorAction SilentlyContinue | Select-Object -Last 8; exit 4 }
Get-Content "$Out\dump.log" | Select-Object -First 4
py -I -c "import json,sys; m=json.load(open(sys.argv[1])); print('event', m['event']); [print('  %-14s %-34s %5dx%-5d %-22s cfmt %-8s %d bytes' % (f['file'], f['role'], f['w'], f['h'], f['fmt'], f['cfmt'] or '-', f['bytes'])) for f in m['files']]" "$Out\manifest.json"
"dumped to $Out"
exit 0
