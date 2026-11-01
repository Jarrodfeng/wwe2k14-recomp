# Session 80 (HD 520 arc, item 3): copies the Surface bench's kit from this PC to the Surface over SSH (host alias
# "surface" in ~\.ssh\config) into C:\wwe: the build laid out as a release zip has it (exe, DLLs, the VC++ runtime, the
# shader list, the id tables), the frozen user data, the bench's routes, the s21_lb replay, the lesnar_bryan scenario
# (local only: it never ships) and the bench scripts. Never the game files: Jarrod copies his own
# (game_data\iso\wwe2k14_test.iso -> C:\wwe\wwe2k14_test.iso; ~3-4 min at the 38 MB/s measured 2026-11-01).
#   pwsh -NoProfile -File tools\surface\push_kit.ps1 [-HostName surface] [-Dest C:/wwe]
param([string]$HostName = 'surface', [string]$Dest = 'C:/wwe')
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$build = Join-Path $R 'out\build\win-amd64-release'
$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'no Visual C++ redist folder (Microsoft.VC143.CRT) found' }

# remote folder -> local files
$groups = [ordered]@{
    'build'          = @('wwe2k14.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'amd_fidelityfx_vk.dll' | ForEach-Object { Join-Path $build $_ }) +
                       @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll' | ForEach-Object { Join-Path $crt.FullName $_ }) +
                       @(Join-Path $R 'db\shaders\shader_list.txt')
    'build/data/ids' = @('characters.json', 'arenas.json' | ForEach-Object { Join-Path $R "db\ids\$_" })
    'routes'         = @('s72_gap_log.txt', 'profile_lesnar_bryan.txt', 's78_anatomy_720.txt', 's80_precompile_idle.txt', 's80_probe_short.txt', 's80_scene_multi.txt', 's80_arc_multi.txt', 's80_owncrowd_multi.txt', 's80_wrestlers_multi.txt', 's80_wrestprops_multi.txt', 's80_gear_multi.txt', 's80_gear2_multi.txt', 's80_wmat_multi.txt', 's80_wall11_multi.txt', 's80_depthwb_multi.txt', 's80_deadcopy_multi.txt', 's80_deadpass_multi.txt', 's80_servecopy_multi.txt', 's80_arena10_multi.txt', 's80_arena23_multi.txt', 's80_arenaside_multi.txt', 's80_arenaall_multi.txt' |
                         ForEach-Object { Join-Path $R "tools\input_scripts\short\$_" })
    'replays'        = @(Join-Path $R 'out\replays\s21_lb.rec')
    'scenarios'      = @(Get-ChildItem (Join-Path $R 'out\scenarios') -Filter 'lesnar_bryan.*' | ForEach-Object FullName)
    '.'              = @((Join-Path $R 'tools\surface\bench.ps1'), (Join-Path $R 'tools\perf\thread_busy.ps1'),
                         (Join-Path $R 'tools\surface\run_bench.ps1'))
}
foreach ($dir in $groups.Keys) {
    $files = @($groups[$dir])
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missing) { throw "missing: $($missing -join ', ')" }
    $remote = if ($dir -eq '.') { $Dest } else { "$Dest/$dir" }
    ssh -o BatchMode=yes $HostName "if not exist `"$($remote -replace '/', '\')`" mkdir `"$($remote -replace '/', '\')`"" | Out-Null
    $t = Get-Date
    scp -q -o BatchMode=yes @files "${HostName}:$remote/"
    if ($LASTEXITCODE) { throw "scp to $remote failed ($LASTEXITCODE)" }
    $mb = ($files | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum / 1MB
    '{0,-15} {1,2} files, {2,6:N1} MB, {3,5:N1} s' -f $dir, $files.Count, $mb, ((Get-Date) - $t).TotalSeconds
}
# The frozen user data (the save the replays were recorded on: Jarrod's own save data, not a game file), whole folder.
$t = Get-Date
scp -q -r -o BatchMode=yes (Join-Path $R 'out\bench\userdata_frozen') "${HostName}:$Dest/"
if ($LASTEXITCODE) { throw "scp of userdata_frozen failed ($LASTEXITCODE)" }
'{0,-15} folder, {1,5:N1} s' -f 'userdata_frozen', ((Get-Date) - $t).TotalSeconds
"kit in ${HostName}:$Dest"
