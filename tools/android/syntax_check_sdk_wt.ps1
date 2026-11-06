# Session 93 (builder s93-window): Android compile check of SDK files changed in a worktree, without building the Android SDK: the
# NDK clang -fsyntax-only with the Android SDK build's own flags (ninja -t commands of the object in out\android\sdk-build-arm64),
# the include roots pointed at the worktree, the precompiled header dropped. Prints only "error" lines; none = it compiles.
#   pwsh -File tools\android\syntax_check_sdk_wt.ps1      (edit $wt and $files below, or pass them as the two variables)
# The Android SDK build must exist (tools\android\build_sdk_android.ps1 ran once). Writes nothing.
. C:\Users\Jarrod\wwe2k14-recomp\tools\android\env.ps1 *> $null
. C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1 *> $null
$b = $AndroidDirs.SdkBuild
Set-Location $b
$wt = 'C:/Users/Jarrod/wwe2k14-recomp/thirdparty/sdk-wt/s93-window'
$main = 'C:/Users/Jarrod/wwe2k14-recomp/thirdparty/rexglue-sdk-src'
$nin = Get-Content build.ninja
$files = 'src/graphics/vulkan/command_processor.cpp','src/graphics/vulkan/pipeline_cache.cpp','src/graphics/pipeline/shader/spirv_translator.cpp','src/graphics/primitive_processor.cpp'
foreach ($f in $files) {
  $line = $nin | Where-Object { $_ -match ('^build [^:]+: CXX_COMPILER__rexgpu-xenos_unscanned_Release C\$:/Users/Jarrod/wwe2k14-recomp/thirdparty/rexglue-sdk-src/' + [regex]::Escape($f) + ' ') } | Select-Object -First 1
  if (-not $line) { "no build statement for $f"; continue }
  $obj = ($line -replace '^build ([^:]+):.*', '$1') -replace '\$:', ':'
  $cmd = (ninja -t commands $obj | Select-Object -Last 1)
  $cmd = $cmd -replace '(?<=\s)-o\s+\S+', '' -replace '(?<=\s)-MD\s+-MT\s+\S+\s+-MF\s+\S+', '' -replace '(?<=\s)-c\s+\S+\s*$', ''
  $cmd = $cmd -replace '-Xclang -include-pch -Xclang \S+', ''
  $cmd = $cmd.Replace("-I$main/include", "-I$wt/include").Replace("-I$main ", "-I$wt ")
  $cmd = "$cmd -fsyntax-only $wt/$f"
  "== $f"
  cmd /c "$cmd 2>&1" | Select-String 'error' | Select-Object -First 12
  "exit(last matched) $LASTEXITCODE"
}
