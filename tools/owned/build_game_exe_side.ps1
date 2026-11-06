# Session 93 (builder s93-window): build the GAME exe from the current working tree into a SIDE folder, without touching the main
# exe folder (out\build\win-amd64-release stays as it is: other builders' staged live_<Name> folders copy it).
#   & .\tools\owned\build_game_exe_side.ps1 -Out C:\Users\Jarrod\wwe2k14-recomp\out\harness\game_exe\s93window
#   then: & .\tools\owned\fast_build.ps1 -Worktree <sdk wt> -Name <n> -ExeSource C:\Users\Jarrod\wwe2k14-recomp\out\harness\game_exe\s93window
#         (fast_build re-copies the exe files from -ExeSource into live_<n>; tools\owned\preflight.ps1 -ExeSource <same> checks them)
#
# Why: our own code in the exe is src\main.cpp (+ the headers it includes: wwe2k14_app.h, scene_vs_swap.h, the SPIR-V headers of
# src\scene\spirv ...). The 345 generated recompiled-game objects and the SDK app sources do not depend on those, so this
# compiles ONLY main.cpp (with the main build's own flags and precompiled header, read only) into <Out>\main.obj and links it with
# the main build's other objects (read only) into <Out>\wwe2k14.exe (+ .map). The rex*.dll next to it are NOT copied (fast_build
# stages the SDK worktree's DLLs next to the exe). Takes the BUILD class of the run lock like fast_build.
# Not for releases (the main build with its pinned SDK is the only release path).
param([Parameter(Mandatory)][string]$Out, [double]$LockWaitMinutes = 30)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$B = "$R\out\build\win-amd64-release"
$Out = [System.IO.Path]::GetFullPath($Out).TrimEnd('\')
if ($Out -ieq $B) { 'build_game_exe_side: refusing the main exe folder'; exit 2 }
. "$R\tools\env.ps1" *> $null
. "$R\tools\runlock.ps1"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$ninja = Get-Content -LiteralPath "$B\build.ninja"
Push-Location $B
$cmds = @(ninja -t commands wwe2k14.exe)
Pop-Location
$compile = $cmds | Where-Object { $_ -match '-c C:/Users/Jarrod/wwe2k14-recomp/src/main\.cpp$' } | Select-Object -First 1
if (-not $compile) { 'build_game_exe_side: no main.cpp compile command in the main build'; exit 3 }
# the compile command with its outputs moved: no dep file, object in -Out
$compile = $compile -replace ' -MD -MT \S+ -MF \S+', '' -replace '-o CMakeFiles/wwe2k14\.dir/src/main\.cpp\.obj', "-o `"$Out\main.obj`""
# the link: the object list is the build statement's explicit inputs (relative to the main build folder)
$line = $ninja | Where-Object { $_ -like 'build wwe2k14.exe: CXX_EXECUTABLE_LINKER__wwe2k14_Release *' } | Select-Object -First 1
$inputs = ($line -replace '^build wwe2k14\.exe: CXX_EXECUTABLE_LINKER__wwe2k14_Release ', '') -split ' \| ' | Select-Object -First 1
$outFwd = $Out.Replace([string][char]92, '/'); $bFwd = $B.Replace([string][char]92, '/')
$objs = $inputs -split ' ' | Where-Object { $_ } | ForEach-Object { if ($_ -like '*src/main.cpp.obj') { "`"$outFwd/main.obj`"" } else { "`"$bFwd/$_`"" } }  # (forward slashes: a response file takes a backslash as an escape)
$libsLine = $ninja | Where-Object { $_ -like '  LINK_LIBRARIES = *' } | Select-Object -First 1
$libs = $libsLine -replace '^  LINK_LIBRARIES = ', ''
$rsp = "$Out\wwe2k14.rsp"
Set-Content -LiteralPath $rsp -Value ((@($objs) -join ' ') + ' ' + $libs)
$link = "C:\PROGRA~1\LLVM\bin\CLANG_~1.EXE -nostartfiles -nostdlib -O3 -DNDEBUG -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -Xlinker /subsystem:windows -Xlinker /MAP:`"$Out\wwe2k14.map`" -fuse-ld=lld-link @`"$rsp`" -o `"$Out\wwe2k14.exe`" -Xlinker /MANIFEST:EMBED -Xlinker /implib:`"$Out\wwe2k14.lib`" -Xlinker /pdb:`"$Out\wwe2k14.pdb`" -Xlinker /version:0.0"
$lock = Enter-RunLock -Name "build_game_exe_side $Out" -TimeoutMinutes $LockWaitMinutes -Mode Build
try {
  $t0 = Get-Date
  Push-Location $B
  try {
    cmd /c "$compile 2>&1"
    if ($LASTEXITCODE -ne 0) { "build_game_exe_side: main.cpp failed ($LASTEXITCODE)"; exit 5 }
    "main.cpp compiled in $([int]((Get-Date) - $t0).TotalSeconds) s"
    cmd /c "$link 2>&1"
    if ($LASTEXITCODE -ne 0) { "build_game_exe_side: link failed ($LASTEXITCODE)"; exit 6 }
  } finally { Pop-Location }
} finally { Exit-RunLock $lock }
"built $Out\wwe2k14.exe in $([int]((Get-Date) - $t0).TotalSeconds) s"
Get-Item "$Out\wwe2k14.exe" | ForEach-Object { "{0} bytes {1}" -f $_.Length, $_.LastWriteTime }
