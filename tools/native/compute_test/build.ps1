# Builds run_compute.exe (the standalone Vulkan compute runner of the offline pass tests) into out\native\compute_test\.
# Needs the Vulkan SDK (VULKAN_SDK, headers + vulkan-1.lib; glslc for compute_test.py) and the repo's clang (tools\env.ps1).
# No lock, no game: ~5 s. Rebuilds only when the source is newer than the exe.
#   pwsh -NoProfile -File tools\native\compute_test\build.ps1
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$vkSdk = $env:VULKAN_SDK
if (-not $vkSdk -or -not (Test-Path "$vkSdk\Include\vulkan\vulkan.h")) {
    $vkSdk = @(Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName)[0]
}
if (-not $vkSdk -or -not (Test-Path "$vkSdk\Include\vulkan\vulkan.h")) { 'compute_test: no Vulkan SDK found (VULKAN_SDK / C:\VulkanSDK)'; exit 2 }
$out = "$R\out\native\compute_test"
$null = New-Item -ItemType Directory -Force -Path $out
$src = "$R\tools\native\compute_test\run_compute.cpp"
$exe = "$out\run_compute.exe"
if ((Test-Path $exe) -and (Get-Item $exe).LastWriteTime -gt (Get-Item $src).LastWriteTime) { "run_compute.exe is current: $exe"; exit 0 }
. "$R\tools\env.ps1" *> $null
clang++ -std=c++17 -O2 -Wall "-I$vkSdk\Include" $src "$vkSdk\Lib\vulkan-1.lib" -o $exe 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE) { "compute_test: build failed ($LASTEXITCODE)"; exit 3 }
"built $exe"
exit 0
