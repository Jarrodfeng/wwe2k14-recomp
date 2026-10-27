# Session 70 (v0.2.8's slow entrances): one real-time entrance run of a staged release folder, as a player has it.
#   & .\tools\perf\s70_ab_run.ps1 -Stage v0.2.8 -Bin cold [-Data userdata_s70_v027] [-Extra '--x=1'] [-SaveData NAME]
# -Stage: out\release\stage\wwe2k14-recomp-<Stage> (or a folder path). -Bin: the folder the exe runs from, under
# out\s70\bin\ - 'cold' = a new copy in a new folder (NVIDIA keys its pipeline cache by the exe's path, so a new folder
# starts with a cold driver cache, as a player unzipping a new release), any other name = that copy, made once and
# reused (warm driver cache from its second run on). -Data: the user data mirrored into the run (out\bench\NAME; the
# frozen data has an empty NATIVE store). -SaveData: the run's user data afterwards copied to out\bench\NAME (the next
# session of the same player). Runs exclusive at normal speed (run_list -Exclusive), NATIVE 720p test settings
# (out\s70\settings_native720.json: renderer 2, resolution 0, frame rate 1 - the GTX 1060 player's), route
# short\s70_entrance_rt.txt. Prints the run line and the report (tools\perf\s70_entrance_report.py).
param([string]$Stage = 'v0.2.8', [string]$Bin = 'cold', [string]$Data = 'userdata_frozen', [string]$Extra = '',
      [string]$SaveData = '', [string]$Route = 'short\s70_entrance_rt.txt', [switch]$FastForward,
      [switch]$Uncapped, [string]$Settings = '', [string]$Affinity = '', [string]$NvCache = '')
# -NvCache NAME: NVIDIA's driver shader cache for this run in out\s70\nvcache\NAME (__GL_SHADER_DISK_CACHE_PATH; a new
# NAME = a cold driver cache; the real one in %LOCALAPPDATA%\NVIDIA is not touched). Its cache is keyed by pipeline
# content, not by the exe or its folder (runs C27 / v027_coldtest: new folders and a changed exe stayed warm).
# -Affinity 0xFF: the game pinned to those logical processors right after it starts (a watcher job), e.g. 0xFF = 4
# cores with SMT on the 5950X - the GTX 1060 player's i7-4770K / the Iris Xe tester's i5-1135G7 (8 threads). Thread
# pools sized from the CPU count at start stay as here; pass --gpu_native_pipeline_workers=2 for v0.2.8's pool.
# -Uncapped (Jarrod: the 60 fps cap hides steady extra work on the 4090): exclusive, but fast-forwarded (--vsync=false)
# with GPU timestamps - frames per second and GPU ms per entrance, for warm runs (nothing left to compile).
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$src = if (Test-Path $Stage) { (Resolve-Path $Stage).Path } else { Join-Path $R "out\release\stage\wwe2k14-recomp-$Stage" }
if (-not (Test-Path (Join-Path $src 'wwe2k14.exe'))) { throw "no wwe2k14.exe in $src" }
$tag = (Split-Path $src -Leaf) -replace '^wwe2k14-recomp-', ''
$binRoot = Join-Path $R 'out\s70\bin'
New-Item -ItemType Directory -Force $binRoot | Out-Null
$binName = if ($Bin -eq 'cold') { "${tag}_cold_$(Get-Date -Format 'MMdd_HHmmss')" } else { $Bin }
$binDir = Join-Path $binRoot $binName
if ($Bin -eq 'cold' -and (Test-Path $binDir)) { throw "$binDir exists (not cold)" }
if (-not (Test-Path (Join-Path $binDir 'wwe2k14.exe'))) {
  robocopy $src $binDir /E /NFL /NDL /NJH /NJS | Out-Null
  # NVIDIA's cache follows the exe file, not its folder (run C27: a timestamp-keeping copy in a new folder was warm):
  # 16 random bytes past the end of the image make it a new exe to the driver (Windows ignores an overlay), so a new
  # folder starts cold, as a player's new version does.
  $exeCopy = Join-Path $binDir 'wwe2k14.exe'
  $fs = [IO.File]::Open($exeCopy, 'Append')
  $fs.Write([Guid]::NewGuid().ToByteArray(), 0, 16)
  $fs.Close()
  Write-Host "[s70] copied $src -> $binDir (exe made unique)"
}
if (-not $Settings) {
  $Settings = Join-Path $R 'out\s70\settings_native720.json'
  if (-not (Test-Path $Settings)) { Set-Content -Path $Settings -Value '{"renderer": 2, "resolution": 0, "frame_rate": 1, "settings_version": 2}' -NoNewline }
}
$flags = "--game_data_root=$R\game_data\extracted --wwe2k14_settings_file=$Settings --wwe2k14_launch=@quick " +
         '--wwe2k14_quick_type=0 --wwe2k14_launch_participants=343,324 --gpu_session_stats_frames=60'
