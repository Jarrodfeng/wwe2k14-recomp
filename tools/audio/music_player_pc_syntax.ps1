# Syntax-only compile of the game's main translation unit from THIS worktree's sources (lane 288): proves that
# src\entrance_music.h / music_preview.h / music_player*.h compile against the SDK headers WITH patch 115. No link, no
# output files, no game run, no run lock.
#   pwsh -File tools\audio\music_player_pc_syntax.ps1 [-SdkWt C:\...\thirdparty\sdk-wt\lane288] [-Android]
# -Android: compiles the player headers alone (src\music_player.h / music_player_app.h's platform-free part) for aarch64-linux-android
# with the NDK clang (tools\android\env.ps1) -fsyntax-only.
param([string]$SdkWt = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\sdk-wt\lane288', [switch]$Android)
$ErrorActionPreference = 'Stop'
$wt = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main = 'C:\Users\Jarrod\wwe2k14-recomp'
if ($Android) {
    . "$main\tools\android\env.ps1" | Out-Null
    $clang = Get-ChildItem "$Llvm\aarch64-linux-android*-clang++.cmd" | Sort-Object Name | Select-Object -Last 1
    if (-not $clang) { 'no NDK clang found'; exit 2 }
    $tu = "$wt\out\music_player_android_tu.cpp"
    Set-Content $tu "#include `"music_player.h`"`n#include `"music_player_mf.h`"`n#include `"music_arena_fx.h`"`nint main() { wwe2k14::music::MusicPlayer p; wwe2k14::music::ArenaFx fx; fx.Prepare(wwe::musicchain::defaults(), false); p.SetChain(fx.AsChain()); return 0; }"
    & $clang.FullName -std=c++20 -fsyntax-only -Wall "-I$wt\src" $tu
    exit $LASTEXITCODE
}
. "$main\tools\env.ps1" | Out-Null
& clang++ -fsyntax-only -w -DNOMINMAX -DRDR_HAS_STREAMLINE=1 '-DREXGLUE_BUILD_CONFIG="Release"' -DREX_HAS_D3D12=1 -DREX_HAS_VULKAN=1 `
    -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DWIN32_LEAN_AND_MEAN '-DWWE2K14_VERSION="syntax"' `
    "-I$wt" "-I$wt\src" "-I$main\thirdparty" "-I$main" "-I$main\generated\default" "-I$main\thirdparty\streamline\sdk\include" `
    -isystem "$SdkWt\include" -isystem "$main\thirdparty\rexglue-sdk-vulkan\include" -isystem "$main\thirdparty\rexglue-sdk-vulkan\include\dxc" `
    -O0 "-std=gnu++23" -D_DLL -D_MT "-msse4.1" "$wt\src\main.cpp" 2>&1 | Select-Object -First 60
exit $LASTEXITCODE
