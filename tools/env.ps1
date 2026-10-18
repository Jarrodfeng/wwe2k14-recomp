<#
.SYNOPSIS
    Build environment for wwe2k14-recomp.

.DESCRIPTION
    Sets up a shell that can build ReXGlue projects:
      1. Loads the Visual Studio 2022 developer environment (MSVC libs, Windows SDK, CMake, Ninja).
      2. Puts standalone LLVM (C:\Program Files\LLVM) AHEAD of VS 2022's bundled Clang.
         VS 2022 17.14 ships Clang 19.1.5; ReXGlue needs 20+. Without this step the
         VS dev shell picks Clang 19 (confirmed 2026-10-06, see PLAYBOOK.md).
      3. Points REXSDK at the ReXGlue SDK in thirdparty/rexglue-sdk (if present).
      4. Prints tool versions and fails loudly if Clang is older than 20.

    Only affects the current shell. Nothing is written to system or user settings.

.EXAMPLE
    Double-click tools\devshell.bat, or from PowerShell 7:
        . .\tools\env.ps1
#>

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$LlvmBin     = "C:\Program Files\LLVM\bin"
$VsInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$MinClang    = 20

# -- 1. Visual Studio 2022 developer environment ------------------------------
if (-not $env:VSCMD_VER) {
    # Pin to VS 2022 (17.x) rather than -latest, so a future VS install can't silently change the toolchain.
    $vs = & "$VsInstaller\vswhere.exe" -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath | Select-Object -First 1
    if (-not $vs) { throw "Visual Studio 2022 with 'Desktop development with C++' not found." }

    # VS's own scripts call vswhere by name; without this they print a harmless "not recognized" error.
    if ($env:PATH -notlike "*$VsInstaller*") { $env:PATH = "$VsInstaller;$env:PATH" }

    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}

# -- 2. Standalone LLVM ahead of VS's bundled Clang ----------------------------
if (-not (Test-Path "$LlvmBin\clang.exe")) { throw "LLVM not found at $LlvmBin. Install with: winget install --id LLVM.LLVM --exact" }
$env:PATH = "$LlvmBin;" + (($env:PATH -split ';' | Where-Object { $_ -and $_ -ne $LlvmBin }) -join ';')

# -- 3. ReXGlue SDK ------------------------------------------------------------
if (-not $env:REXSDK) {
    # Prefer the source-built SDK with Vulkan (thirdparty\rexglue-sdk-vulkan, see PLAYBOOK), else the prebuilt zip.
    foreach ($candidate in "rexglue-sdk-vulkan", "rexglue-sdk") {
        $sdk = Join-Path $ProjectRoot "thirdparty\$candidate"
        if (Test-Path "$sdk\lib\cmake\rexglue\rexglueConfig.cmake") { $env:REXSDK = $sdk; break }
    }
}
if ($env:REXSDK -and (Test-Path "$env:REXSDK\bin")) {
    if ($env:PATH -notlike "*$env:REXSDK\bin*") { $env:PATH = "$env:REXSDK\bin;$env:PATH" }
    # Projects made by `rexglue init` v0.10.0 call find_package(rexglue) and never read REXSDK,
    # so the SDK must also be on CMAKE_PREFIX_PATH (confirmed from generated/rexglue.cmake).
    if ("$env:CMAKE_PREFIX_PATH" -notlike "*$env:REXSDK*") {
        $env:CMAKE_PREFIX_PATH = (@($env:REXSDK) + @($env:CMAKE_PREFIX_PATH -split ';' | Where-Object { $_ })) -join ';'
    }
}

# -- 4. Report and sanity-check ------------------------------------------------
$clangLine = (& clang --version | Select-Object -First 1)
$clangMajor = if ($clangLine -match 'clang version (\d+)') { [int]$Matches[1] } else { 0 }

function Show-Tool([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { "  {0,-9} {1}" -f $Name, $cmd.Source } else { "  {0,-9} (not found)" -f $Name }
}

Write-Host ""
Write-Host "  wwe2k14-recomp build shell" -ForegroundColor Cyan
Write-Host "  Project: $ProjectRoot" -ForegroundColor Gray
Write-Host "  VS:      $env:VSCMD_VER" -ForegroundColor Gray
Write-Host "  Clang:   $clangLine" -ForegroundColor Gray
Write-Host "  CMake:   $((& cmake --version | Select-Object -First 1))" -ForegroundColor Gray
Write-Host "  Ninja:   $(& ninja --version)" -ForegroundColor Gray
Write-Host "  REXSDK:  $(if ($env:REXSDK) { $env:REXSDK } else { '(not set - SDK not installed yet)' })" -ForegroundColor Gray
Write-Host ""
Show-Tool clang; Show-Tool clang++; Show-Tool cmake; Show-Tool ninja; Show-Tool rexglue
Write-Host ""

if ($clangMajor -lt $MinClang) {
    throw "Clang $clangMajor is on PATH but ReXGlue needs $MinClang+. Check that $LlvmBin comes first in PATH."
}
