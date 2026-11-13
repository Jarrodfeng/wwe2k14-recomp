# Builds and runs the music player's offline proof (lane 288): tools\audio\music_player_selftest.cpp. No game, no speakers.
#   pwsh -File tools\audio\music_player_selftest.ps1 [-SdkWt C:\...\thirdparty\sdk-wt\lane288]
# The SDK worktree must carry patch 115 (include\rex\perf\audio_inject.h); its hook code is compiled into the test.
# Output: out\music_player\music_player_selftest.exe + the synthetic WAVs it makes (all under out\).
param([string]$SdkWt = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\lane288', [string]$Pck = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root\tools\env.ps1" | Out-Null
$out = "$root\out\music_player"
[void][System.IO.Directory]::CreateDirectory($out)
if (-not (Test-Path "$SdkWt\include\rex\perf\audio_inject.h")) { "no patch 115 in $SdkWt"; exit 2 }
# A stand-alone test compile into its own out folder (not the game build, no shared outputs, no run lock).
Push-Location $out
& clang-cl /nologo /std:c++20 /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations "/I$SdkWt\include" `
    "$root\tools\audio\music_player_selftest.cpp" "$SdkWt\src\core\perf\audio_inject.cpp" "$SdkWt\src\core\perf\audio_tap.cpp" `
    /Fe:music_player_selftest.exe 2>&1 | Out-File "$out\build.log"
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { Get-Content "$out\build.log" | Select-String 'error' | Select-Object -First 20; exit $code }
# lane 298: the arena-effects cases read the stock chain from the player's own sound bank when it is there (else the built-in defaults)
if (-not $Pck) {
    foreach ($c in @("$root\game_data\extracted\sound\Game_Sound_E_S.pck", 'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted\sound\Game_Sound_E_S.pck')) { if (Test-Path $c) { $Pck = $c; break } }
}
& "$out\music_player_selftest.exe" "$out\wav" "$SdkWt\src\audio\sdl\sdl_audio_driver.cpp" $Pck
exit $LASTEXITCODE
