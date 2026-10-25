# Phone-side load sampler for one run (Session 64, SESSION64_PLAN.md step 1): while the game runs, every few seconds the
# busiest threads (top -H, CPU % over one second, the core each ran on last), both CPU clusters' clocks, the thermal
# status and the CPU / GPU / skin temperatures. Start it next to run.ps1 (its own background task); it waits for the app
# and stops when the app ends. Output: out\android\runs\NNN\samples.txt; summary: py tools\android\samples_report.py NNN
#   pwsh -File tools\android\sample.ps1 -Run 025
param([string]$Run, [int]$Every = 3, [int]$WaitSeconds = 180, [int]$Threads = 14)
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = 'Continue'
if (-not $Run) { "sample.ps1: -Run NNN is required"; exit 2 }
$out = "$($AndroidDirs.Runs)\$Run"
$file = "$out\samples.txt"

$t0 = Get-Date
$appPid = ''
while (-not $appPid -and ((Get-Date) - $t0).TotalSeconds -lt $WaitSeconds) {
    $appPid = ((& $Adb shell pidof $Pkg) -as [string]).Trim()
    if (-not $appPid) { Start-Sleep -Seconds 1 }
}
if (-not $appPid) { "sample.ps1: $Pkg did not start within $WaitSeconds s"; exit 3 }
# (only now: run.ps1 refuses a run whose folder already exists, and it makes the folder before it starts the app)
[void](New-Item -ItemType Directory -Force $out)
"sample.ps1: sampling pid $appPid every $Every s into $file"

# One phone-side command per sample (one adb round trip). top's own -d 1 makes each sample a one-second CPU window.
$cmd = "echo ==== `$(date +%s.%N); top -H -b -n 1 -d 1 -p $appPid -o TID,%CPU,CPU,CMD -s 2 | grep -E '^ *[0-9]' | head -n $Threads; " +
       "echo freq `$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq) `$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq); " +
       "dumpsys thermalservice | grep -E 'Thermal Status|HAL:|mName=(CPU[0-7]|GPU[0-7]|skin|socd),' | sed -n '/temperatures from HAL/,/cooling devices/p;/Thermal Status/p' | grep -E 'Status|mName' | head -n 24"
while (((& $Adb shell pidof $Pkg) -as [string]) -match '\d') {
    $s = Get-Date
    & $Adb shell $cmd | Add-Content -Path $file -Encoding utf8
    $wait = $Every - ((Get-Date) - $s).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
}
"sample.ps1: app ended after $([int]((Get-Date) - $t0).TotalSeconds) s"
