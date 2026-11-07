# A game exe from a worktree's OWN src\, without a build folder of its own and without codegen (Session 95).
#   & C:\Users\Jarrod\wwe2k14-recomp\out\wt\<name>\tools\owned\fast_game_exe.ps1 -Worktree <abs worktree> [-DllSource DIR]
# Answers: "I changed our game code (src\*.h) in a worktree; give me an exe to run, in ~2 min, and leave the main build alone."
# tools\bisect\wt_build.ps1 builds a whole worktree (codegen and every generated file, a 30 GB folder); fast_build.ps1 builds the
# SDK DLLs only. Our game code is ONE translation unit (src\main.cpp), so this compiles that file from the worktree with the main
# build's exact defines, flags and precompiled header (read from its build.ninja) and links it with the main build's objects into
# <worktree>\out\exe\ (exe, map, the rex DLLs of -DllSource, the launcher and the other files a run needs next to the exe).
# Nothing in out\build\win-amd64-release is written. Holds a BUILD slot of the run lock while it reads the objects.
# Limits: the worktree's generated declarations must be the main build's (no change to config\*.toml / the manifest, no NEW hooked
# function: REX_HOOK_RAW on a function the main build does not hook needs a real build); the launcher is the main build's, so a
# change to src\launcher or src\diag_plan.h is NOT in it.
# -SdkWorktree DIR -NewSdkHeaders 'rex/perf/x.h,...' (Session 95): for game code that uses something NEW from an SDK worktree (fast_build first). The named
# headers (new ones only: a changed header would clash with the precompiled header's copy) go on the include path, and the exe links that worktree's rexruntime.lib.
# Output ends with "FAST_GAME_EXE <path>". Exit: 0 ok, 2 bad arguments, 3 compile failed, 4 link failed, 1 lock wait over -LockWaitMinutes (or any error before the script ran inside the lock).
# -SdkInclude DIR: an SDK worktree's include folder, searched before the main build's (a change to an SDK header the game uses;
# give the same worktree's DLLs with -DllSource; no precompiled header then).
param([Parameter(Mandatory)][string]$Worktree, [string]$DllSource = '', [string]$SdkInclude = '', [switch]$Locked, [double]$LockWaitMinutes = 15, [string]$SdkWorktree = '', [string]$NewSdkHeaders = '')
$ErrorActionPreference = 'Continue'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$Worktree = [System.IO.Path]::GetFullPath($Worktree).TrimEnd('\')
if ($Worktree -ieq $R) { 'fast_game_exe: the main tree has tools\build.ps1'; exit 2 }
if (-not (Test-Path -LiteralPath "$Worktree\src\main.cpp")) { "fast_game_exe: no src\main.cpp in $Worktree"; exit 2 }
if (-not $Locked) {
    # the BUILD slot, then this script again inside it
    $self = $MyInvocation.MyCommand.Path
    $name = 'gameexe_' + (Split-Path $Worktree -Leaf)
    $lockArgs = @('-NoProfile', '-File', "$R\tools\runlock.ps1", 'run', '-Build', '-Name', $name, '-TimeoutMinutes', $LockWaitMinutes, '--', 'pwsh', '-NoProfile', '-File', $self, '-Worktree', $Worktree, '-Locked')
    if ($DllSource) { $lockArgs += @('-DllSource', $DllSource) }
    if ($SdkInclude) { $lockArgs += @('-SdkInclude', $SdkInclude) }
    if ($SdkWorktree) { $lockArgs += @('-SdkWorktree', $SdkWorktree, '-NewSdkHeaders', $NewSdkHeaders) }
    & pwsh @lockArgs
    exit $LASTEXITCODE
}
$B = "$R\out\build\win-amd64-release"
if (-not $DllSource) { $DllSource = $B }
. "$R\tools\env.ps1" *> $null
$ErrorActionPreference = 'Continue'
$Out = "$Worktree\out\exe"
$null = New-Item -ItemType Directory -Force -Path "$Out\obj"

# The two build statements, as ninja would run them.
$ninja = [System.IO.File]::ReadAllLines("$B\build.ninja")
function Get-Statement([string]$prefix) {
    $i = 0
    while ($i -lt $ninja.Count -and -not $ninja[$i].StartsWith($prefix)) { $i++ }
    if ($i -ge $ninja.Count) { throw "no '$prefix' in build.ninja" }
    $vars = @{ '_line' = $ninja[$i] }
    for ($j = $i + 1; $j -lt $ninja.Count -and $ninja[$j].StartsWith('  '); $j++) {
        $eq = $ninja[$j].IndexOf(' = ')
        if ($eq -gt 0) { $vars[$ninja[$j].Substring(0, $eq).Trim()] = $ninja[$j].Substring($eq + 3) }
    }
    $vars
}
$cc = Get-Statement 'build CMakeFiles/wwe2k14.dir/src/main.cpp.obj:'
$ln = Get-Statement 'build wwe2k14.exe:'
$clang = 'C:\PROGRA~1\LLVM\bin\CLANG_~1.EXE'
$wtFwd = $Worktree -replace '\\', '/'
$rFwd = $R -replace '\\', '/'
# our headers from the worktree first; generated\ and the SDK stay the main tree's. The main -I...src stays behind them:
# src\native_zlib.h reaches stb through "../thirdparty/...", which only resolves from the main tree (no thirdparty\ in a worktree).
$includes = "-I$wtFwd -I$wtFwd/src " + $(if ($SdkInclude) { "-I$($SdkInclude -replace '\\', '/') " } else { '' }) + $cc['INCLUDES']
$runtimeLib = ''
if ($SdkWorktree) {
    $stage = "$Out\obj\sdkinc"
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($h in ($NewSdkHeaders -split ',' | Where-Object { $_ })) {
        $null = New-Item -ItemType Directory -Force -Path (Split-Path "$stage\$h")
        Copy-Item -LiteralPath "$SdkWorktree\include\$h" -Destination "$stage\$h" -Force
    }
    $includes = "-I$($stage -replace '\\', '/') " + $includes
    $runtimeLib = (Get-ChildItem "$SdkWorktree\out\win-amd64" -Recurse -Filter 'rexruntime.lib' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    if (-not $runtimeLib) { "fast_game_exe: no rexruntime.lib under $SdkWorktree\out\win-amd64"; exit 2 }
    "linking with $runtimeLib"
}
$obj = "$Out\obj\main.cpp.obj"
$t0 = Get-Date
$rsp = "$Out\obj\compile.rsp"
# (a response file: quotes in -DWWE2K14_VERSION stay as ninja's shell would pass them)
# (-SdkInclude: the precompiled header holds the main build's SDK headers, so the file compiles without it; about a minute more)
$flags = $cc['FLAGS']
if ($SdkInclude) { $flags = $flags -replace '-Xclang -include-pch -Xclang \S+', '' }
[System.IO.File]::WriteAllText($rsp, (($cc['DEFINES'] + ' ' + $includes + ' ' + $flags + " -o `"$($obj -replace '\\','/')`" -c `"$wtFwd/src/main.cpp`"") -replace '\\"', '\"'))
"compile main.cpp from $Worktree"
& $clang "@$rsp" 2>&1 | Select-Object -Last 30
if ($LASTEXITCODE) { "fast_game_exe: compile failed ($LASTEXITCODE)"; exit 3 }
"compiled in {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds

# Link: the main build's inputs with our main.cpp.obj in the place of its own.
$inputs = ($ln['_line'] -split ':\s*', 2)[1]
$inputs = ($inputs -split '\s+\|\s*')[0]
$items = @($inputs -split '\s+' | Where-Object { $_ -and $_ -ne 'CXX_EXECUTABLE_LINKER__wwe2k14_Release' })
$objs = foreach ($it in $items) {
    if ($it -eq 'CMakeFiles/wwe2k14.dir/src/main.cpp.obj') { $obj -replace '\\', '/' }
    elseif ($it -match '^[A-Za-z]\$:') { $it -replace '\$:', ':' }
    else { "$($B -replace '\\','/')/$it" }
}
$linkRsp = "$Out\obj\link.rsp"
$linkLibs = $ln['LINK_LIBRARIES']
if ($runtimeLib) { $linkLibs = $linkLibs -replace '\S*/rexruntime\.lib', ($runtimeLib -replace '\\', '/') }
[System.IO.File]::WriteAllText($linkRsp, (($objs -join ' ') + ' ' + $ln['LINK_PATH'] + ' ' + $linkLibs))
$t1 = Get-Date
$flags = @($ln['FLAGS'] -split '\s+' | Where-Object { $_ })
$linkFlags = @(($ln['LINK_FLAGS'] -replace [regex]::Escape("/MAP:$($B -replace '\\','/')/wwe2k14.map"), "/MAP:$($Out -replace '\\','/')/wwe2k14.map") -split '\s+' | Where-Object { $_ })
& $clang -nostartfiles -nostdlib @flags @linkFlags "@$linkRsp" -o "$Out\wwe2k14.exe" -Xlinker /MANIFEST:EMBED -Xlinker "/implib:$Out\obj\wwe2k14.lib" -Xlinker "/pdb:$Out\obj\wwe2k14.pdb" -Xlinker /version:0.0 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE) { "fast_game_exe: link failed ($LASTEXITCODE)"; exit 4 }
"linked in {0:N0} s" -f ((Get-Date) - $t1).TotalSeconds

foreach ($f in (Get-ChildItem "$DllSource\rex*.dll", "$DllSource\rex*.pdb" -File)) { Copy-Item -LiteralPath $f.FullName -Destination $Out -Force }
foreach ($n in 'amd_fidelityfx_vk.dll', 'wwe2k14_launcher.exe', 'shader_list.txt') {
    if (Test-Path -LiteralPath "$B\$n") { Copy-Item -LiteralPath "$B\$n" -Destination $Out -Force }
}
"FAST_GAME_EXE $Out\wwe2k14.exe"
