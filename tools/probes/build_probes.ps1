# Session 30 (#4): build only the probe plugin (probes/ -> wwe2k14_probes.dll next to the game), WHILE a game runs.
#   pwsh -File tools\probes\build_probes.ps1 [-Preset win-amd64-coverage]
# then, in the running coverage-build game: live.py run "plugin reload" (or a script's `plugin reload`).
# No run lock, on purpose (the one exception to "every build queues on the run lock", which exists so a build never
# replaces the exe or SDK DLLs under a running game): this builds one small DLL that no game holds open (the game loads
# a copy of it, src/probe_plugin.h), and a hot reload is pointless if it has to wait for the game to quit. If the
# CMake files changed, ninja re-runs CMake first (that writes only build files). The exe is never touched: the target
# has no dependency on it.
param([string]$Preset = 'win-amd64-coverage')
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\env.ps1" | Out-Null
$dir = "$R\out\build\$Preset"
# Review (Session 30): never next to a locked build (two ninja processes in one build folder; a CMake re-run could
# rewrite build.ninja under it). A running game is fine: that is the point of this script.
. "$R\tools\runlock.ps1"
$st = Get-RunLockState
if ($st.LockAlive -and "$($st.Lock.name)" -match 'build') { "ABORT: a build holds the run lock ($($st.Lock.name)); run this again when it is done"; exit 3 }
if (-not (Test-Path "$dir\build.ninja")) { "ABORT: $dir has no build.ninja (configure that preset first: tools\build.ps1 -Preset $Preset)"; exit 2 }
$t0 = Get-Date
$out = & ninja -C $dir wwe2k14_probes 2>&1
$code = $LASTEXITCODE
$out | Select-Object -Last 6
$dll = Get-ChildItem "$dir\wwe2k14_probes.dll" -ErrorAction SilentlyContinue
"build_probes: exit $code in {0:N1} s; {1}" -f ((Get-Date) - $t0).TotalSeconds, $(if ($dll) { "$($dll.FullName) ($($dll.LastWriteTime.ToString('HH:mm:ss')))" } else { 'no DLL' })
exit $code
