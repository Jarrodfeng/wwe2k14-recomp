# Lane 292: builds and runs the offscreen picture tool (tools\music\picker_shots.cpp) and the input host test
# (tools\music\picker_view_test.cpp), then turns the PPMs into PNGs. No game, no window, no GPU.
#   pwsh -NoProfile -File tools\music\build_picker_shots.ps1 [-GameRoot C:\...\game_data\extracted] [-Out out\l292] [-TestOnly]
# Needs Dear ImGui's sources (the SDK checkout's thirdparty\imgui) and stb_image.h: -Thirdparty defaults to the main checkout's.
param(
  [string]$GameRoot = "",
  [string]$Out = "out\l292",
  [string]$Thirdparty = "C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src\thirdparty",
  [switch]$TestOnly
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$clang = "C:\Program Files\LLVM\bin\clang++.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$imgui = "$Thirdparty\imgui"
& $clang -std=c++20 -O1 -Wall -Wno-unused-function "-I$root\src" "-I$Thirdparty\stb" tools\music\picker_view_test.cpp src\music_picker_assets.cpp -o "$Out\picker_view_test.exe"
if ($LASTEXITCODE) { throw "test build failed" }
& "$Out\picker_view_test.exe"
if ($LASTEXITCODE) { throw "picker_view_test FAILED" }
if ($TestOnly) { return }
$srcs = @("$imgui\imgui.cpp", "$imgui\imgui_draw.cpp", "$imgui\imgui_tables.cpp", "$imgui\imgui_widgets.cpp")
& $clang -std=c++20 -O2 -Wall -Wno-unused-function "-I$root\src" "-I$imgui" "-I$Thirdparty\stb" tools\music\picker_shots.cpp src\music_picker_assets.cpp @srcs -o "$Out\picker_shots.exe"
if ($LASTEXITCODE) { throw "picker_shots build failed" }
if ($GameRoot) { & "$Out\picker_shots.exe" $Out $GameRoot } else { Write-Host "(no -GameRoot: the game-font pictures are skipped)" }
& "$Out\picker_shots.exe" $Out
$py = @"
import glob, os
from PIL import Image
for p in glob.glob(r'$Out/*.ppm'):
    Image.open(p).save(p[:-4] + '.png', optimize=True)
    os.remove(p)
"@
$py | py -
Remove-Item "$Out\*.exe" -ErrorAction SilentlyContinue
