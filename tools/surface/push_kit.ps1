# Session 80 (HD 520 arc, item 3): copies the Surface bench's kit from this PC to the Surface over SSH (host alias
# "surface" in ~\.ssh\config) into C:\wwe: the build laid out as a release zip has it (exe, DLLs, the VC++ runtime, the
# shader list, the id tables), the frozen user data, the bench's routes, the s21_lb replay, the lesnar_bryan scenario
# (local only: it never ships) and the bench scripts. Never the game files: Jarrod copies his own
# (game_data\iso\wwe2k14_test.iso -> C:\wwe\wwe2k14_test.iso; ~3-4 min at the 38 MB/s measured 2026-11-01).
#   pwsh -NoProfile -File tools\surface\push_kit.ps1 [-HostName surface] [-Dest C:/wwe]
#   -Build DIR   another build's exe folder (Session 82: e.g. out\harness\bisect\exe\sdk_<commit9> - a worktree SDK build
#                next to the main exe; its rex*.dll are unpinned, as every bisect build's)
#   -Symbols     also copy wwe2k14.pdb (S102 S0-6: the game exe's own symbols, made by tools\build.ps1 -Symbols; optional, never committed or
#                shipped, docs\harness\symbols.md) and rexgpu-xenos.pdb / rexruntime.pdb (~170 MB) so profile runs name our functions (Session 82:
#                without them every rexgpu sample showed as `rex_gpu_create`)
#   -Lab         S102 M2 (the Surface lab, tools\surface\lab.py): push only the replay player, out\renderer\surface\rplay.exe (built by
#                tools\renderer\build.ps1 -Surface: our code, its shaders are compiled in, no game data, C runtime linked in), to <Dest>/lab/rplay.exe.
#                Captures go separately: py -I tools\surface\lab.py push out\captures\NAME (they hold game data and stay on Jarrod's machines).
param([string]$HostName = 'surface', [string]$Dest = 'C:/wwe', [string]$Build = '', [switch]$Symbols, [switch]$Lab)
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($Lab) {   # S102 M2
    $exe = Join-Path $R 'out\renderer\surface\rplay.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "no $exe (pwsh -NoProfile -File tools\renderer\build.ps1 -Surface)" }
    ssh -o BatchMode=yes $HostName "if not exist `"$(($Dest + '/lab') -replace '/', '\')`" mkdir `"$(($Dest + '/lab') -replace '/', '\')`"" | Out-Null
    scp -q -o BatchMode=yes $exe "${HostName}:$Dest/lab/rplay.exe"
    if ($LASTEXITCODE) { throw "scp of rplay.exe failed ($LASTEXITCODE)" }
    # the hash lab.py checks before every job (same file lab.py push-exe writes): a jobs-time rplay.exe that is not this one fails "exe hash unknown or changed"
    $shaFile = Join-Path ([IO.Path]::GetTempPath()) 'rplay.sha256'
    [IO.File]::WriteAllText($shaFile, (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower() + "`n")
    scp -q -o BatchMode=yes -o ConnectTimeout=10 $shaFile "${HostName}:$Dest/lab/rplay.sha256"
    if ($LASTEXITCODE) { throw "scp of rplay.sha256 failed ($LASTEXITCODE)" }
    "rplay.exe + rplay.sha256 -> ${HostName}:$Dest/lab/ ({0:N1} MB)" -f ((Get-Item -LiteralPath $exe).Length / 1MB)
    return
}
$build = if ($Build) { (Resolve-Path $Build).Path } else { Join-Path $R 'out\build\win-amd64-release' }
$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'no Visual C++ redist folder (Microsoft.VC143.CRT) found' }

# remote folder -> local files
$groups = [ordered]@{
    'build'          = @('wwe2k14.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'amd_fidelityfx_vk.dll' | ForEach-Object { Join-Path $build $_ }) +
                       @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll' | ForEach-Object { Join-Path $crt.FullName $_ }) +
                       @(Join-Path $R 'db\shaders\shader_list.txt')
    'profiles'       = @(Get-ChildItem (Join-Path $R 'tools\surface\profiles') -Filter *.json | ForEach-Object FullName)   # S102 S0-1: bench.ps1 -Profile (main 05:15: every profile, not a hand list: auto_dh540 never reached the Surface)
    'build/data/ids' =@('characters.json', 'arenas.json' | ForEach-Object { Join-Path $R "db\ids\$_" })
    'routes'         = @('s95_gpu_profile.txt', 's95_shader_rank.txt', 's72_gap_log.txt', 'profile_lesnar_bryan.txt', 's78_anatomy_720.txt', 's80_precompile_idle.txt', 's80_probe_short.txt', 's80_scene_multi.txt', 's80_arc_multi.txt', 's80_owncrowd_multi.txt', 's80_wrestlers_multi.txt', 's80_wrestprops_multi.txt', 's80_gear_multi.txt', 's80_gear2_multi.txt', 's80_wmat_multi.txt', 's80_wall11_multi.txt', 's80_depthwb_multi.txt', 's80_deadcopy_multi.txt', 's80_deadpass_multi.txt', 's80_servecopy_multi.txt', 's80_arena10_multi.txt', 's80_arena23_multi.txt', 's80_arenaside_multi.txt', 's80_arenaall_multi.txt', 's80_clearload_multi.txt', 's80_clearalias_multi.txt', 's80_bounds_multi.txt', 's80_tonight_multi.txt', 's80_shadowcost_multi.txt', 's80_ownring_multi.txt', 's80_ownring2_multi.txt', 's80_ownpass_multi.txt', 's80_owned_multi.txt', 's80_owned_levers_multi.txt', 's80_owned_levers2_multi.txt' |
                         ForEach-Object { Join-Path $R "tools\input_scripts\short\$_" }) +
                       # (Session 89: the per-scene routes and the nav snippet they include, flat beside the others.)
                       @('bryan_entrance.txt', 'bryan_heavy.txt', 'brock_entrance.txt', 'match.txt' | ForEach-Object { Join-Path $R "tools\input_scripts\scenes\$_" }) +
                       @(Join-Path $R 'tools\input_scripts\nav\boot_to_menu.txt') +
                       # (S105 SRJ: Only=route jobs: the Move-set stall route and the nav snippets it includes, flat as the others.)
                       @('s103_moveset_stall.txt' | ForEach-Object { Join-Path $R "tools\input_scripts\scenes\$_" }) +
                       @('menu_to_creations.txt', 'creations_to_moveset_specials.txt' | ForEach-Object { Join-Path $R "tools\input_scripts\nav\$_" }) +
                       # (review: the scene routes use wait_state / press_until_state, and the game finds states.toml
                       # beside the script - src\game_state.h FindStatesFile.)
                       @(Join-Path $R 'tools\input_scripts\states.toml')
    'replays'        = @(Join-Path $R 'out\replays\s21_lb.rec')
    'scenarios'      = @(Get-ChildItem (Join-Path $R 'out\scenarios') -Filter 'lesnar_bryan.*' | ForEach-Object FullName)
    '.'              = @((Join-Path $R 'tools\surface\bench.ps1'), (Join-Path $R 'tools\perf\thread_busy.ps1'), (Join-Path $R 'tools\surface\power_log.ps1'), (Join-Path $R 'tools\surface\heat_steady.ps1'),
                         (Join-Path $R 'tools\surface\run_bench.ps1'))
}
if ($Symbols) {
    $groups['build'] += @('rexgpu-xenos.pdb', 'rexruntime.pdb' | ForEach-Object { Join-Path $build $_ })
    # S102 S0-6: the exe's own pdb, only when tools\build.ps1 -Symbols made it. One older than the exe names the wrong code: refused.
    $exePdb = Join-Path $build 'wwe2k14.pdb'
    if (Test-Path -LiteralPath $exePdb) {
        if ((Get-Item -LiteralPath $exePdb).LastWriteTime -lt (Get-Item -LiteralPath (Join-Path $build 'wwe2k14.exe')).LastWriteTime.AddMinutes(-5)) {
            throw 'wwe2k14.pdb is older than wwe2k14.exe: rebuild with tools\build.ps1 -Symbols'
        }
        $groups['build'] += $exePdb
    } else { 'note: no wwe2k14.pdb next to the exe (tools\build.ps1 -Symbols makes it): the profile will not name our code' }
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
