# S102 lane H1: builds tools\tests\thread_names_test.cpp, runs it, samples it with tools\perf\thread_busy.ps1 (-ProcessName thread_names_test) and checks the
# CSV: the two named threads show their description, the planted unnamed twin shows '-' (so the column can tell them apart), and the exe's own read-back passed.
# 0 game runs. Windows only.
param([string]$Out = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
if (-not $Out) { $Out = Join-Path $root 'out\thread_names_test' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'thread_names_test.exe'; $csv = Join-Path $Out 'threads.csv'; $so = Join-Path $Out 'stdout.txt'
Remove-Item $csv, $so -ErrorAction SilentlyContinue
& clang++ -std=c++20 -O1 "-I$root\src" "$PSScriptRoot\thread_names_test.cpp" -o $exe -lkernel32
if ($LASTEXITCODE -ne 0) { "compile failed"; exit 2 }
$p = Start-Process -FilePath $exe -ArgumentList '8' -PassThru -RedirectStandardOutput $so -WindowStyle Hidden
& pwsh -NoProfile -File "$root\tools\perf\thread_busy.ps1" -Out $csv -ProcessName thread_names_test -ExePath $exe -WaitForStart 10 -IntervalMs 500 | Out-Null
$null = $p.WaitForExit(20000)
$bad = 0
"exe said: $((Get-Content $so -Raw).Trim())"
if ($p.ExitCode -ne 0) { "FAIL exe exit $($p.ExitCode)"; $bad++ }
if (-not (Test-Path $csv)) { "FAIL no CSV"; exit 1 }
$rows = Import-Csv $csv
$busy = $rows | Where-Object { [double]$_.mcycles_fixedrate -gt 1 }  # fixed-rate cycles: cpu_ms moves in 15.6 ms ticks and reads 0.0 for a light thread
foreach ($want in 'h1_test_alpha', 'rdr_h1_test_beta') {
    $n = @($busy | Where-Object { $_.name -eq $want } | Select-Object -ExpandProperty tid -Unique).Count
    if ($n -ne 1) { "FAIL thread_busy.ps1 shows '$want' on $n threads (want 1)"; $bad++ } else { "ok   '$want' read back in the CSV" }
}
$unnamed = @($busy | Where-Object { $_.name -eq '-' } | Select-Object -ExpandProperty tid -Unique).Count
if ($unnamed -lt 1) { "FAIL the planted unnamed busy thread did not show as '-'"; $bad++ } else { "ok   the planted unnamed twin shows '-' ($unnamed thread)" }
if ($bad) { "thread_names_test: $bad problem(s)"; exit 1 }
"thread_names_test: 0 problems"; exit 0
