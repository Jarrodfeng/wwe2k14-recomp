# Session 72: compile our post-effect replacement shaders (src\post\*.frag, our own code) to SPIR-V headers
# (src\post\spirv\<name>.h, a uint32_t array named after the file), as the SDK keeps its own built-in shaders.
# Run after editing a shader; the headers are committed, so the game build needs no shader compiler.
#   pwsh -File tools\post\build_shaders.ps1 [-Name match_combine_enhanced]
param([string]$Name = '')
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$glslang = Get-ChildItem 'C:\VulkanSDK\*\Bin\glslangValidator.exe' -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $glslang) { $glslang = Get-Command glslangValidator -ErrorAction SilentlyContinue }
if (-not $glslang) { throw 'glslangValidator not found (Vulkan SDK)' }
$src = Join-Path $R 'src\post'
$out = Join-Path $src 'spirv'
New-Item -ItemType Directory -Force $out | Out-Null
$files = Get-ChildItem $src -Filter '*.frag' | Where-Object { -not $Name -or $_.BaseName -eq $Name }
foreach ($f in $files) {
    $spv = Join-Path $env:TEMP "$($f.BaseName).spv"
    & $glslang.FullName -V --target-env vulkan1.0 -S frag -o $spv $f.FullName
    if ($LASTEXITCODE -ne 0) { throw "glslangValidator failed on $($f.Name)" }
    $bytes = [IO.File]::ReadAllBytes($spv)
    $words = for ($i = 0; $i -lt $bytes.Length; $i += 4) { '0x{0:X8}' -f [BitConverter]::ToUInt32($bytes, $i) }
    $lines = for ($i = 0; $i -lt $words.Count; $i += 8) { '    ' + (($words[$i..([Math]::Min($i + 7, $words.Count - 1))]) -join ', ') + ',' }
    $header = @(
        '#pragma once'
        "// Session 72: generated from ../$($f.Name) by tools\post\build_shaders.ps1"
        "// (glslangValidator -V --target-env vulkan1.0 -S frag). Do not edit: edit the .frag and run the script."
        '#include <cstdint>'
        ''
        "inline constexpr uint32_t $($f.BaseName)_spv[] = {"
    ) + $lines + @('};', '')
    $path = Join-Path $out "$($f.BaseName).h"
    Set-Content -Path $path -Value ($header -join "`r`n") -NoNewline
    Remove-Item $spv
    "{0}: {1} words -> {2}" -f $f.Name, $words.Count, $path
}
