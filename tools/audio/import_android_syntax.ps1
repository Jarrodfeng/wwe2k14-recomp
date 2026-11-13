# Syntax check of the song importer for Android (lane 283): the NDK's clang on audio_import.cpp (MediaCodec decoder path included).
# No link, no run. Also checks the header-only parts compile for arm64.
#   pwsh -File tools\audio\import_android_syntax.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root\tools\android\env.ps1" | Out-Null
if (-not $Llvm) { 'no NDK found (tools\android\env.ps1)'; exit 2 }
& "$Llvm\clang++.exe" --target=aarch64-linux-android$MinApi -fsyntax-only -std=c++20 -Wall -Wno-unused-function `
    "$root\src\audio_import\audio_import.cpp"
& "$Llvm\clang++.exe" --target=aarch64-linux-android$MinApi -fsyntax-only -std=c++20 -Wall "$root\src\audio_import\lookup.cpp"
exit $LASTEXITCODE
