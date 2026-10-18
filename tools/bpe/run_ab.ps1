# Session 11 step 6: timed native-BPE A/B, one exclusive run-lock hold per run (run_list -Exclusive with one script
# each time). Both arms carry the same probes (--wwe2k14_decode_probe --wwe2k14_barrier_probe); the "on" arm adds
# --wwe2k14_native_bpe=true, the "off" arm =false (native is the default since Session 11). Order per route: off, on, off, on, ... (-Pairs N). Prints one result line per run.
# Analyse: py tools\bpe\ab_report.py RUN ...
# Usage: pwsh -NoProfile -File tools\bpe\run_ab.ps1 -Route pan|taker [-Pairs 3]
param([Parameter(Mandatory)][ValidateSet('pan', 'taker')][string]$Route, [int]$Pairs = 3)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$probe = '--wwe2k14_decode_probe=true --wwe2k14_barrier_probe=true'
switch ($Route) {
    # Launched Lesnar vs Bryan (Session 10 Track C route), default 1440p preset.
    'pan' { $script = "$R\tools\input_scripts\short\bench_launch_lesnar_bryan_pan.txt"; $base = "--wwe2k14_launch=lesnar_bryan $probe" }
    # The decoder study's route (menus, Lesnar vs Undertaker, WrestleMania XXX) at the 4K preset, to 103.bik2 + 600.
    'taker' { $script = "$R\tools\input_scripts\short\bench_taker_wm_taker_wm_hitch.txt"; $base = "--wwe2k14_preset_4k60=true $probe" }
}
for ($i = 1; $i -le $Pairs; $i++) {
    foreach ($arm in 'off', 'on') {
        $extra = if ($arm -eq 'on') { "$base --wwe2k14_native_bpe=true" } else { "$base --wwe2k14_native_bpe=false" }
        $line = & "$R\tools\bench\run_list.ps1" -Scripts @($script) -Extra $extra -Exclusive -WaitSeconds 600 2>&1 |
            Where-Object { "$_" -match ' -> ' } | Select-Object -Last 1
        Write-Output "[ab] $Route pair $i $arm : $line"
    }
}