if ($Uncapped) { $flags += ' --vsync=false --vulkan_gpu_timestamps=true' }
if ($Extra) { $flags += " $Extra" }
$script = Join-Path $R "tools\input_scripts\$Route"
Write-Host "[s70] $binName  data=$Data  extra='$Extra'"
$args2 = @{ Scripts = @($script); Exe = (Join-Path $binDir 'wwe2k14.exe'); Data = $Data; Extra = $flags; WaitSeconds = 900 }
# (-Affinity: the SDK resets the process mask to every CPU at start (threading_win.cpp EnableAffinityConfiguration),
# so an inherited mask is undone and a watcher job came 30 s late (its own PowerShell starting). A watcher thread in
# this process (a runspace) sets it 3 s after the game starts and again whenever it finds it changed.)
$watchPs = $null
$watchAr = $null
if ($Affinity) {
  $mask = [Convert]::ToInt64($Affinity.Replace('0x', ''), 16)
  $watchPs = [powershell]::Create()
  $null = $watchPs.AddScript({
    param($exe, $mask, $since)
    $log = @()
    for ($i = 0; $i -lt 6000; $i++) {  # 20 min at 200 ms
      $p = Get-Process wwe2k14 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe -and $_.StartTime -gt $since }
      foreach ($q in $p) {
        if (([datetime]::Now - $q.StartTime).TotalSeconds -ge 3 -and [int64]$q.ProcessorAffinity -ne $mask) {
          $q.ProcessorAffinity = [IntPtr]$mask
          $log += ('{0:HH:mm:ss.f} pid {1}: affinity 0x{2:X} set ({3:N1} s after start)' -f [datetime]::Now, $q.Id, $mask, ([datetime]::Now - $q.StartTime).TotalSeconds)
        }
      }
      if ($log.Count -and -not $p) { break }  # (the game has quit)
      Start-Sleep -Milliseconds 200
    }
    $log -join '; '
  }).AddArgument((Join-Path $binDir 'wwe2k14.exe')).AddArgument($mask).AddArgument([datetime]::Now)
  $watchAr = $watchPs.BeginInvoke()
}
$oldNv = $env:__GL_SHADER_DISK_CACHE_PATH
if ($NvCache) {
  $nvDir = Join-Path $R "out\s70\nvcache\$NvCache"
  New-Item -ItemType Directory -Force $nvDir | Out-Null
  $env:__GL_SHADER_DISK_CACHE_PATH = $nvDir
  $env:__GL_SHADER_DISK_CACHE_SKIP_CLEANUP = '1'
  Write-Host "[s70] NVIDIA cache: $nvDir ($((Get-ChildItem $nvDir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count) files)"
}
try {
  if ($FastForward) { $out = & "$R\tools\bench\run_list.ps1" @args2 } else { $out = & "$R\tools\bench\run_list.ps1" @args2 -Exclusive }
} finally {
  if ($watchPs) {
    if ($watchAr.AsyncWaitHandle.WaitOne(5000)) { Write-Host "[s70] affinity watcher: $($watchPs.EndInvoke($watchAr))" }
    else { $watchPs.Stop(); Write-Host '[s70] affinity watcher: stopped (it never set the mask)' }
    $watchPs.Dispose()
  }
  $env:__GL_SHADER_DISK_CACHE_PATH = $oldNv
  Remove-Item Env:__GL_SHADER_DISK_CACHE_SKIP_CLEANUP -ErrorAction SilentlyContinue
}
if ($NvCache) { Write-Host "[s70] NVIDIA cache after: $((Get-ChildItem $nvDir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count) files" }
$out | ForEach-Object { "$_" }
$line = $out | Where-Object { "$_" -match 'wwe2k14_(\d+)' } | Select-Object -First 1
if (-not $line) { throw 'no run line' }
$run = [regex]::Match("$line", 'wwe2k14_(\d+)').Groups[1].Value
$log = Join-Path $binDir "logs\wwe2k14_$run.log"
if ($SaveData) {
  $work = if ($FastForward) { $null } else { Join-Path $R 'out\bench\userdata_work' }
  if ($FastForward) {
    $slot = (Select-String -Path $log -Pattern 'userdata_work_s(\d)' | Select-Object -First 1).Matches[0].Groups[1].Value
    $work = Join-Path $R "out\bench\userdata_work_s$slot"
  }
  robocopy $work (Join-Path $R "out\bench\$SaveData") /MIR /NFL /NDL /NJH /NJS | Out-Null
  Write-Host "[s70] user data saved: $work -> out\bench\$SaveData"
}
py "$R\tools\perf\s70_entrance_report.py" $log
