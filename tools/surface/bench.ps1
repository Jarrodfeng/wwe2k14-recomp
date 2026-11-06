# Session 79 (HD 520 arc, item 3): the first Surface session's bench in one command, run ON the Surface (over SSH or at
# its desktop), from a folder holding a build of the game (a release / preview zip unpacked: wwe2k14.exe + DLLs). It
# never touches the PC's run lock. Plugged in, best-performance power mode (Jarrod). Uses only Jarrod's own game files
# (-GameData) and a copy of the PC's frozen user data (-UserData: the save the replays were recorded on).
#   pwsh -File bench.ps1 -Exe C:\wwe\wwe2k14.exe -GameData D:\wwe2k14 -UserData C:\wwe\userdata_frozen
#   (Session 80: Windows PowerShell 5.1 works too - the Surface has no pwsh: powershell -ExecutionPolicy Bypass -File ...)
#        -Routes C:\wwe\routes -Replays C:\wwe\replays -Out C:\wwe\bench_out
#        [-Only passes,passes_ram,multi,arc,frames,mul,cold,levers,probes,pinned]
# Steps (each one game run; logs land in the game's logs folder, copied to -Out\<step>):
#   passes  per-pass GPU log at 720p on the native frame (s72_gap_log.txt + the s21_lb replay, fast-forwarded):
#           the Intel per-pass split (read on the PC: tools\native\pass_roles.py / pass_log.py)
#   frames  frame times at NORMAL speed, 60 fps, true 720p, native frame on (profile_lesnar_bryan route, launched):
#           against 16.7 ms per scene (tools\bench\bench_report.py)
#   frames_bryan / frames_brock / frames_match (Session 89, ask for them with -Only): one scene each, normal speed,
#           fixed seed 1, quits at the scene's end (tools\input_scripts\scenes\)
#   mul     the Intel multiply rule: the same frames run with --wwe2k14_intel_exact_mul=0 (exact rule off): its cost
#   cold    first launch with our shader cache emptied (a fresh -UserData copy): shader preparation time. The driver's
#           own cache (%LOCALAPPDATA%Low\Intel\ShaderCache) is moved aside ONLY with -ColdDriver (Jarrod's call), and
#           moved back afterwards - nothing is deleted.
# Then: Out\surface_bench_<stamp>.zip (logs only: no pictures, no game data) for the PC.
param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$GameData,
      [Parameter(Mandatory)][string]$UserData, [Parameter(Mandatory)][string]$Routes,
      [Parameter(Mandatory)][string]$Replays, [Parameter(Mandatory)][string]$Out,
      # (Session 80: the --wwe2k14_launch=lesnar_bryan scenario's folder; the game's default is ..\..\scenarios from the
      # exe, which only a build inside the repo has - out\scenarios.)
      [string]$Scenarios = '',
      # (Session 80: flags added to every step's game, without their leading -- (pwsh -File takes a value starting with
      # -- for a parameter name), e.g. -GameFlags wwe2k14_shader_list=X for a dev build folder, which has no
      # shader_list.txt next to the exe.)
      [string[]]$GameFlags = @(),
      # (Session 80, Jarrod: skip most of each entrance for A/Bs) the levers' and probes' route; profile_lesnar_bryan.txt
      # plays both entrances in full.
      [string]$AbRoute = 's80_probe_short.txt',
      # (Session 80) the multi step's in-run route (tools\native\passab_multi.py writes them), e.g. s80_owncrowd_multi.txt.
      [string]$MultiRoute = 's80_scene_multi.txt',
      [string[]]$Only = @('passes', 'frames', 'mul', 'cold'), [switch]$ColdDriver, [int]$TimeoutSeconds = 1500,
      [string[]]$Levers = @(),
      # (Session 80, 2026-11-02 10:10: a game started by the scheduled task is not let to the foreground by Windows, so
      # the taskbar stays over its borderless fullscreen window and the desktop compositor composes the 2736x1824
      # screen every refresh - ~3.3 ms of GPU time inside ~2 frames in 3 (runs 033-036, dwm.exe 7-13% of the 3D
      # engine). -Front puts the game's window on top and in the foreground once it shows, as a player's launch does.)
      [switch]$Front)
# (-Levers a,b: only those levers / probes and their base run, e.g. -Only levers -Levers no_alias_direct,aniso_4x.)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })  # (pwsh -File: one string)
$Levers = @($Levers | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
function Select-Levers($table) {
  if (-not $Levers.Count) { return @($table.Keys) }
  $unknown = @($Levers | Where-Object { -not $table.Contains($_) })
  if ($unknown.Count) { Write-Host "[surface] not a lever / probe here: $($unknown -join ', ')" }
  return @($table.Keys | Where-Object { $_ -eq 'base' -or $Levers -contains $_ })
}
$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$exeDir = Split-Path -Parent $Exe
$logs = Join-Path $exeDir 'logs'
New-Item -ItemType Directory -Force $Out | Out-Null
$settings = Join-Path $Out 'settings_native720.json'
Set-Content $settings '{"renderer": 2, "frame_rate": 1, "resolution": 0, "quality_tier": 2}'   # NATIVE, 60 fps, 720p (a test file)

# (Session 80, -Front: see the parameter.) Waits for the game's window, makes it topmost (above the taskbar) and the
# foreground window (an Alt press first: Windows lets a process that just had input move the foreground), and says
# whether it is the foreground window afterwards.
function Set-GameFront($proc, [string]$name) {
  if (-not ('WweBench.User32' -as [type])) {
    Add-Type -Namespace WweBench -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
  }
  $deadline = (Get-Date).AddSeconds(120)
  while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
  }
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { Write-Host "[surface] $name : front: no window"; return }
  Start-Sleep -Seconds 2  # (the window goes fullscreen right after it shows)
  [void][WweBench.User32]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040)  # TOPMOST, NOSIZE NOMOVE SHOWWINDOW
  [WweBench.User32]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)  # Alt down
  [void][WweBench.User32]::SetForegroundWindow($h)
  [WweBench.User32]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)  # Alt up
  Start-Sleep -Milliseconds 500
  $fg = [WweBench.User32]::GetForegroundWindow() -eq $h
  Write-Host "[surface] $name : front: topmost, foreground $fg"
}

# (Session 79, 04:30: -Threads samples every thread's CPU time once a second with thread_busy.ps1 - copied next to this
# script - into <step>\threads.csv; -Affinity limits the game to those logical processors, e.g. 0xF = 2 cores / 4
# threads, the i3-6100U's count. Read on the PC: tools\perf\thread_scenes.py <threads.csv> <the step's log>.)
function Invoke-Step([string]$name, [string]$route, [string[]]$flags, [bool]$freshData, [bool]$Threads = $false,
                     [int64]$Affinity = 0) {
  $work = Join-Path $Out "userdata_$name"
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }   # our own scratch copy only
  Copy-Item -Recurse $UserData $work
  if ($freshData) { $cache = Join-Path $work 'cache'; if (Test-Path $cache) { Remove-Item -Recurse -Force $cache } }
  $before = @(Get-ChildItem $logs -Filter 'wwe2k14_*.log' -ErrorAction SilentlyContinue | ForEach-Object Name)
  $gameArgs = @("--game_data_root=$GameData", "--user_data_root=$work", "--cache_root=$(Join-Path $work 'cache')",
            "--wwe2k14_settings_file=$settings", '--wwe2k14_use_launcher_settings=true',
            "--wwe2k14_replay_dir=$Replays", "--wwe2k14_input_script=$(Join-Path $Routes $route)",
            $(if ($Scenarios) { "--wwe2k14_scenario_dir=$Scenarios" }),
            '--wwe2k14_timeline=true', '--wwe2k14_native_frame=true') + $flags +
              @($GameFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { '--' + $_.TrimStart('-') })
  $gameArgs = @($gameArgs | Where-Object { $_ })
  Write-Host "[surface] $name : $route $($flags -join ' ')$(if ($Affinity) { ' (affinity 0x{0:X})' -f $Affinity })"
  $dest = Join-Path $Out $name
  New-Item -ItemType Directory -Force $dest | Out-Null
  $busy = $null
  $busyScript = Join-Path $PSScriptRoot 'thread_busy.ps1'
  if ($Threads -and (Test-Path $busyScript)) {
    # (Session 80: the PowerShell running this script, pwsh or Windows PowerShell.)
    $busy = Start-Process (Get-Process -Id $PID).Path -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                               $busyScript, '-Out', (Join-Path $dest 'threads.csv'),
                                               '-WaitForStart', '120') -PassThru -WindowStyle Hidden
  } elseif ($Threads) {
    Write-Host "[surface] $name : thread_busy.ps1 not next to bench.ps1 - no thread times"
  }
  $t0 = Get-Date
  $p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory $exeDir -PassThru
  if ($Affinity) { try { $p.ProcessorAffinity = [IntPtr]$Affinity } catch { Write-Host "[surface] affinity not set: $_" } }
  if ($Front) { Set-GameFront $p $name }
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { $p.Kill(); Write-Host "[surface] $name : timed out, killed" }
  if ($busy -and -not $busy.WaitForExit(30000)) { Stop-Process -Id $busy.Id -Force }
  Get-ChildItem $logs | Where-Object { $before -notcontains $_.Name -and $_.LastWriteTime -ge $t0 } |
    Where-Object { $_.Extension -in '.log', '.csv', '.txt' } | Copy-Item -Destination $dest
  Write-Host "[surface] $name : exit $($p.ExitCode), $([int]((Get-Date) - $t0).TotalSeconds) s"
}

