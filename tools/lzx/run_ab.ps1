# Session 12 step 6: timed native-LZX load-time A/B on tools\input_scripts\short\load_times.txt
# (--wwe2k14_launch=lesnar_bryan, default 1440p preset), one exclusive run-lock hold per run (run_list -Exclusive with
# one script each time). Both arms carry the probe (--wwe2k14_lzx_probe=true); the "on" arm adds
# --wwe2k14_native_lzx=true. Order: off, on, off, on, ... (-Pairs N). Prints one result line per run.
# Analyse: py tools\lzx\load_report.py --off A,C,E --on B,D,F
# Usage: pwsh -NoProfile -File tools\lzx\run_ab.ps1 [-Pairs 3] [-Scenario lesnar_bryan]
param([int]$Pairs = 3, [string]$Scenario = 'lesnar_bryan')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$script = "$R\tools\input_scripts\short\load_times.txt"
$base = "--wwe2k14_launch=$Scenario --wwe2k14_lzx_probe=true"
for ($i = 1; $i -le $Pairs; $i++) {
    foreach ($arm in 'off', 'on') {
        $extra = if ($arm -eq 'on') { "$base --wwe2k14_native_lzx=true" } else { "$base --wwe2k14_native_lzx=false" }
        $line = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra $extra -Exclusive -WaitSeconds 600 2>&1 |
            Where-Object { "$_" -match ' -> ' } | Select-Object -Last 1
        Write-Output "[ab] pair $i $arm : $line"
    }
}
