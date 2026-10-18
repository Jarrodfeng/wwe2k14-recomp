# Configures the coverage build preset (Session 8 task 6). Run under the run lock:
#   pwsh -NoProfile -File tools\runlock.ps1 run -Name cov-configure -- pwsh -NoProfile -File tools\coverage\configure.ps1
# then build with:  pwsh -NoProfile -File tools\build.ps1 -Preset win-amd64-coverage -Run cov_NNN
# The first build compiles every generated file again with the instrumented prologue (a few minutes, CPU heavy).
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$repo\tools\env.ps1" *> $null
Set-Location $repo
cmake --preset win-amd64-coverage
exit $LASTEXITCODE
