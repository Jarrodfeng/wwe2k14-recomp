# Builds and runs tools\audio\chain_selftest.cpp (offline proof of src\audio\music_chain*.h). 0 game runs, no audio device.
# Usage: pwsh -File tools\audio\build_selftest.ps1 [-Pck PATH] [-NoRun]
param([string]$Pck = "", [switch]$NoRun)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root\tools\env.ps1" | Out-Null
$out = "$root\out\audio"
New-Item -ItemType Directory -Force $out | Out-Null
& clang++ -std=c++20 -O2 -Wall -Wextra "-I$root\src\audio" "$root\tools\audio\chain_selftest.cpp" -o "$out\chain_selftest.exe"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
if ($NoRun) { return }
if (-not $Pck) {
    foreach ($c in @("$root\game_data\extracted\sound\Game_Sound_E_S.pck", "C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted\sound\Game_Sound_E_S.pck")) { if (Test-Path $c) { $Pck = $c; break } }
}
if ($Pck) { & "$out\chain_selftest.exe" --pck $Pck } else { & "$out\chain_selftest.exe" }
exit $LASTEXITCODE
