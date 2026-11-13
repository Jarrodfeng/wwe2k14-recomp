# Session 100: builds ucode_disasm.cpp against the objects of a built SDK worktree (same approach as tools\shaders\vi_translate.ps1). 0 game runs.
#   & tools\shaders\packets\build_disasm.ps1 [-Worktree C:\...\thirdparty\sdk-wt\s99]   -> out\s100\disasm\ucode_disasm.exe
param([string]$Worktree = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\s99', [double]$LockWaitMinutes = 8)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\env.ps1" *> $null
. "$R\tools\runlock.ps1"
$Tool = Join-Path $PSScriptRoot 'ucode_disasm.cpp'
$ViBuild = "$Worktree\out\build\win-amd64"
$Bin = "$R\out\s100\disasm"
New-Item -ItemType Directory -Force $Bin | Out-Null
$wtF = $Worktree.Replace('\', '/')
$mainF = "$R\thirdparty\rexglue-sdk-src".Replace('\', '/')
$lock = Enter-RunLock -Name 'ucode_disasm' -Mode Build -TimeoutMinutes $LockWaitMinutes
try {
    Push-Location $ViBuild
    try {
        $c = (& ninja -f build-Release.ninja -t commands src/graphics/CMakeFiles/rexgpu-xenos.dir/Release/pipeline/shader/translator.cpp.obj | Select-Object -Last 1)
        $c = $c -replace '^\S*sccache\S*\s+', ''
        $c = $c -replace '-Xclang -include-pch -Xclang \S+', '' -replace '-Xclang -include -Xclang \S+', ''
        $c = $c -replace '(?<=\s)-MD\s+-MT\s+\S+\s+-MF\s+\S+', '' -replace '(?<=\s)-o\s+\S+', '' -replace '(?<=\s)-c\s+\S+\s*$', ''
        $c = $c.Replace("$wtF/thirdparty", "$mainF/thirdparty")
        $obj = "$Bin\ucode_disasm.obj"
        $rsp = "$Bin\compile.rsp"
        ($c -replace '^\S+\s+', '').Replace('\"', '\\"') + " -c `"$($Tool.Replace('\', '/'))`" -o `"$($obj.Replace('\', '/'))`"" | Set-Content -Encoding ascii $rsp
        & clang++ "@$rsp"
        if ($LASTEXITCODE) { throw "compile failed $LASTEXITCODE" }
        $impl = Get-Content 'CMakeFiles\impl-Release.ninja'
        $at = ($impl | Select-String -Pattern '^build .*rexgpu-xenos\.dll' | Select-Object -First 1).LineNumber
        $libs = ($impl[$at..($at + 12)] | Where-Object { $_ -match '^\s+LINK_LIBRARIES = ' }) -replace '^\s+LINK_LIBRARIES = ', ''
        $objs = Get-ChildItem 'src\graphics\CMakeFiles\rexgpu-xenos.dir\Release' -Recurse -Filter *.obj | ForEach-Object { '"' + $_.FullName.Replace('\', '/') + '"' }
        $exe = "$Bin\ucode_disasm.exe"
        $lrsp = "$Bin\link.rsp"
        (@("`"$($obj.Replace('\', '/'))`"") + $objs + @($libs, '-fuse-ld=lld-link', '-Wl,/DEBUG:NONE', "-o `"$($exe.Replace('\', '/'))`"")) -join "`n" | Set-Content -Encoding ascii $lrsp
        & clang++ "@$lrsp"
        if ($LASTEXITCODE) { throw "link failed $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { Exit-RunLock $lock }
Copy-Item "$Worktree\out\win-amd64\Release\rexruntime.dll" $Bin -Force
"built $exe"
