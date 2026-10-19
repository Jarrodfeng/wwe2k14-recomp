# Session 33 (FSR 3): build AMD's FidelityFX runtime (amd_fidelityfx_vk.dll: ffx-api + FSR 3 upscaler, Vulkan backend)
# from the FidelityFX SDK 1.1.4 source in thirdparty\FidelityFX-SDK (AMD's official GitHub, sparse checkout of
# ffx-api\ and sdk\; MIT). No Vulkan SDK on this PC: an import library for the system vulkan-1.dll is made first.
# Run under the run lock: pwsh -NoProfile -File tools\runlock.ps1 run -Name ffx_build -- pwsh -NoProfile -File tools\fsr\build_ffx.ps1
# The game's CMake copies the DLL next to wwe2k14.exe; make_release.ps1 ships it.
$ErrorActionPreference = 'Stop'
. C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1
$ffx = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\FidelityFX-SDK'
$vk = "$ffx\vk_import"
New-Item -ItemType Directory -Force $vk | Out-Null
if (-not (Test-Path "$vk\vulkan-1.lib")) {
  $exports = & dumpbin /exports C:\Windows\System32\vulkan-1.dll | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(vk\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
  "LIBRARY vulkan-1`nEXPORTS`n" + ($exports -join "`n") | Set-Content "$vk\vulkan-1.def" -Encoding ascii
  & lib /nologo /def:"$vk\vulkan-1.def" /machine:x64 /out:"$vk\vulkan-1.lib"
  "exports: $($exports.Count)"
}
$inc = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src\thirdparty\vulkan-headers\include'
Set-Location "$ffx\ffx-api"
& cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DFFX_API_BACKEND=VK_X64 "-DVulkan_INCLUDE_DIR=$inc" "-DVulkan_LIBRARY=$vk\vulkan-1.lib"
if ($LASTEXITCODE) { throw "configure failed" }
& cmake --build build --config Release --parallel 16 -- /p:CL_MPcount=16 /v:minimal
if ($LASTEXITCODE) { throw "build failed" }
Get-ChildItem -Recurse "$ffx\ffx-api" -Filter 'amd_fidelityfx_vk*' | Select-Object FullName, Length
