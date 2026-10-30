# Session 73 (gpu_native_ps_relaxed_precision): tests the SDK's RelaxedPrecision SPIR-V pass without the game.
# Builds fp16_pass_test.exe (clang++, the pass compiled standalone), compiles the test shaders in tests\ and our post
# shaders (src\post\*.frag) with glslangValidator, runs the pass over them and over any dump folders given
# (--dump_shaders output: *.vk_native.bin.frag / *.vk_fsi.bin.frag / ...; local only, never committed), then runs
# spirv-val on every input and every changed output. Output in out\fp16\.
#   pwsh -File tools\android\fp16\run.ps1
#   pwsh -File tools\android\fp16\run.ps1 -Dumps out\s72\dump
# Ends with "PASS" when every changed module validates (an input that already fails validation is reported and not
# counted against the pass).
param([string[]]$Dumps = @(), [string]$TargetEnv = 'vulkan1.1')
$ErrorActionPreference = 'Stop'
$Dumps = @($Dumps | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # (pwsh -File passes "a,b" as one string)
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$sdk = "$root\thirdparty\rexglue-sdk-src"
$out = "$root\out\fp16"
$vk = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
$glslang = if ($vk) { "$($vk.FullName)\Bin\glslangValidator.exe" } else { "$root\out\glslang-build\StandAlone\glslangValidator.exe" }
$spirvVal = if ($vk) { "$($vk.FullName)\Bin\spirv-val.exe" } else { throw 'spirv-val not found (Vulkan SDK)' }
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
New-Item -ItemType Directory -Force "$out\in", "$out\relaxed" | Out-Null
Get-ChildItem "$out\relaxed" -File | Remove-Item -Force -Confirm:$false

# 1. The test tool with the pass built in (no cvar / logging: REX_SPIRV_RELAXED_PRECISION_STANDALONE).
$exe = "$out\fp16_pass_test.exe"
& $clang -std=c++20 -O2 -DREX_SPIRV_RELAXED_PRECISION_STANDALONE "-I$sdk" "-I$sdk\thirdparty\glslang" `
  "$PSScriptRoot\fp16_pass_test.cpp" "$sdk\src\graphics\pipeline\shader\spirv_relaxed_precision.cpp" -o $exe
if ($LASTEXITCODE) { throw 'clang++ failed' }

# 2. Shaders: the tests and our post shaders.
$testNames = @(Get-ChildItem "$PSScriptRoot\tests" -File).Name
$sources = @(Get-ChildItem "$PSScriptRoot\tests" -File) + @(Get-ChildItem "$root\src\post" -Filter '*.frag' -File)
foreach ($s in $sources) {
  & $glslang -V --target-env $TargetEnv -o "$out\in\$($s.Name).spv" $s.FullName | Out-Null
  if ($LASTEXITCODE) { throw "glslangValidator failed on $($s.Name)" }
}

# 3. The pass (verbose for the hand-written tests).
& $exe "$out\relaxed" --verbose (Get-ChildItem "$out\in" -Filter '*.spv' | Where-Object { $testNames -contains $_.BaseName } | ForEach-Object FullName)
& $exe "$out\relaxed" (Get-ChildItem "$out\in" -Filter '*.spv' | Where-Object { $testNames -notcontains $_.BaseName } | ForEach-Object FullName)
foreach ($d in $Dumps) {
  $lines = & $exe "$out\relaxed" (Resolve-Path $d).Path
  $lines | Select-String '^TOTAL' | ForEach-Object { "$d : $_" }
  $lines | Set-Content "$out\dump_$((Split-Path $d -Leaf)).txt"
}

# 4. spirv-val on every changed module, and on its input.
$inputs = @{}
Get-ChildItem "$out\in" -Filter '*.spv' | ForEach-Object { $inputs[$_.Name] = $_.FullName }
foreach ($d in $Dumps) { Get-ChildItem (Resolve-Path $d).Path -File | ForEach-Object { $inputs[$_.Name] = $_.FullName } }
$ok = 0; $bad = 0; $inputBad = 0
foreach ($r in Get-ChildItem "$out\relaxed" -Filter '*.relaxed.spv') {
  $name = $r.Name -replace '\.relaxed\.spv$', ''
  $msg = & $spirvVal --target-env vulkan1.3 --scalar-block-layout $r.FullName 2>&1
  if ($LASTEXITCODE -eq 0) { $ok++; continue }
  & $spirvVal --target-env vulkan1.3 --scalar-block-layout $inputs[$name] 2>&1 | Out-Null
  if ($LASTEXITCODE) { $inputBad++; Write-Host "input already invalid: $name" } else { $bad++; Write-Host "INVALID after the pass: $name`n$msg" }
}
Write-Host "spirv-val: $ok changed modules valid, $bad invalid after the pass, $inputBad with an invalid input"
if ($bad -eq 0 -and $ok -gt 0) { Write-Host 'PASS' } else { Write-Host 'FAIL'; exit 1 }
