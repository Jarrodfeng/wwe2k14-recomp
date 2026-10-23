# Session 52 (from s20_nsight_trace.ps1): an Nsight Graphics GPU Trace of NATIVE frames with any route (-Route, relative
# to the repo; default short\s52_trace.txt, the s21_lb replay to the match: pass -Extra "--wwe2k14_replay_play=s21_lb
# --wwe2k14_preset_low=true" for LOW at 720p). Mirrors the frozen user data, launches through ngfx.exe. Exclusive lock:
#   pwsh -NoProfile -File tools\runlock.ps1 run -Name s52-nsight -Exclusive -- pwsh -NoProfile -File tools\perf\s52_nsight_trace.ps1 -Extra "..."
# The trace (game-derived) lands in out\harness\step20\nsight\ (local only).
param([int]$StartAfterFrames = 7100, [int]$Frames = 3, [string]$Extra = "", [int]$TimeoutSeconds = 600,
      [string]$Route = 'tools\input_scripts\short\s52_trace.txt', [string]$Launch = 'lesnar_bryan')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe'
$exe = "$R\out\build\win-amd64-release\wwe2k14.exe"
$out = "$R\out\harness\step20\nsight"
New-Item -ItemType Directory -Force $out | Out-Null
robocopy "$R\out\bench\userdata_frozen" "$R\out\bench\userdata_work" /MIR /NFL /NDL /NJH /NJS | Out-Null
$route = "$R\$Route"
$gameArgs = "--user_data_root=$R\out\bench\userdata_work --cache_root=$R\out\bench\userdata_work\cache " +
            "--wwe2k14_input_script=$route --wwe2k14_launch=$Launch --wwe2k14_fixed_seed=1 --vsync=false $Extra"
$a = @('--activity', 'GPU Trace Profiler', '--exe', $exe, '--dir', (Split-Path $exe), '--args', $gameArgs.Trim(),
       '--output-dir', $out, '--start-after-frames', "$StartAfterFrames", '--limit-to-frames', "$Frames",
       '--max-duration-ms', '5000', '--auto-export', '--no-timeout')
"$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ngfx start: frames $StartAfterFrames +$Frames"
$p = Start-Process -FilePath $ngfx -ArgumentList ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -PassThru -NoNewWindow -RedirectStandardOutput "$out\ngfx_stdout.txt" -RedirectStandardError "$out\ngfx_stderr.txt"
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    "timeout after $TimeoutSeconds s: stopping ngfx and the game"
    Get-Process wwe2k14 -ErrorAction SilentlyContinue | Stop-Process -Force
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
"$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ngfx exit $($p.ExitCode)"
Get-Content "$out\ngfx_stdout.txt" -Tail 15
Get-Content "$out\ngfx_stderr.txt" -Tail 10
Get-ChildItem $out | Sort-Object LastWriteTime -Descending | Select-Object -First 8 Name, Length, LastWriteTime
