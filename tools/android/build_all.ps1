# The whole Android chain in one go: SDK (incremental) -> game library -> APK [-> install]. Stops at the first failure.
#   pwsh -File tools\android\build_all.ps1 -Run b01 [-Install]
# The game's precompiled headers are thrown away when the SDK install changed headers (CMake does not track the
# installed SDK's headers: a stale PCH fails with "has been modified since the precompiled header was built").
param([Parameter(Mandatory)][string]$Run, [switch]$Install, [string]$AllowBranch = '')  # Session 66: -AllowBranch X builds the SDK checkout on branch X too (compile checks of a session branch)
. "$PSScriptRoot\env.ps1"
$inc = "$($AndroidDirs.SdkInstall)\include"
$before = if (Test-Path $inc) { (Get-ChildItem $inc -Recurse -File | Measure-Object LastWriteTime -Maximum).Maximum } else { $null }
pwsh -NoProfile -File "$PSScriptRoot\build_sdk_android.ps1" -Run "$Run-sdk" -AllowBranch "$AllowBranch"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$after = (Get-ChildItem $inc -Recurse -File | Measure-Object LastWriteTime -Maximum).Maximum
if ($before -ne $after) {
    Get-ChildItem "$($AndroidDirs.Game)\CMakeFiles" -Recurse -Filter 'cmake_pch.hxx.pch' -ErrorAction SilentlyContinue | Remove-Item
    "SDK headers changed: the game's precompiled headers will be rebuilt"
}
pwsh -NoProfile -File "$PSScriptRoot\build_game_android.ps1" -Run "$Run-game"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$apkArgs = @()
if ($Install) { $apkArgs += '-Install' }
pwsh -NoProfile -File "$PSScriptRoot\build_apk.ps1" @apkArgs
exit $LASTEXITCODE
