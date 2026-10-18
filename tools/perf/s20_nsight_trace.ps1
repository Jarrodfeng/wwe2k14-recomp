# Session 20 item 1: an Nsight Graphics GPU Trace of Bryan's ramp pan. Mirrors the frozen user data and launches the
# game through ngfx.exe (GPU Trace Profiler) with the profile route, seed 1, fast-forwarded (no timeline). Run it
# under the exclusive run lock:
#   pwsh -NoProfile -File tools\runlock.ps1 run -Name s20-nsight -Exclusive -- pwsh -NoProfile -File tools\perf\s20_nsight_trace.ps1 [-StartAfterFrames 6260] [-Frames 3]
# The trace (game-derived) lands in out\harness\step20\nsight\ (local only).
param([int]$StartAfterFrames = 6260, [int]$Frames = 3, [string]$Extra = '', [int]$TimeoutSeconds = 600)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe'
$exe = "$R\out\build\win-amd64-release\wwe2k14.exe"
$out = "$R\out\harness\step20\nsight"
New-Item -ItemType Directory -Force $out | Out-Null
robocopy "$R\out\bench\userdata_frozen" "$R\out\bench\userdata_work" /MIR /NFL /NDL /NJH /NJS | Out-Null
$route = "$R\tools\input_scripts\short\profile_lesnar_bryan.txt"
$gameArgs = "--user_data_root=$R\out\bench\userdata_work --cache_root=$R\out\bench\userdata_work\cache " +
            "--wwe2k14_input_script=$route --wwe2k14_launch=lesnar_bryan --wwe2k14_fixed_seed=1 --vsync=false $Extra"
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
