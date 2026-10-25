# Session 66 (prepared in Session 65): one command per NATIVE 2 item - is it exact, is gameplay unchanged, is it faster.
# In-process only (call with & from PowerShell, so the flag strings stay whole):
#   & .\tools\s66\check_item.ps1 -Name aliases -Flag '--gpu_native_resolve_alias=true' [-Base '...'] [-Hot 'gpu_native_resolve_alias'] [-Skip det,ab]
# Steps (each wraps an existing tool, docs\native\S66_HARNESS.md):
#   parity  s45_parity_shots (6 same-frame shots of the s21_lb replay) base vs item -> tools\native\parity_pair.py
#   hash    s45_hash_seglog (every resolve's hash at Bryan +440 / match +600) base vs item -> resolve_hash_compare.py
#   det     replay_check recording base vs item with det hashes -> tools\determinism\det_compare.py (runs alone)
#   ab      only with -Hot (cvars that switch at run time): s56_ab_route.py route, ab_gpu.py GPU ms + command busy
# Base = NATIVE as it ships in a +dev build (--wwe2k14_preset_low=true, 720p) plus -Base; item = base plus -Flag.
# Base runs are cached per exe hash + base flags (out\s66\base_cache.json; -NoCache runs them again).
# Writes out\s66\items\<Name>\verdict.txt; exit 0 = exact + identical, 1 = pictures / hashes differ, 2 = gameplay
# differs, 3 = a run failed. -MaxOver8: largest share (%) of pixels more than 8 levels apart still called exact
# (replay frames are near-identical, not byte-identical).
param([Parameter(Mandatory)] [string]$Name, [Parameter(Mandatory)] [string]$Flag, [string]$Base = '',
      [string[]]$Hot = @(), [string[]]$Skip = @(), [double]$MaxOver8 = 0.05, [switch]$NoCache)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$runList = "$R\tools\bench\run_list.ps1"
$short = "$R\tools\input_scripts\short"
$exe = "$R\out\build\win-amd64-release\wwe2k14.exe"
$outDir = "$R\out\s66\items\$Name"
New-Item -ItemType Directory -Force $outDir | Out-Null
$settings = "--wwe2k14_settings_file=$R\out\s66\settings_test.json"
$common = "--wwe2k14_preset_low=true $settings $Base".Trim()
$replay = '--wwe2k14_replay_play=s21_lb'
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$verdict = [System.Collections.Generic.List[string]]::new()
$code = 0
function Say([string]$s) { $s; $verdict.Add($s) }
Say "check_item $Name  ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
Say "  item flags: $Flag"
Say "  base flags: $common"

$exeHash = (Get-FileHash $exe -Algorithm SHA256).Hash.Substring(0, 12)
$cacheFile = "$R\out\s66\base_cache.json"
$cache = @{}
if ((Test-Path $cacheFile) -and -not $NoCache) {
    (Get-Content $cacheFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $cache[$_.Name] = $_.Value }
}
function Save-Cache { $cache | ConvertTo-Json | Set-Content $cacheFile }

# Runs scripts (parallel lanes) with per-script flags; returns @{ script-name = run number }.
function Invoke-Runs([string[]]$scripts, [string[]]$extras) {
    $out = & $runList -Scripts $scripts -ScriptExtras $extras -Parallel $scripts.Count -WaitSeconds 900 2>&1
    $runs = @{}
    foreach ($l in $out) {
        # one run: 'X.txt -> wwe2k14_N'; -Parallel: 'X.txt > wwe2k14_N' (PowerShell names ignore case: never $r here)
        if ("$l" -match '(\S+\.txt) -?> wwe2k14_(\d+)') { $runs[$Matches[1]] = [int]$Matches[2] }
    }
    foreach ($s in $scripts) {
        $leaf = Split-Path $s -Leaf
        if (-not $runs.ContainsKey($leaf)) { $out | Select-Object -Last 15 | ForEach-Object { "  run_list: $_" } }
    }
    $runs
}

# parity + hash: one run_list call per arm (two different scripts, two lanes).
$pairScripts = @()
if ('parity' -notin $Skip) { $pairScripts += "$short\s45_parity_shots.txt" }
if ('hash' -notin $Skip) { $pairScripts += "$short\s45_hash_seglog.txt" }
if ($pairScripts.Count) {
    $key = "$exeHash|$common|pair"
    $baseRuns = $cache[$key]
    if (-not $baseRuns -or ($pairScripts | Where-Object { -not $baseRuns.(Split-Path $_ -Leaf) })) {
        $pairBase = Invoke-Runs $pairScripts @($pairScripts | ForEach-Object { "$replay $common" })
        $baseRuns = [pscustomobject]$pairBase
        $cache[$key] = $baseRuns
        Save-Cache
    }
    $itemRuns = Invoke-Runs $pairScripts @($pairScripts | ForEach-Object { "$replay $common $Flag" })
    if ('parity' -notin $Skip) {
        $a, $b = $baseRuns.'s45_parity_shots.txt', $itemRuns['s45_parity_shots.txt']
        if (-not $a -or -not $b) { Say "PARITY: run failed (base $a, item $b)"; $code = 3 }
        else {
            $p = py "$R\tools\native\parity_pair.py" $a $b --prefix p_ 2>&1
            $p | Set-Content "$outDir\parity.txt"
            $worst = 999.0
            $w = $p | Select-String 'worst share more than 8 levels apart: ([\d.]+)%'
            if ($w) { $worst = [double]$w.Matches[0].Groups[1].Value }
            $same = @($p | Where-Object { "$_" -match '^p_\S+\s+100\.000%' }).Count
            $rows = @($p | Where-Object { "$_" -match '^p_' }).Count
            $ok = $worst -le $MaxOver8
            Say ("PARITY: {0}  runs {1} vs {2}: {3} of {4} shots pixel-identical, worst {5:N3}% of pixels > 8 levels (limit {6})" -f
                 ($(if ($ok) { 'EXACT' } else { 'DIFFERS' })), $a, $b, $same, $rows, $worst, $MaxOver8)
            if (-not $ok -and $code -eq 0) { $code = 1 }
        }
    }
    if ('hash' -notin $Skip) {
        $a, $b = $baseRuns.'s45_hash_seglog.txt', $itemRuns['s45_hash_seglog.txt']
        if (-not $a -or -not $b) { Say "HASHES: run failed (base $a, item $b)"; $code = 3 }
        else {
            $h = py "$R\tools\native\resolve_hash_compare.py" $a $b 2>&1
            $h | Set-Content "$outDir\hashes.txt"
            $n = 0; $same = 0
            foreach ($l in $h) { if ("$l" -match '^0x\S+\s+(\d+)\s+(\d+)\s') { $n += [int]$Matches[1]; $same += [int]$Matches[2] } }
            $counts = ($h | Select-Object -First 1)
            $ok = $n -gt 0 -and $same -eq $n
            Say ("HASHES: {0}  runs {1} vs {2}: {3} of {4} matched resolves have the same hash ({5})" -f
                 ($(if ($ok) { 'EXACT' } else { 'DIFFERS' })), $a, $b, $same, $n, $counts)
            if (-not $ok -and $code -eq 0) { $code = 1 }
        }
    }
}

# det: the replay_check recording, base vs item, one run at a time (fixed-seed routes go alone).
if ('det' -notin $Skip) {
    $det = '--wwe2k14_replay_play=replay_check --wwe2k14_det_hash=default,objects --wwe2k14_det_hash_raw_every=30 --resolution_scale=1'
    $script = "$R\tools\replay\replay_check.txt"
    $key = "$exeHash|$common|det"
    $a = $cache[$key]
    if (-not $a) {
        $a = (Invoke-Runs @($script) @("$det $common"))['replay_check.txt']
        if ($a) { $cache[$key] = $a; Save-Cache }
    }
    $b = (Invoke-Runs @($script) @("$det $common $Flag"))['replay_check.txt']
    if (-not $a -or -not $b) { Say "GAMEPLAY: run failed (base $a, item $b)"; $code = 3 }
    else {
        py "$R\tools\determinism\det_compare.py" $a $b | Out-Null
        $cmp = "$R\out\harness\determinism\compare_$($a)_$($b).txt"
        $lines = @(Get-Content $cmp)
        $div = @($lines | Where-Object { $_ -match ' diverges, changes ' })
        $idn = @($lines | Where-Object { $_ -match ' identical, changes ' })
        $ok = $div.Count -eq 0 -and $idn.Count -gt 0
        Say ("GAMEPLAY: {0}  runs {1} vs {2}: {3} regions identical, {4} diverge ({5})" -f
             ($(if ($ok) { 'IDENTICAL' } else { 'DIFFERS' })), $a, $b, $idn.Count, $div.Count, $cmp)
        $div | Select-Object -First 6 | ForEach-Object { Say "  $($_.Trim())" }
        if (-not $ok -and $code -in 0, 1) { $code = 2 }
    }
}

# ab: in-run A/B on the s21_lb replay (Brock, Bryan, match), fast-forwarded; only for cvars that switch live.
if ('ab' -notin $Skip) {
    if (-not $Hot.Count) { Say 'A/B: skipped (no -Hot cvars: restart-only item; compare two perf_scene runs instead)' }
    else {
        $route = py "$R\tools\perf\s56_ab_route.py" "s66_$Name" "Session 66 item $Name" @Hot
        $ab = (Invoke-Runs @("$route") @("$replay $common --vulkan_gpu_timestamps=true --wwe2k14_file_trace=true"))[(Split-Path "$route" -Leaf)]
        if (-not $ab) { Say 'A/B: run failed'; $code = 3 }
        else {
            Say "A/B: run $ab (mode 1 = on; change = on vs the neighbouring off windows, median and middle half)"
            Say '  GPU ms:'
            py "$R\tools\native\ab_gpu.py" $ab | Select-Object -Skip 1 | ForEach-Object { Say "    $_" }
            Say '  command-thread busy ms:'
            py "$R\tools\native\ab_gpu.py" $ab --metric busy | Select-Object -Skip 1 | ForEach-Object { Say "    $_" }
        }
    }
}

Say "VERDICT: exit $code (0 exact + identical, 1 pictures / hashes differ, 2 gameplay differs, 3 a run failed)"
$verdict | Set-Content "$outDir\verdict.txt"
"verdict: $outDir\verdict.txt"
exit $code
