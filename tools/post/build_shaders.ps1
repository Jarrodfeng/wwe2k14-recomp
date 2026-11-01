# Session 72: compile our post-effect replacement shaders (src\post\*.frag, our own code) to SPIR-V headers
# (src\post\spirv\<name>.h, a uint32_t array named after the file), as the SDK keeps its own built-in shaders.
# Session 80: and our scene materials (src\scene\*.frag / *.vert -> src\scene\spirv\, SCENE_SHADERS_PLAN.md).
# Run after editing a shader; the headers are committed, so the game build needs no shader compiler.
#   pwsh -File tools\post\build_shaders.ps1 [-Name match_combine_enhanced]
param([string]$Name = '')
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$glslang = Get-ChildItem 'C:\VulkanSDK\*\Bin\glslangValidator.exe' -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $glslang) { $glslang = Get-Command glslangValidator -ErrorAction SilentlyContinue }
if (-not $glslang) { throw 'glslangValidator not found (Vulkan SDK)' }
foreach ($dir in @(@{ Path = 'src\post'; Session = 'Session 72' }, @{ Path = 'src\scene'; Session = 'Session 80' })) {
  $src = Join-Path $R $dir.Path
  if (-not (Test-Path $src)) { continue }
  $out = Join-Path $src 'spirv'
  New-Item -ItemType Directory -Force $out | Out-Null
  $files = Get-ChildItem $src -File | Where-Object { $_.Extension -in '.frag', '.vert' -and (-not $Name -or $_.BaseName -eq $Name) }
  foreach ($f in $files) {
    $stage = $f.Extension.TrimStart('.')
    $id = if ($stage -eq 'vert') { "$($f.BaseName)_vert" } else { $f.BaseName }  # (a .vert and a .frag may share a name)
    $spv = Join-Path $env:TEMP "$id.spv"
    & $glslang.FullName -V --target-env vulkan1.0 -S $stage -o $spv $f.FullName
    if ($LASTEXITCODE -ne 0) { throw "glslangValidator failed on $($f.Name)" }
    $bytes = [IO.File]::ReadAllBytes($spv)
    $words = for ($i = 0; $i -lt $bytes.Length; $i += 4) { '0x{0:X8}' -f [BitConverter]::ToUInt32($bytes, $i) }
    $lines = for ($i = 0; $i -lt $words.Count; $i += 8) { '    ' + (($words[$i..([Math]::Min($i + 7, $words.Count - 1))]) -join ', ') + ',' }
    $header = @(
        '#pragma once'
        "// $($dir.Session): generated from ../$($f.Name) by tools\post\build_shaders.ps1"
        "// (glslangValidator -V --target-env vulkan1.0 -S $stage). Do not edit: edit the .$stage and run the script."
        '#include <cstdint>'
        ''
        "inline constexpr uint32_t ${id}_spv[] = {"
    ) + $lines + @('};', '')
    $path = Join-Path $out "$id.h"
    Set-Content -Path $path -Value ($header -join "`r`n") -NoNewline
    Remove-Item $spv
    "{0}: {1} words -> {2}" -f $f.Name, $words.Count, $path
  }
}
