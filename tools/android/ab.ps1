# Unattended phone A/B (Session 64): builds alternate A B A B ... (cancels the phone's warm-up / heat drift), each run
# installs its APK, runs the route with run.ps1 and samples the load with sample.ps1. Results: py tools\android\demo_fps.py
# <runs> --marks and samples_report.py per run. Keep runs short (Jarrod: 2 minutes unattended).
#   pwsh -File tools\android\ab.ps1 -First 031 -Apks out\android\apk\b32_base.apk,out\android\apk\b31_waitfix_faultfix.apk -Rounds 2 -Bench match
param([Parameter(Mandatory)][int]$First, [Parameter(Mandatory)][string[]]$Apks, [int]$Rounds = 2,
      [ValidateSet('match', 'entrance')][string]$Bench = 'match', [int]$Seconds = 120, [string]$ExtraArgs = '')
. "$PSScriptRoot\env.ps1"
$Apks = @($Apks | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # pwsh -File passes a list as one string
$benches = @{
    match = @{ Route = "$ProjectRoot\tools\input_scripts\android\bench_cena_taker_match.txt"; Scenario = 'lesnar_bryan';
               Args = '--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=139,103 --wwe2k14_launch_skip_entrances=true' }
    entrance = @{ Route = "$ProjectRoot\tools\input_scripts\android\bench_taker_entrance.txt"; Scenario = 'lesnar_bryan';
                  Args = '--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=103,139' }
}
$b = $benches[$Bench]
$run = $First
$log = @()
for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($apk in $Apks) {
        $n = '{0:D3}' -f $run
        "== run $n : $([IO.Path]::GetFileName($apk))"
        & $Adb install -r (Resolve-Path $apk).Path | Select-Object -Last 1
        $sampler = Start-Process pwsh -ArgumentList '-NoProfile', '-File', "$PSScriptRoot\sample.ps1", '-Run', $n -PassThru -WindowStyle Hidden
        pwsh -NoProfile -File "$PSScriptRoot\run.ps1" -Run $n -Seconds $Seconds -Shots 1 -Route $b.Route -Scenario $b.Scenario `
            -GameArgs "$($b.Args) $ExtraArgs".Trim() | Select-Object -Last 3
        $sampler.WaitForExit(30000) | Out-Null
        if (-not $sampler.HasExited) { Stop-Process -Id $sampler.Id -ErrorAction SilentlyContinue }
        $log += "$n $([IO.Path]::GetFileName($apk))"
        $run++
    }
}
"== A/B runs:"; $log
$log | Set-Content "$($AndroidDirs.Runs)\ab_$('{0:D3}' -f $First).txt"
