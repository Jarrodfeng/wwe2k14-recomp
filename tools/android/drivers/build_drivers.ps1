# Session 72 part 7 (Jarrod: bundled drivers built from source): builds the custom Vulkan drivers the APK ships, in
# Linux containers (Docker Desktop), from source only:
#   turnip  Adreno 6xx / 7xx / 8xx: thirdparty\mesa (Mesa's official repo, tag $MesaTag)          -> libvulkan_freedreno.so
#           + mesa_patches\*.patch applied in the container (Session 73: upstream's Adreno 710 / 720 / 722 entries)
#   panvk   Mali Bifrost / Valhall on stock Android (kbase): thirdparty\mali-panvk (JimVulkan, MIT,
#           commit $PanvkCommit; Session 72 review: no network, no other programs, files only /proc/self
#           and /dev/mali0; its profiler writes only when switched on by env / debug property)  -> libvulkan_panfrost.so
# Output: out\android\drivers\*.so (build_apk.ps1 packs them next to the game's libraries).
#   pwsh -File tools\android\drivers\build_drivers.ps1 [-Only turnip|panvk] [-Rebuild]   (-Rebuild: the images too)
param([ValidateSet('', 'turnip', 'panvk')][string]$Only = '', [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$MesaTag = 'mesa-26.2.5'
$PanvkCommit = 'd896b0a'
$out = Join-Path $R 'out\android\drivers'
New-Item -ItemType Directory -Force $out | Out-Null

function Image([string]$name, [string]$dockerfile) {
  docker image inspect $name *> $null
  if ($Rebuild -or $LASTEXITCODE) {
    docker build -t $name -f (Join-Path $PSScriptRoot $dockerfile) $PSScriptRoot
    if ($LASTEXITCODE) { throw "docker build $name failed" }
  }
}
function Run([string]$image, [string]$src, [string]$script) {
  $t0 = Get-Date
  docker run --rm -v "${src}:/src:ro" -v "${out}:/out" -v "${PSScriptRoot}:/scripts:ro" $image sh "/scripts/$script"
  if ($LASTEXITCODE) { throw "$script failed" }
  "$script`: $([int]((Get-Date) - $t0).TotalSeconds) s"
}

if ($Only -in '', 'turnip') {
  $mesa = Join-Path $R 'thirdparty\mesa'
  if (-not (Test-Path "$mesa\.git")) { git clone -q --depth 1 --branch $MesaTag https://gitlab.freedesktop.org/mesa/mesa.git $mesa }
  $have = (git -C $mesa describe --tags --exact-match 2>$null)
  if ($have -ne $MesaTag) { throw "thirdparty\mesa is at '$have', expected $MesaTag" }
  Image 'wwe2k14-android-drivers:1' 'Dockerfile'
  Run 'wwe2k14-android-drivers:1' $mesa 'build_turnip.sh'
}
if ($Only -in '', 'panvk') {
  $panvk = Join-Path $R 'thirdparty\mali-panvk'
  if (-not (Test-Path "$panvk\.git")) { git clone -q https://github.com/JimVulkan/mali-panvk.git $panvk }
  $have = (git -C $panvk rev-parse --short=7 HEAD)
  if ($have -ne $PanvkCommit) { throw "thirdparty\mali-panvk is at $have, expected $PanvkCommit (the reviewed commit)" }
  Image 'wwe2k14-android-panvk:1' 'Dockerfile.panvk'
  Run 'wwe2k14-android-panvk:1' $panvk 'build_panvk.sh'
}
Get-ChildItem $out -Filter 'libvulkan_*.so' | ForEach-Object { "{0}: {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }
