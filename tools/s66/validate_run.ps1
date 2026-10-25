# Session 66 (prepared in Session 65): run one route with the Khronos validation layer (Vulkan SDK 1.4.363.0, installed
# 2026-10-25 by Jarrod) and keep the layer's messages in a file. In-process only (& from PowerShell).
#   & .\tools\s66\validate_run.ps1 -Script tools\input_scripts\short\s45_parity_shots.txt -Extra '--wwe2k14_replay_play=s21_lb --wwe2k14_preset_low=true ...' [-GpuAv] [-Sync]
# -GpuAv: GPU-assisted validation (out-of-range descriptor indexes, buffer-address reads: the bindless / B3 checks);
# slow, and the GPU numbers of such a run mean nothing. -Sync: synchronization validation (barriers, render passes).
# The layer is explicit (registered, never loaded unless asked), so ordinary runs are unaffected. Output:
# out\s66\validation\<run>_<script>.txt plus a count of distinct message ids.
# Exit 0 = no message id outside the baseline, 2 = new ids (listed as NEW), 1 = no layer output.
param([Parameter(Mandatory)] [string]$Script, [string]$Extra = '', [switch]$GpuAv, [switch]$Sync, [switch]$SetBaseline,
      [int]$WaitSeconds = 1800)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$dir = "$R\out\s66\validation"
New-Item -ItemType Directory -Force $dir | Out-Null
$log = "$dir\pending_$([IO.Path]::GetFileNameWithoutExtension($Script)).txt"
Remove-Item $log -ErrorAction SilentlyContinue
$env:VK_KHRONOS_VALIDATION_LOG_FILENAME = $log
$env:VK_KHRONOS_VALIDATION_DEBUG_ACTION = 'VK_DBG_LAYER_ACTION_LOG_MSG'
$env:VK_KHRONOS_VALIDATION_REPORT_FLAGS = 'error,warn'
$env:VK_KHRONOS_VALIDATION_GPUAV_ENABLE = $(if ($GpuAv) { '1' } else { '0' })
$env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = $(if ($Sync) { '1' } else { '0' })
$settings = "--wwe2k14_settings_file=$R\out\s66\settings_test.json"
try {
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @($Script) -Extra "--vulkan_validation_enabled=true $settings $Extra".Trim() -WaitSeconds $WaitSeconds 2>&1
} finally {
    'VK_KHRONOS_VALIDATION_LOG_FILENAME', 'VK_KHRONOS_VALIDATION_DEBUG_ACTION', 'VK_KHRONOS_VALIDATION_REPORT_FLAGS',
    'VK_KHRONOS_VALIDATION_GPUAV_ENABLE', 'VK_KHRONOS_VALIDATION_VALIDATE_SYNC' | ForEach-Object { Remove-Item "env:$_" -ErrorAction SilentlyContinue }
}
$line = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' }) | Select-Object -Last 1
$run = if ("$line" -match 'wwe2k14_(\d+)') { $Matches[1] } else { 'norun' }
"run: $line"
if (-not (Test-Path $log)) { "no layer output file (layer not loaded? check the run log for 'VK_LAYER_KHRONOS_validation'), run $run"; exit 1 }
$final = "$dir\$($run)_$([IO.Path]::GetFileNameWithoutExtension($Script)).txt"
Move-Item $log $final -Force
# Message ids (the layer prints each id at most 10 times by default). Ids already seen in the stored baseline
# (out\s66\validation\baseline_ids.txt, made from an unchanged build with -SetBaseline) are listed as "known".
$ids = Select-String -Path $final -Pattern '\[ (VUID-[A-Za-z0-9_-]+|UNASSIGNED-[A-Za-z0-9_-]+) \]' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Group-Object | Sort-Object Count -Descending
$baseFile = "$dir\baseline_ids.txt"
if ($SetBaseline) { $ids | ForEach-Object { $_.Name } | Set-Content $baseFile; "baseline ids stored: $baseFile" }
$known = if (Test-Path $baseFile) { @(Get-Content $baseFile) } else { @() }
$new = @($ids | Where-Object { $_.Name -notin $known })
"validation: $((Get-Content $final).Count) lines, $(@($ids).Count) distinct ids, NEW (not in baseline): $($new.Count) -> $final"
$ids | Select-Object -First 20 | ForEach-Object { "  {0,4}  {1}{2}" -f $_.Count, $_.Name, $(if ($_.Name -in $known) { '  (known)' } else { '  NEW' }) }
if ($new.Count) { exit 2 }
