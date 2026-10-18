# Session 19 timed batch 2 (exclusive; PC idle):
#   item 2, the CCD test: short\profile_lesnar_bryan.txt fast-forwarded, arms alternating A (unrestricted) / B (the
#   whole process on CCD0 = logical processors 0-15, mask 0xFFFF from tools\perf\cpu_topology.ps1), 3 runs each,
#   then 1 real-time run each;
#   item 3, the video-mode cache: short\ab_vmcache.txt (in-run A/B of kernel_video_mode_cache), fast-forwarded;
#   GPU lead: the profile route fast-forwarded at 720p (--resolution_scale=1).
# The CCD arm sets the game's process affinity as soon as the process appears (a watcher started before the run).
# Background (GPU %, CPU %) before each run -> out\harness\step19\batch2_background.txt; run list -> batch2_runs.txt.
#   pwsh -NoProfile -File tools\perf\s19_batch2.ps1 [-Only ccd,vmcache]
param([string[]]$Only = @('ccd', 'vmcache', 'res720'))
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
$out = "$repo\out\harness\step19"
New-Item -ItemType Directory -Force $out | Out-Null
$route = "$repo\tools\input_scripts\short\profile_lesnar_bryan.txt"
$ab = "$repo\tools\input_scripts\short\ab_vmcache.txt"
function Background([string]$tag) {
    $g = (1..4 | ForEach-Object { (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits).Trim(); Start-Sleep -Milliseconds 400 }) -join ' '
    $c = ((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples |
          ForEach-Object { [math]::Round($_.CookedValue, 1) }) -join ' '
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $tag background: GPU % [$g], CPU % [$c]" | Tee-Object -FilePath "$out\batch2_background.txt" -Append
}
function RunOne([string]$tag, [string]$script, [string]$extra, [string]$mask) {
    Background $tag
    $watch = $null
    if ($mask) {
        $cmd = "`$d=(Get-Date).AddSeconds(120); while((Get-Date) -lt `$d){ `$p=Get-Process wwe2k14 -ErrorAction SilentlyContinue | Select-Object -First 1; if(`$p){ `$p.ProcessorAffinity=[IntPtr]$mask; 'affinity set ' + `$p.Id + ' ' + `$p.ProcessorAffinity | Out-File -Append '$out\batch2_affinity.txt'; break }; Start-Sleep -Milliseconds 20 }"
        $watch = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', $cmd -PassThru -WindowStyle Hidden
    }
    $line = & "$repo\tools\bench\run_list.ps1" -Scripts @($script) -Exclusive -Extra $extra -WaitSeconds 900 2>&1 | Select-String ' -> '
    "$tag : $line" | Tee-Object -FilePath "$out\batch2_runs.txt" -Append
    if ($watch) { $watch | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue }
}
$ff = '--wwe2k14_launch=lesnar_bryan --vsync=false'
$rt = '--wwe2k14_launch=lesnar_bryan'
if ($Only -contains 'ccd') {
    foreach ($i in 1..3) {
        RunOne "ccd A$i (all cores) FF" $route $ff ''
        RunOne "ccd B$i (CCD0) FF" $route $ff '0xFFFF'
    }
    RunOne 'ccd A (all cores) RT' $route $rt ''
    RunOne 'ccd B (CCD0) RT' $route $rt '0xFFFF'
}
if ($Only -contains 'vmcache') {
    RunOne 'vmcache in-run A/B FF' $ab $ff ''
}
if ($Only -contains 'res720') {
    # GPU lead (item 1): the same route at 720p (resolution_scale 1), to tell per-pixel from per-draw GPU cost.
    RunOne 'res720 FF' $route "$ff --resolution_scale=1" ''
}
Background 'batch2 end'
