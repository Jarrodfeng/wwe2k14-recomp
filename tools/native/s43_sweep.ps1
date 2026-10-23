# Session 43 (stretch S1): robustness sweep of the native paths over contexts other than the s21_lb replay. Each
# context runs twice through the scenario launcher: the LOW preset as it is (gpu_native 2) and LOW with every new
# native setting on (post, post depth, 16-bit, quantize, after-scene). Prints the run numbers; read the results with
#   py tools\native\sweep_report.py RUN...
# Untimed, fast-forwarded, up to 3 at once. In-process only (call with & from PowerShell).
#   & .\tools\native\s43_sweep.ps1 [-Only cage,hiac] [-AllExtra '...'] [-Variants all]
param([string[]]$Only = @(), [string]$AllExtra = '', [string[]]$Variants = @('low', 'all'))  # Session 44: -AllExtra adds settings to the "all" variant; Session 45: -Variants all (skip the LOW reference runs)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$boot = "$R\tools\input_scripts\short\launch_boot_check.txt"
$rumble = "$R\tools\input_scripts\short\launch_rumble_check.txt"
$quick = "$R\tools\input_scripts\short\quick_type_check.txt"
$all = '--gpu_native_post=true --gpu_native_post_depth=true --gpu_native_post_16bit=true --gpu_native_post_quantize=true --gpu_native_after_scene=true --gpu_native_scene_extras=true --vulkan_texture_load_direct_unscaled=true'
$contexts = [ordered]@{
    wm_x7  = @($boot, '--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=44 --wwe2k14_launch_participants=101,100')
    hiac   = @($boot, '--wwe2k14_launch=hiac_cena_brodus')
    chamber = @($boot, '--wwe2k14_launch=chamber_6man')
    cage   = @($boot, '--wwe2k14_launch=s30_cage')
    rumble = @($rumble, '--wwe2k14_launch=rumble_40 --wwe2k14_step_hz=30')
    # Session 43 additions: Quick Matches (match only), tag team, ladder, TLC
    tag    = @($quick, '--wwe2k14_launch=@quick --wwe2k14_quick_type=3 --wwe2k14_launch_participants=343,324,139,103 --wwe2k14_launch_skip_entrances=true')
    ladder = @($quick, '--wwe2k14_launch=@quick --wwe2k14_quick_type=55 --wwe2k14_launch_participants=343,324 --wwe2k14_launch_skip_entrances=true')
    tlc    = @($quick, '--wwe2k14_launch=@quick --wwe2k14_quick_type=72 --wwe2k14_launch_participants=139,103 --wwe2k14_launch_skip_entrances=true')
    # Session 44 additions: 30 Years of WrestleMania gameplay (match 43, The Undertaker vs. CM Punk; match 15, the
    # Iron Man score board), a Rumble well past its start, Create a Story (its route runs at normal speed by header)
    wm30_43 = @("$R\tools\input_scripts\short\s44_wm30_play.txt", '--wwe2k14_wm30=43 --wwe2k14_wm30_auto=false')
    wm30_15 = @("$R\tools\input_scripts\short\s44_wm30_play.txt", '--wwe2k14_wm30=15 --wwe2k14_wm30_auto=false')
    rumble_mid = @("$R\tools\input_scripts\short\s44_rumble_mid.txt", '--wwe2k14_launch=rumble_40 --wwe2k14_step_hz=30')
    story  = @("$R\tools\input_scripts\short\s44_story_check.txt", '')
}
if ($AllExtra) { $all = "$all $AllExtra" }
$scripts = @(); $extras = @(); $names = @()
foreach ($k in $contexts.Keys) {
    if ($Only.Count -and $Only -notcontains $k) { continue }
    foreach ($variant in $Variants) {
        $x = "--wwe2k14_preset_low=true $($contexts[$k][1])"
        if ($variant -eq 'all') { $x = "$x $all" }
        $scripts += $contexts[$k][0]; $extras += $x; $names += "$k/$variant"
    }
}
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras -Parallel 3 -WaitSeconds 1500 2>&1
$runs = @($out | Select-String ' -> wwe2k14_(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
for ($i = 0; $i -lt $names.Count; $i++) { "$($names[$i]) -> $($runs[$i])" }
$out | Select-String 'checks:|CRASH|crash' | ForEach-Object { $_.Line }
