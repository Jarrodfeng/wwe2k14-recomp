# Session 95, Lane C: builds tools\shaders\vi_translate.cpp against the objects of a fast_build worktree and translates a --dump_shaders folder's vertex shaders, plain and with kNativeExtVsInput. 0 game runs.
#   & tools\shaders\vi_translate.ps1 -Worktree C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\<name> -Dump <dump folder> -Out <folder under out\> [-List db\shaders\shader_list.txt]
# Then: py tools\shaders\vi_translate.py <Out> (spirv-val and the "no storage variable" bar). The DLL exports none of the translator, so the tool links the DLL's own objects and libraries, read from the build's
# ninja files. The worktree must have been built by fast_build (its junctions are gone by then, so the headers of thirdparty come from the main SDK checkout, whose submodule pins fast_build checked).
param([Parameter(Mandatory)][string]$Worktree, [Parameter(Mandatory)][string]$Dump, [Parameter(Mandatory)][string]$Out, [string]$List = '', [double]$LockWaitMinutes = 8)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\env.ps1" *> $null
. "$R\tools\runlock.ps1"
$Tool = Join-Path $PSScriptRoot 'vi_translate.cpp'
$ViBuild = "$Worktree\out\build\win-amd64"
$Bin = Join-Path $Out 'bin'
New-Item -ItemType Directory -Force $Bin | Out-Null
$wtF = $Worktree.Replace('\', '/')
$mainF = "$R\thirdparty\rexglue-sdk-src".Replace('\', '/')

$lock = Enter-RunLock -Name 'vi_translate' -Mode Build -TimeoutMinutes $LockWaitMinutes
try {
    Push-Location $ViBuild
    try {
        # The compile flags of a translator object, without the cache launcher, the precompiled header or the object's own names.
        $c = (& ninja -f build-Release.ninja -t commands src/graphics/CMakeFiles/rexgpu-xenos.dir/Release/pipeline/shader/translator.cpp.obj | Select-Object -Last 1)
        $c = $c -replace '^\S*sccache\S*\s+', ''
        $c = $c -replace '-Xclang -include-pch -Xclang \S+', '' -replace '-Xclang -include -Xclang \S+', ''
        $c = $c -replace '(?<=\s)-MD\s+-MT\s+\S+\s+-MF\s+\S+', '' -replace '(?<=\s)-o\s+\S+', '' -replace '(?<=\s)-c\s+\S+\s*$', ''
        $c = $c.Replace("$wtF/thirdparty", "$mainF/thirdparty")
        $obj = "$Bin\vi_translate.obj"
        $rsp = "$Bin\compile.rsp"
        ($c -replace '^\S+\s+', '').Replace('\"', '\\"') + " -c `"$($Tool.Replace('\', '/'))`" -o `"$($obj.Replace('\', '/'))`"" | Set-Content -Encoding ascii $rsp
        & clang++ "@$rsp"
        if ($LASTEXITCODE) { throw "compile failed $LASTEXITCODE" }

        # The DLL's link, as an exe: its objects, this object, its libraries.
        $impl = Get-Content 'CMakeFiles\impl-Release.ninja'
        $at = ($impl | Select-String -Pattern '^build .*rexgpu-xenos\.dll' | Select-Object -First 1).LineNumber
        $libs = ($impl[$at..($at + 12)] | Where-Object { $_ -match '^\s+LINK_LIBRARIES = ' }) -replace '^\s+LINK_LIBRARIES = ', ''
        $objs = Get-ChildItem 'src\graphics\CMakeFiles\rexgpu-xenos.dir\Release' -Recurse -Filter *.obj | ForEach-Object { '"' + $_.FullName.Replace('\', '/') + '"' }
        $exe = "$Bin\vi_translate.exe"
        $lrsp = "$Bin\link.rsp"
        (@("`"$($obj.Replace('\', '/'))`"") + $objs + @($libs, '-fuse-ld=lld-link', '-Wl,/DEBUG:NONE', "-o `"$($exe.Replace('\', '/'))`"")) -join "`n" | Set-Content -Encoding ascii $lrsp
        & clang++ "@$lrsp"
        if ($LASTEXITCODE) { throw "link failed $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { Exit-RunLock $lock }

Copy-Item "$Worktree\out\win-amd64\Release\rexruntime.dll" $Bin -Force
& $exe $Dump (Join-Path $Out 'spv') $List | Tee-Object (Join-Path $Out 'translate.txt') | Select-Object -Last 12
"vi_translate exit $LASTEXITCODE"
