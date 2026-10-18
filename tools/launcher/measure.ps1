# Launcher cost: CPU (all cores, % of one core) and GPU engine use of a test instance, idle on the Trons grid and while
# one preview plays (--hover). Read-only for the user's settings (--no-save). Usage:
#   pwsh -NoProfile -File tools\launcher\measure.ps1 [-Seconds 8] [-Tron 343]
param([int]$Seconds = 8, [int]$Tron = 343, [string]$Label = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$exe = "$R\out\build\win-amd64-release\wwe2k14_launcher.exe"

function Measure-Instance([string]$argsLine, [string]$what) {
    $p = Start-Process $exe -ArgumentList $argsLine -PassThru
    Start-Sleep -Seconds 4  # settle: thumbnails loaded, preview started
    $p.Refresh()
    $cpu0 = $p.TotalProcessorTime.TotalMilliseconds
    $t0 = Get-Date
    $gpu = @()
    for ($i = 0; $i -lt $Seconds; $i++) {
        $c = Get-Counter "\GPU Engine(pid_$($p.Id)*)\Utilization Percentage" -ErrorAction SilentlyContinue
        if ($c) { $gpu += ($c.CounterSamples | Measure-Object CookedValue -Sum).Sum }
        Start-Sleep -Milliseconds 1000
    }
    $p.Refresh()
    $cpu = ($p.TotalProcessorTime.TotalMilliseconds - $cpu0) / ((Get-Date) - $t0).TotalMilliseconds * 100
    $ws = $p.WorkingSet64 / 1MB
    Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
    '{0,-28} CPU {1,6:N1} % of one core   GPU {2,5:N1} %   memory {3,6:N0} MB' -f $what, $cpu,
        ($(if ($gpu.Count) { ($gpu | Measure-Object -Average).Average } else { 0 })), $ws
}
if ($Label) { "== $Label" }
Measure-Instance '--no-save --tab trons --view grid' 'idle, grid'
Measure-Instance "--no-save --tab trons --view grid --select $Tron --hover $Tron" "playing tron $Tron"
Measure-Instance '--no-save --tab trons --view grid --select 500 --hover 500' 'playing tron 500 (880x720)'
