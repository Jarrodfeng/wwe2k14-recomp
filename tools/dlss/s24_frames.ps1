# Session 24 (DLSS): render the same replay frames in several arms and keep lossless PNGs of chosen guest frames, for
# exact quality comparisons (tools\dlss\compare.py). Plays out\replays\s21_lb.rec on the Session 21 hash route
# (tools\input_scripts\short\s21_hash.txt: frame-locked clip from Brock's titantron to match +2,000), with
# --async_shader_compilation=false (deterministic rendering) and --wwe2k14_clip_png=FRAMES.
#   & .\tools\dlss\s24_frames.ps1 -Tag T -Arms @('native2=', 'dlss_perf=--wwe2k14_dlss=true ...', ...) [-Parallel 2]
#       [-Frames 'A-B/S,...'] [-RealTime]
# Output: out\dlss_s24\frames\T\ARM\RUN_clip_h_fGUEST.png (+ the run's clip json / hashes), and T\arms.txt
# (arm = run). Local only (game frames).
param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string[]]$Arms, [int]$Parallel = 2,
      [string]$Frames = '2457,2700,5771-5891/30,5830-5833,7600,7800,10200,10500,10850-10872/2,11262-11278/4,11700',
      [string]$Common = '', [switch]$RealTime, [string]$Route = 'tools\input_scripts\short\s21_hash.txt')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$route = "$R\$Route"   # -Route: e.g. tools\input_scripts\short\s24_pan.txt (Bryan's ramp pan only)
$root = "$R\out\dlss_s24\frames\$Tag"
New-Item -ItemType Directory -Force $root | Out-Null
$scripts = @(); $extras = @(); $names = @()
foreach ($a in $Arms) {
    $i = $a.IndexOf('=')
    $name = $a.Substring(0, $i); $flags = $a.Substring($i + 1)
    $dir = "$root\$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $scripts += $route; $names += $name
    $extras += ("--wwe2k14_replay_play=s21_lb --async_shader_compilation=false --wwe2k14_clip_dir=$dir " +
                "--wwe2k14_clip_frame_locked=true --wwe2k14_clip_hash_log=true --wwe2k14_clip_png=$Frames " +
                "$Common $flags").Trim()
}
$sw = @{ RealTime = [bool]$RealTime }
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras @sw -Parallel $Parallel -WaitSeconds 1500 2>&1
$lines = @($out | Where-Object { "$_" -match ' -> wwe2k14_(\d+)' })
$lines | ForEach-Object { "frames: $_" }
# Which run is which arm: each arm's clip dir holds its run's files.
$map = @()
foreach ($n in $names) {
    $json = Get-ChildItem -LiteralPath "$root\$n" -Filter '*_clip_h.json' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $run = if ($json) { ($json.Name -split '_')[0] } else { '?' }
    $pngs = @(Get-ChildItem -LiteralPath "$root\$n" -Filter "${run}_clip_h_f*.png" -ErrorAction SilentlyContinue).Count
    $map += "$n=$run ($pngs PNGs)"
}
$map | Set-Content -LiteralPath "$root\arms.txt"
$map | ForEach-Object { "arm: $_" }
