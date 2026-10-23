# Session 53: a release-configured build of the working tree for local tests (never published).
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$R\tools\env.ps1"
$build = "$R\out\s53\relbuild"
$sdkCmake = (Select-String -Path "$R\out\build\win-amd64-release\CMakeCache.txt" -Pattern '^rexglue_DIR:PATH=(.*)$').Matches[0].Groups[1].Value
Set-Location $R
if (-not (Test-Path "$build\build.ninja")) {
  cmake --preset win-amd64-release -B $build -DWWE2K14_RELEASE=ON "-DWWE2K14_VERSION_SUFFIX=-s53test" "-Drexglue_DIR=$sdkCmake" *>&1 | Out-File "$R\logs\s53_relbuild.log"
}
cmake --build $build -j 16 *>&1 | Out-File "$R\logs\s53_relbuild.log" -Append
"exit $LASTEXITCODE"
foreach ($dll in Get-ChildItem "$env:REXSDK\bin\rex*.dll") { Copy-Item $dll.FullName (Join-Path $build $dll.Name) -Force }
