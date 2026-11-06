# Session 72: compile our post-effect replacement shaders (src\post\*.frag, our own code) to SPIR-V headers
# (src\post\spirv\<name>.h, a uint32_t array named after the file), as the SDK keeps its own built-in shaders.
# Session 80: and our scene materials (src\scene\*.frag / *.vert -> src\scene\spirv\, SCENE_SHADERS_PLAN.md).
# Run after editing a shader; the headers are committed, so the game build needs no shader compiler.
#   pwsh -File tools\post\build_shaders.ps1 [-Name match_combine_enhanced]
# Session 93 (idea 8): the shaders listed in $MediumpShaders get a second compile with -DMEDIUMP (hand-placed mediump colour maths,
# out\s938_mediump_notes.md) into <id>_mp.h (array <id>_mp_spv); the base header is compiled exactly as before (byte-identical).
param([string]$Name = '')
$MediumpShaders = @('arena_bg_bump_lm', 'arena_bg_shadow_lm', 'arena_bg_shadow_spec_mask_lm', 'arena_bg_spec_mask_lm',
  'arena_prop_bump', 'arena_prop_bump_reflect', 'arena_prop_bump_rope', 'arena_prop_bump_rope2', 'arena_prop_spec_mask_lm',
  'match_combine_original', 'entrance_combine_original')
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
    # Session 93: one compile per shader (Session 92's split variants _s2 / _s4 are gone): xenos_vs.glsl reads the shared memory
    # through the per-draw window base, which is 0 whenever the SDK's window mode is off.
    $variants = @(@{ Suffix = ''; Defs = @() })
    $body = @()
    foreach ($variant in $variants) {
      $spv = Join-Path $env:TEMP "$id$($variant.Suffix).spv"
      & $glslang.FullName -V --target-env vulkan1.0 @($variant.Defs) -S $stage -o $spv $f.FullName
      if ($LASTEXITCODE -ne 0) { throw "glslangValidator failed on $($f.Name) $($variant.Defs)" }
      $bytes = [IO.File]::ReadAllBytes($spv)
      $words = for ($i = 0; $i -lt $bytes.Length; $i += 4) { '0x{0:X8}' -f [BitConverter]::ToUInt32($bytes, $i) }
      $lines = for ($i = 0; $i -lt $words.Count; $i += 8) { '    ' + (($words[$i..([Math]::Min($i + 7, $words.Count - 1))]) -join ', ') + ',' }
      $body += @("inline constexpr uint32_t ${id}_spv$($variant.Suffix)[] = {") + $lines + @('};', '')
      Remove-Item $spv
    }
    $header = @(
        '#pragma once'
        "// $($dir.Session): generated from ../$($f.Name) by tools\post\build_shaders.ps1"
        "// (glslangValidator -V --target-env vulkan1.0 -S $stage). Do not edit: edit the .$stage and run the script."
        '#include <cstdint>'
        ''
    ) + $body
    $path = Join-Path $out "$id.h"
    Set-Content -Path $path -Value ($header -join "`r`n") -NoNewline
    "{0}: {1} variant(s) -> {2}" -f $f.Name, $variants.Count, $path
    if ($stage -eq 'frag' -and $MediumpShaders -contains $f.BaseName) {
      $spv = Join-Path $env:TEMP "${id}_mp.spv"
      & $glslang.FullName -V --target-env vulkan1.0 -DMEDIUMP -S $stage -o $spv $f.FullName
      if ($LASTEXITCODE -ne 0) { throw "glslangValidator failed on $($f.Name) -DMEDIUMP" }
      $bytes = [IO.File]::ReadAllBytes($spv)
      $words = for ($i = 0; $i -lt $bytes.Length; $i += 4) { '0x{0:X8}' -f [BitConverter]::ToUInt32($bytes, $i) }
      $lines = for ($i = 0; $i -lt $words.Count; $i += 8) { '    ' + (($words[$i..([Math]::Min($i + 7, $words.Count - 1))]) -join ', ') + ',' }
      $mpHeader = @(
          '#pragma once'
          "// Session 93: generated from ../$($f.Name) by tools\postuild_shaders.ps1 with -DMEDIUMP (hand-placed mediump colour maths,"
          "// out\s938_mediump_notes.md; selected by --wwe2k14_shader_mediump). Do not edit: edit the .$stage and run the script."
          '#include <cstdint>'
          ''
          "inline constexpr uint32_t ${id}_mp_spv[] = {"
      ) + $lines + @('};', '')
      $mpPath = Join-Path $out "${id}_mp.h"
      Set-Content -Path $mpPath -Value ($mpHeader -join "`r`n") -NoNewline
      Remove-Item $spv
      "{0}: mediump variant -> {1}" -f $f.Name, $mpPath
    }
  }
}