if ($Only -contains 'passes') {
  Invoke-Step 'passes' 's72_gap_log.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                            '--vsync=false') $false
}
# passes_ram (Session 80, 14:20; not in the default list): the same per-pass log with gpu_ram_stats, which times every
# resolve's copy and texture fill on its own (segment kinds 6 / 7): the plugged-in run 14:01 had 11.0 of the
# entrance's 32.0 GPU ms between passes, at the scene's colour resolves and depth fills (gap_log.py).
if ($Only -contains 'passes_ram') {
  Invoke-Step 'passes_ram' 's72_gap_log.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                                '--gpu_ram_stats=true', '--vsync=false') $false
}
# multi (Session 80, not in the default list): the in-run A/B of SCENE_SHADERS_PLAN.md - five arms (the game's
# shaders, our crowd shaders, a flat wrestler body, the crowd skipped, the wrestler body skipped) in rotation every 8
# frames at Brock +300, Bryan +440 and the match +900 of the s21_lb replay (tools\native\passab_multi.py; read
# with its --read). Sequential runs drift with the Surface's heat (14:49: a lever that removes GPU work came out 47% slower
# as the 4th of 4 runs), so every comparison on it runs inside one run.
if ($Only -contains 'multi') {
  # (The route's own switches - e.g. scene_multi's --wwe2k14_scene_shaders=true / _probe=1 - come in -GameFlags:
  # a flag given twice drops every startup flag after it.)
  Invoke-Step 'multi' $MultiRoute @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true', '--vsync=false') $false
}
# arc (Session 80, not in the default list): the same kind of in-run A/B for the HD 520 arc's GPU parts (route
# s80_arc_multi.txt: all on = today, all off = the arc's start for GPU work, and alias loads / copy batching / cube
# faces / the crowd cull + thin off one at a time). PC check 5420 (4090): all off +20% / +34% / +9% GPU (Brock / Bryan /
# match).
if ($Only -contains 'arc') {
  Invoke-Step 'arc' 's80_arc_multi.txt' @('--wwe2k14_replay_play=s21_lb', '--gpu_native_gpu_timers=true',
                                         '--vsync=false') $false
}
if ($Only -contains 'frames') {
  Invoke-Step 'frames' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan') $false -Threads $true
}
# Session 89 (per-scene frames, not in the default list): one scene per run at NORMAL speed, fixed seed 1 (Bryan's camera
# variant pinned), entrances quitting 10 s after their step 15 and the match 10 s after its clock starts (Jarrod)
# (tools\input_scripts\scenes\; pushed flat into Routes with
# nav\boot_to_menu.txt, which they include). frames_bryan = Bryan out FIRST (participants 324,343), so his entrance
# starts ~6 s after the launch instead of after Brock's. The flags here repeat the routes' '# harness: flags' headers
# (the Surface bench does not read them). Read: py tools\native\frames_fps.py CSV name:first:last (prints the seed).
$sceneSteps = [ordered]@{
  'frames_bryan' = @('bryan_entrance.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_participants=324,343', '--wwe2k14_fixed_seed=1'))
  'frames_brock' = @('brock_entrance.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_fixed_seed=1'))
  'frames_match' = @('match.txt', @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_skip_entrances=true', '--wwe2k14_fixed_seed=1'))
}
foreach ($k in $sceneSteps.Keys) {
  if ($Only -contains $k) { Invoke-Step $k $sceneSteps[$k][0] $sceneSteps[$k][1] $false -Threads $true }
}
# pinned (Session 79, not in the default list): the frames run held to 2 cores / 4 threads (the i3-6100U's count; the
# Surface's i5 has 4 / 8), with every thread's CPU time: the CPU budget at 60 fps on a 4-thread laptop.
if ($Only -contains 'pinned') {
  Invoke-Step 'pinned' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan') $false -Threads $true -Affinity 0xF
}
if ($Only -contains 'mul') {
  Invoke-Step 'mul' 'profile_lesnar_bryan.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_intel_exact_mul=0') $false
}
# levers: each candidate lever as one fast-forwarded run of the frames route on top of --wwe2k14_native_frame (the GPU
# is the limit on Intel, so fast-forwarded frames per second per scene compare the GPU cost): base, then one change
# each. Read: bench_report.py report <run> per variant, scene by scene against "base".
if ($Only -contains 'levers') {
  $lever_flags = [ordered]@{
    'base'           = @()
    'no_crowd_thin'  = @('--gpu_scene_crowd_thin=0')   # (Session 80: the thinner distant crowd is everyone's default)
    'no_depth_clear' = @('--gpu_native_truth_clear_attachments=false')
    'no_dedupe'      = @('--gpu_native_truth_dedupe_state=false')
    'no_dead_glare'  = @('--gpu_native_frame_dead_glare=0')
    'no_alias_direct' = @('--gpu_native_truth_alias_direct=false')        # (Session 79 01:46: the stores back)
    'no_alias_rgba16' = @('--gpu_native_truth_alias_direct_rgba16=false') # (Session 79 05:18: the RGBA16 owners' stores back)
    'no_alias_lazy'  = @('--gpu_native_truth_alias_direct_lazy_flush=false')  # (Session 79 05:50: a flush before each)
    'no_defer_memory' = @('--gpu_native_truth_defer_memory_resolves=false')
    'no_defer_depth' = @('--gpu_native_truth_defer_depth_resolves=false')  # (Session 79 04:34: the depth copies batched)
    'no_defer_full'  = @('--gpu_native_truth_defer_full_resolves=false')   # (Session 79 04:56: the k_8 / k_4_4_4_4 copies)
    'no_cube_faces'  = @('--gpu_native_truth_texture_only_cube_faces=false', '--gpu_native_truth_fill_cube_faces=false')
    'mul_off'        = @('--wwe2k14_intel_exact_mul=0')
    # (Look levers: texture filtering. Since Session 80 players get the game's own (anisotropic_override -1; the forced
    # 4x before, 3); Gen9 filters anisotropically at a fraction of the bilinear rate. 0 = none.)
    'aniso_4x'       = @('--anisotropic_override=3')
    'aniso_off'      = @('--anisotropic_override=0')
    # (Session 80: the readback rules are everyone's default - this puts back Session 71's one limit, the character
    # windows' CPU / GPU waits.)
    'no_readback_narrow' = @('--wwe2k14_readback_skip_recurring_narrow=0')
    # (Session 80, look lever, off: the pixel shaders' maths at half precision, characters excepted. ps_relaxed = the
    # decoration only - does Intel's driver honour it at all; ps_half = real 16-bit floats. Shots: out\s80\review.)
    'ps_relaxed'     = @('--gpu_native_ps_relaxed_precision=true')
    'ps_half'        = @('--gpu_native_ps_relaxed_precision=true', '--gpu_native_ps_half=true')
    # (Session 80, off: the quiet pipeline workers raised when a skipped draw's pipeline is queued.)
    'workers_raise'  = @('--gpu_native_pipeline_workers_raise=true')
    # (Session 80, look lever, off - Jarrod: "Every other frame looks fine": the wrestlers' shadow maps on alternate
    # frames.)
    'shadow_half'    = @('--gpu_shadow_half_rate=true')
    # (Session 80, SCENE_SHADERS_PLAN.md step 1, off: our own scene material shaders - so far the crowd's pixel
    # shaders.)
    'scene_shaders'  = @('--wwe2k14_scene_shaders=true')
  }
  $chosen = Select-Levers $lever_flags
  foreach ($name in $chosen) {
    Invoke-Step "lever_$name" $AbRoute (@('--wwe2k14_launch=lesnar_bryan', '--vsync=false') + $lever_flags[$name]) $false
  }
  $Only += @($chosen | ForEach-Object { "lever_$_" })
}
# probes (Session 79, 04:05; not in the default list): what the frame's structure costs on Intel, as the levers step
# (one fast-forwarded frames run each against probe_base). Not candidate defaults - each turns a NATIVE part off.
# Dry run on the PC (out\s79\surface_dryrun4, 4 min 49 s): GPU per frame +25-30% with no_batch, the rest within ~5%.
# (A "no copies" ceiling was tried and dropped: the textures then reload from memory, so GPU time rose - not a ceiling.)
if ($Only -contains 'probes') {
  $probe_flags = [ordered]@{
    'base'              = @()
    'no_batch'          = @('--gpu_native_truth_batch_resolves=false', '--gpu_native_truth_batch_fills=false')  # one barrier pair a copy
    'no_merge_barriers' = @('--vulkan_merge_adjacent_barriers=false', '--vulkan_cancel_inverse_transitions=false')
    'no_dont_care'      = @('--gpu_native_truth_pass_dont_care=false', '--gpu_native_truth_color_none=false')  # load / store ops
    'no_crowd_batch'    = @('--gpu_scene_crowd_batch=false')   # a draw per crowd member: the front end's per-draw cost
    # (Session 80, SCENE_SHADERS_PLAN.md step 0: what each target material costs on Intel, its draws not drawn - the
    # most our own shader could save. The debug skip also turns the lean draw path off: compare GPU-bound scenes.)
    'skip_crowd'        = @('--gpu_debug_skip_vertex_shader=183CC64A4C4E96AC,0E2A3DF5BAA2CA18,C1028A64EFCF8229,CB5B16DA1312E5AB')
    'skip_wrestler_body' = @('--gpu_debug_skip_vertex_shader=F709D14F30FCC95C')
    'skip_4f11'         = @('--gpu_debug_skip_vertex_shader=4F1118D4B3F6D388')
    # (Session 80: a flat grey in place of the wrestlers' body material, src\scene_swap.h - the most our own shader for
    # it could save on its pixel side; skip_wrestler_body minus this = its vertices and rasterising.)
    'flat_wrestler_body' = @('--wwe2k14_scene_shaders_probe=1')
  }
  $chosen = Select-Levers $probe_flags
  foreach ($name in $chosen) {
    Invoke-Step "probe_$name" $AbRoute (@('--wwe2k14_launch=lesnar_bryan', '--vsync=false') + $probe_flags[$name]) $false
  }
  $Only += @($chosen | ForEach-Object { "probe_$_" })
}
# (-ColdDriver: Intel's driver cache moved aside for the step and back afterwards; the step's own cache is kept.)
function Invoke-DriverCold([string]$Name, [scriptblock]$Body) {
  $driverCache = Join-Path $env:USERPROFILE 'AppData\LocalLow\Intel\ShaderCache'
  $aside = "$driverCache.aside_${stamp}_$Name"
  if ($ColdDriver -and (Test-Path $driverCache)) { Rename-Item $driverCache $aside; Write-Host "[surface] driver cache moved aside" }
  try { & $Body } finally {
    if ($ColdDriver -and (Test-Path $aside)) {
      if (Test-Path $driverCache) { Rename-Item $driverCache "$driverCache.cold_${stamp}_$Name" }
      Rename-Item $aside $driverCache
      Write-Host "[surface] driver cache moved back (the step's own cache kept as ShaderCache.cold_${stamp}_$Name)"
    }
  }
}
if ($Only -contains 'cold') {
  Invoke-DriverCold 'cold' {
    Invoke-Step 'cold' 's78_anatomy_720.txt' @('--wwe2k14_launch=lesnar_bryan', '--wwe2k14_preset_low=true',
                                               '--vsync=false') $true
  }
}
# precompile (Session 80, not in the default list): a player's first launch - the launcher's PREPARING SHADERS run
# (--wwe2k14_precompile: every stored NATIVE pipeline on a worker per CPU thread, here only the shipped list's, our cache
# emptied; the game quits when done). Its progress JSON (made, total, seconds) lands in the step's folder. With
# -ColdDriver: the first launch after an install or a driver update (the arc's 30 s target on the HD 520).
if ($Only -contains 'precompile') {
  Invoke-DriverCold 'precompile' {
    Invoke-Step 'precompile' 's80_precompile_idle.txt' @("--wwe2k14_precompile=$(Join-Path $Out 'precompile\progress.json')",
                                                          '--vsync=false') $true
  }
}
$zip = Join-Path $Out "surface_bench_$stamp.zip"
$dirs = @(Get-ChildItem $Out -Directory | Where-Object Name -in $Only | ForEach-Object FullName)
if ($dirs.Count) {
  Compress-Archive -Path $dirs -DestinationPath $zip
  Write-Host "[surface] done: $zip"
} else {
  Write-Host "[surface] nothing ran (-Only $($Only -join ','))"
}
