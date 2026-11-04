# Pre-flight for runs of a staged exe folder (Session 82, harness builder): refuses BEFORE any game is launched when
# something a run needs is missing (the failures it prevents: a game that plays the attract demo because the replay
# folder flag is absent, a missing amd_fidelityfx_vk.dll, a stale folder, no free RAM for the lanes).
#   & .\tools\owned\preflight.ps1 -Exe C:\Users\Jarrod\wwe2k14-recomp\out\harness\bisect\exe\live_p0\wwe2k14.exe
#   -Entrance  also checks the quick entrance replay's files    -MinFreeGB N (default 10): free RAM wanted for 3 lanes
# Prints one line per check ("ok" / "FAIL"); exit 0 when all pass, 1 otherwise. Reads only.
param([Parameter(Mandatory)][string]$Exe, [switch]$Entrance, [double]$MinFreeGB = 10, [string]$ExeSource = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$fails = 0
function Chk([string]$what, [bool]$ok, [string]$detail = '') {
    if ($ok) { "  ok    $what" } else { $script:fails++; "  FAIL  $what$(if ($detail) { ' - ' + $detail })" }
}
$exeFull = [System.IO.Path]::GetFullPath($Exe)
$dir = Split-Path -Parent $exeFull
"pre-flight for $exeFull"
Chk 'the exe exists' (Test-Path -LiteralPath $exeFull -PathType Leaf) $exeFull
Chk 'not the main exe folder (its DLLs are pinned; builders never run from or write it)' ($dir -ine "$R\out\build\win-amd64-release") 'use a staged folder (tools\owned\fast_build.ps1 / wt_build.ps1)'
foreach ($n in 'rexgpu-xenos.dll', 'rexruntime.dll', 'amd_fidelityfx_vk.dll') {
    $f = Join-Path $dir $n
    Chk "$n present" ((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).Length -gt 100000) $f
}
foreach ($n in 'rexgpu-xenos.dll', 'rexruntime.dll') {
    $f = Join-Path $dir $n
    if (Test-Path -LiteralPath $f) {
        $age = ((Get-Date) - (Get-Item -LiteralPath $f).LastWriteTime).TotalHours
        Chk "$n is not older than 12 h (a stale build?)" ($age -lt 12) ('{0:N1} h old: build again' -f $age)
    }
}
# A live_<Name> folder (tools\owned\fast_build.ps1) must carry the CURRENT main exe: fast_build re-copies it when the main
# folder's copy differs; a stale one means the folder was not built since the main exe was rebuilt.
if ((Split-Path -Leaf $dir) -like 'live_*') {
    foreach ($n in 'wwe2k14.exe', 'amd_fidelityfx_vk.dll') {
        $m = Join-Path $(if ($ExeSource) { $ExeSource } else { "$R\out\build\win-amd64-release" }) $n; $f = Join-Path $dir $n
        if ((Test-Path -LiteralPath $m) -and (Test-Path -LiteralPath $f)) {
            $same = ((Get-Item -LiteralPath $m).Length -eq (Get-Item -LiteralPath $f).Length) -and ((Get-Item -LiteralPath $m).LastWriteTimeUtc -eq (Get-Item -LiteralPath $f).LastWriteTimeUtc)
            Chk "$n equals the main folder's copy (a live_ folder follows the main exe)" $same 'run tools\owned\fast_build.ps1 again: it re-copies the main exe files'
        }
    }
}
Chk 'no BROKEN marker (a fast_build that failed between two DLL renames leaves mixed DLLs)' (-not (Test-Path -LiteralPath (Join-Path $dir 'BROKEN'))) 'run tools\owned\fast_build.ps1 again'
$inUse = @(Get-Process -Name wwe2k14 -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and $_.Path -like "$dir\*" } catch { $false } })
Chk 'no game running from this folder (it would lock the DLLs)' ($inUse.Count -eq 0) "pid $(($inUse | ForEach-Object Id) -join ',')"
Chk 'game data: game_data\extracted\default.xex' (Test-Path "$R\game_data\extracted\default.xex")
Chk 'replays: out\replays\s80_lb_quick.rec (the flag --wwe2k14_replay_dir=out\replays is required)' (Test-Path "$R\out\replays\s80_lb_quick.rec")
Chk 'scenarios: out\scenarios exists (the flag --wwe2k14_scenario_dir=out\scenarios)' (Test-Path "$R\out\scenarios" -PathType Container)
Chk 'the check routes exist (short\s82_match_shots_fast.txt, s82_match_shots_census.txt)' ((Test-Path "$R\tools\input_scripts\short\s82_match_shots_fast.txt") -and (Test-Path "$R\tools\input_scripts\short\s82_match_shots_census.txt"))
if ($Entrance) {
    Chk 'the entrance replay out\replays\s82_lb_quick_ent.rec' (Test-Path "$R\out\replays\s82_lb_quick_ent.rec")
    Chk 'the entrance route short\s82_ent_shots.txt' (Test-Path "$R\tools\input_scripts\short\s82_ent_shots.txt")
}
Chk 'frozen user data out\bench\userdata_frozen' (Test-Path "$R\out\bench\userdata_frozen" -PathType Container)
$free = [double](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
Chk ("free RAM {0:N1} GB >= {1:N0} GB" -f $free, $MinFreeGB) ($free -ge $MinFreeGB) 'close something or wait: three lanes of games need it'
$games = @(Get-Process -Name wwe2k14 -ErrorAction SilentlyContinue).Count
"  info  $games wwe2k14 process(es) running now (the run lock lets at most 3 shared runs at once)"
if ($fails) { "pre-flight: $fails check(s) FAILED - nothing launched"; exit 1 }
"pre-flight: all checks passed"
exit 0
