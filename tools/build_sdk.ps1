# Build + install our patched ReXGlue SDK (thirdparty\rexglue-sdk-src, branch wwe2k14-local) and stage the DLLs and
# PDBs next to the game. Log: logs\sdk_build_<Run>.log.
#   pwsh -File tools\build_sdk.ps1 -Run s6a               incremental build + install
#   pwsh -File tools\build_sdk.ps1 -Run s6a -Reconfigure  re-run CMake with our Release flags first (full rebuild)
# Release flags (Session 6): -g -gcodeview + /DEBUG so every SDK build is profileable (no separate symbol build to
# keep in sync); /OPT:REF /OPT:ICF restore the link optimisations /DEBUG would otherwise turn off. Debug info does not
# change clang's generated code.
# Session 8 (task 2): -Pin "note" adds the freshly staged DLL hashes (+ the installed rexglue.exe) to tools\sdk_pins.json
# with the SDK commit and the matching patch file, after a successful build + stage; tools\build.ps1 refuses unpinned
# DLLs. -PinPatch names the patch file yourself (default: detected, see Find-SdkPatchMatch in tools\sdk_pins.ps1);
# -PinId names the entry (default pin-<commit7>-<MMdd-HHmm>). Commit the SDK change first so the pin names a real commit.
param([string]$Run = 's6', [switch]$Reconfigure, [string]$ExtraCxxFlags = '', [string]$Pin = '', [string]$PinPatch = '',
      [string]$PinId = '', [string]$PinAllowDirty = '')
# -PinAllowDirty "reason": pin even though the SDK tree has uncommitted changes (refused otherwise; the entry is marked).
if ($PSBoundParameters.ContainsKey('Pin') -and -not $Pin.Trim()) { 'build_sdk.ps1: -Pin needs a note, e.g. -Pin "s8 rebase onto upstream v0.11"'; exit 2 }
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\env.ps1' *> $null
$ErrorActionPreference = 'Continue'
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\runlock.ps1'
$sdk = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src'
$log = "C:\Users\Jarrod\wwe2k14-recomp\logs\sdk_build_$Run.log"
# Session 8: the whole build + install + staging holds the run lock (no SDK build or DLL restage during a game run).
$lock = Enter-RunLock -Name "build_sdk.ps1 $Run"
try {
Set-Location $sdk
$t0 = Get-Date
if ($Reconfigure) {
    $cxx = "-O3 -DNDEBUG -g -gcodeview $ExtraCxxFlags".Trim()
    cmake -S . -B out/build/win-amd64 "-DCMAKE_CXX_FLAGS_RELEASE=$cxx" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g -gcodeview" `
        "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" `
        "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF" *>&1 | Out-File $log
}
cmake --build out/build/win-amd64 --config Release --target install -j 16 *>&1 | Out-File $log -Append
$code = $LASTEXITCODE
"sdk build exit: $code  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s  log: $log"
Get-Content $log | Select-String ': error|FAILED|error:' | Select-Object -First 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
if ($code -ne 0) { exit $code }

# Stage DLLs + PDBs next to the game (tools\build.ps1 also stages DLLs, but only from the install dir and without PDBs).
$exeDir = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release'
foreach ($name in 'rexgpu-xenos', 'rexruntime') {
    foreach ($ext in 'dll', 'pdb') {
        $src = Join-Path "$sdk\out\win-amd64\Release" "$name.$ext"
        $dst = Join-Path $exeDir "$name.$ext"
        if ((Test-Path $src) -and (-not (Test-Path $dst) -or (Get-FileHash $dst).Hash -ne (Get-FileHash $src).Hash)) {
            Copy-Item $src $dst -Force
            "staged: $name.$ext"
        }
    }
}
. 'C:\Users\Jarrod\wwe2k14-recomp\tools\sdk_pins.ps1'
if ($Pin) {
    # The staged DLLs and the installed copies build.ps1 would stage (normally identical; Add-SdkPin refuses if not).
    $pinFiles = @(Get-ChildItem "$exeDir\rex*.dll" -File | ForEach-Object FullName) + @(Get-ChildItem "$env:REXSDK\bin\rex*.dll" -File | ForEach-Object FullName)
    if (Test-Path "$env:REXSDK\bin\rexglue.exe") { $pinFiles += "$env:REXSDK\bin\rexglue.exe" }
    try {
        Add-SdkPin -Note $Pin.Trim() -Files $pinFiles -SdkDir $sdk -Id $PinId -Patch $PinPatch -AllowDirty $PinAllowDirty -Built "build_sdk.ps1 -Run $Run, $((Get-Date).ToString('s')) (log $log)"
    } catch { "sdk pin: NOT added: $($_.Exception.Message)"; $code = 4 }
}
Get-SdkPinLine -ExeDir $exeDir
} finally { Exit-RunLock $lock }
if ($code -ne 0) { exit $code }
