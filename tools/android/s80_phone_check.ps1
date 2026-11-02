# Session 80 (Jarrod: "do the android work preliminary that you don't need my phone for"): the first phone check of
# the arc build (SDK step 85, APK from tools\android\build_all.ps1). One phone run at a time (run.ps1), each under 2
# minutes; Jarrod watches the screen. Then the frame times side by side (demo_fps.py --marks).
#   & .\tools\android\s80_phone_check.ps1 -First 162            all four runs, numbered 162-165
#   & .\tools\android\s80_phone_check.ps1 -First 162 -Only arc,off   some of them
# Runs (Cena vs Undertaker, WrestleMania XXX, CPU vs CPU, entrances skipped - bench_cena_taker_match.txt):
#   arc    the build's defaults (the arc on: native frame, our scene shaders and crowd, ...)
#   off    the arc off (--wwe2k14_native_frame=false): how much the arc saves on this phone
#   none   the arc + --gpu_native_truth_color_none=true: does Session 80's color_none fix cure the white patches /
#          black flashes of Session 74 (Jarrod looks; off on phones until a phone run says otherwise)
#   tag    the tag match replay (xdet_cov_tag, cover_3600.txt) with the phone's default render guard: Session 77's
#          crash at the bell (fixed 10-31, first Android release with it) - plays through = fixed
# Notes: on the phone's render path (host render targets) the owned shadow steps and the clear-load step barely take
# (PC with --wwe2k14_test_no_interlock=true: ~1 owned draw a frame vs ~140; tools\native\owned_verdict.py "MOSTLY
# REFUSED") - the arc's other parts run. Install the APK first: pwsh -File tools\android\build_all.ps1 -Run X -Install
# (or adb install -r out\android\apk\wwe2k14-android-dev.apk).
param([Parameter(Mandatory)][int]$First, [string[]]$Only = @('arc', 'off', 'none', 'tag'))
$root = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $root
$match = "--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=56 --wwe2k14_launch_participants=139,103 " +
         "--wwe2k14_launch_skip_entrances=true"
$route = 'tools\input_scripts\android\bench_cena_taker_match.txt'
$plan = [ordered]@{
  arc  = @{ Args = $match; Route = $route; Scenario = 'lesnar_bryan'; Seconds = 120 }
  off  = @{ Args = "$match --wwe2k14_native_frame=false"; Route = $route; Scenario = 'lesnar_bryan'; Seconds = 120 }
  none = @{ Args = "$match --gpu_native_truth_color_none=true"; Route = $route; Scenario = 'lesnar_bryan'; Seconds = 120 }
  tag  = @{ Args = ''; Route = 'tools\xdet\cover_3600.txt'; Replay = 'xdet_cov_tag'; Seconds = 110 }
}
$n = $First
$done = @{}
foreach ($kind in $plan.Keys) {
  if ($Only -notcontains $kind) { continue }
  $p = $plan[$kind]
  $run = '{0:D3}' -f $n
  "== $kind -> phone run $run"
  $a = @{ Run = $run; Seconds = $p.Seconds; Shots = 2; Route = $p.Route }
  if ($p.Args) { $a.GameArgs = $p.Args }
  if ($p.Scenario) { $a.Scenario = $p.Scenario }
  if ($p.Replay) { $a.Replay = $p.Replay }
  & "$root\tools\android\run.ps1" @a 2>&1 | Select-Object -Last 6
  $done[$kind] = $run
  $n++
}
$bench = @('arc', 'off', 'none') | Where-Object { $done.Contains($_) } | ForEach-Object { $done[$_] }
if ($bench.Count) {
  "== frame times (" + (($done.Keys | ForEach-Object { "$_ $($done[$_])" }) -join ', ') + ")"
  py -I "$root\tools\android\demo_fps.py" @bench --marks
}
