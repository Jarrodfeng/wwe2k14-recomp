# Session 70 (Jarrod: "This class of regression should fail a check before it ships"): the cold-cache NATIVE entrance
# check. v0.2.6 - v0.2.8 shipped with Brock's entrance freezing 4.6 s on a slow-compiling PC because every check ran
# with warm caches on a fast CPU. This one runs the release exe as an upgrading player on a slow PC meets it:
#   - an empty NVIDIA shader cache (__GL_SHADER_DISK_CACHE_PATH = a new folder: every pipeline compiles cold);
#   - every pipeline creation +40 ms on top (the Intel tester averaged 76 ms a pipeline);
#   - an upgrader's store (out\bench\userdata_entrance_check: the frozen save + a v0.2.7-format NATIVE store, made in
#     Session 70 run 001 of the v0.2.7 release; local only, never shipped);
#   - NATIVE at 720p (a test settings file), the shipped shader list, no precompile (the in-game path: a player may
#     skip the precompile);
#   - route short\s70_entrance_rt.txt: Brock vs Bryan, both entrances and 600 match frames in real time (~3 min,
#     exclusive).
# PASS: both entrances and the match without a frame over -MaxFrameMs (500) and with at most -MaxLostS (2.0 s) lost
# (the sum of each frame's time over 16.7 ms). Session 70 numbers on these conditions: v0.2.5 55 ms / 0.68 s, v0.2.8
# 200 ms / 0.79 s; v0.2.6 / v0.2.7 4,701 / 4,584 ms and 7.8 / 7.6 s lost (Brock's entrance).
#   pwsh -NoProfile -File tools\release\entrance_check.ps1 [-Exe out\release\build\wwe2k14.exe] [-Extra '...']
param([string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\release\build\wwe2k14.exe', [string]$Extra = '',
      [double]$MaxFrameMs = 500, [double]$MaxLostS = 2.0)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$data = 'userdata_entrance_check'
if (-not (Test-Path "$R\out\bench\$data\cache\shaders\shareable\545408B2.native.fsi.vk.xnps")) {
  if (Test-Path "$R\out\bench\userdata_s70_v027") {
    robocopy "$R\out\bench\userdata_s70_v027" "$R\out\bench\$data" /MIR /NFL /NDL /NJH /NJS | Out-Null
  } else {
    Write-Host "entrance check: FAIL: no upgrader store in out\bench\$data (Session 70: copy of userdata_s70_v027)"
    exit 1
  }
}
$settings = "$R\out\release\entrance_check_settings.json"
Set-Content -Path $settings -Value '{"renderer": 2, "resolution": 0, "frame_rate": 1, "settings_version": 2}' -NoNewline
$nv = "$R\out\release\entrance_check_nvcache"
if (Test-Path $nv) { Remove-Item -Recurse -Force $nv }
New-Item -ItemType Directory -Force $nv | Out-Null
$flags = "--game_data_root=$R\game_data\extracted --wwe2k14_settings_file=$settings --wwe2k14_use_launcher_settings=true " +
         "--wwe2k14_shader_list=$R\db\shaders\shader_list.txt --wwe2k14_launch=@quick --wwe2k14_quick_type=0 " +
         '--wwe2k14_launch_participants=343,324 --gpu_session_stats_frames=60 ' +
         '--gpu_native_truth_debug_pipeline_delay_ms=40 --vulkan_pipeline_debug_delay_ms=40'
if ($Extra) { $flags += " $Extra" }
$old = $env:__GL_SHADER_DISK_CACHE_PATH
$env:__GL_SHADER_DISK_CACHE_PATH = $nv
$env:__GL_SHADER_DISK_CACHE_SKIP_CLEANUP = '1'
try {
  $out = & "$R\tools\bench\run_list.ps1" -Scripts @("$R\tools\input_scripts\short\s70_entrance_rt.txt") -Exe $Exe -Data $data -Extra $flags -Exclusive -WaitSeconds 900
} finally {
  $env:__GL_SHADER_DISK_CACHE_PATH = $old
  Remove-Item Env:__GL_SHADER_DISK_CACHE_SKIP_CLEANUP -ErrorAction SilentlyContinue
}
$line = @($out | Where-Object { "$_" -match '-> wwe2k14_\d+' }) | Select-Object -Last 1
if ("$line" -notmatch 'wwe2k14_(\d+)') { Write-Host "entrance check: FAIL: no run line:`n$($out -join "`n")"; exit 1 }
$log = Join-Path (Split-Path -Parent $Exe) "logs\wwe2k14_$($Matches[1]).log"
if ((Get-ChildItem $nv -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0) {
  Write-Host 'entrance check: FAIL: the driver wrote no cache to the test folder (the cold cache did not take)'
  exit 1
}
$runNo = $Matches[1]
py "$R\tools\perf\s70_entrance_report.py" $log --check --max-frame-ms $MaxFrameMs --max-lost-s $MaxLostS
$code = $LASTEXITCODE
# Session 72: a PASS at the default limits is remembered for release.ps1 (tools\release\entrance_check_reuse.py), which
# leaves the check out while nothing that changes the game has changed since. A dev build's PASS counts too.
if ($code -eq 0 -and $MaxFrameMs -le 500 -and $MaxLostS -le 2.0 -and -not $Extra) {
  py -I "$R\tools\release\entrance_check_reuse.py" record --exe $Exe --run $runNo
}
exit $code
