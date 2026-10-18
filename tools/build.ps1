# Incremental build of the game exe (+ codegen when the manifest/config changed), then stage the SDK DLLs.
#   pwsh -File tools\build.ps1 -Run NNN          log: logs\build_NNN.log
# Session 8: holds the run lock (tools\runlock.ps1) for the whole build, since relinking the exe and restaging
# DLLs must not happen while a game run has them loaded. -Preset builds another preset's folder.
#
# Session 8 (task 2), SDK pinning: every rex*.dll the build would stage (the SDK bin folder CMake was configured with
# and $env:REXSDK\bin) and every rex*.dll already staged next to the exe must be listed in tools\sdk_pins.json
# (name + SHA256 -> SDK commit + patch file). Otherwise the build is refused before anything is built or copied
# (exit 3) and the reason is printed: file, hash, what is pinned, how to pin or override. The DLLs staged together
# (and those in each would-stage folder) must also share one pin entry: MIXED pins are refused the same way.
#   -AllowUnpinnedSdk "reason"   build anyway; the reason is mandatory, printed and appended to logs\sdk_pin_overrides.log
#   -CheckOnly                   verify the pins only: no build, no lock, no env.ps1. Exit 0 = pass, 3 = unpinned,
#                                2 = error (no pin list, nothing to check, bad folder). For regress / bisect / rebase.
#   -ExeDir DIR                  check another staged folder instead of out\build\<Preset> (testing)
#   -SdkBin DIR[,DIR]            check another "would stage" folder instead of the SDK bin (testing; , or ; separated)
#   (-CheckOnly with -AllowUnpinnedSdk reports "WOULD BE OVERRIDDEN" and writes nothing to the override log.)
#   -PinFile FILE                use another pin list (testing)
# Pin a new SDK build: pwsh -File tools\build_sdk.ps1 -Run NAME -Pin "what this build is".
param([string]$Run = '002', [string]$Preset = 'win-amd64-release', [string]$AllowUnpinnedSdk = '',
      [switch]$CheckOnly, [string]$ExeDir = '', [string[]]$SdkBin = @(), [string]$PinFile = '')
$repo = 'C:\Users\Jarrod\wwe2k14-recomp'
. "$repo\tools\sdk_pins.ps1"
$overrideGiven = $PSBoundParameters.ContainsKey('AllowUnpinnedSdk')
if ($overrideGiven -and -not $AllowUnpinnedSdk.Trim()) {
    'build.ps1: -AllowUnpinnedSdk needs a reason, e.g. -AllowUnpinnedSdk "testing SDK branch s8/foo (not pinned yet)"'
    exit 2
}
if (-not $ExeDir) { $ExeDir = Join-Path $repo "out\build\$Preset" }
if (-not $PinFile) { $PinFile = Join-Path $repo 'tools\sdk_pins.json' }

# Checks the pins and prints a short report; sets $script:pinOk (build may go ahead) and $script:pinError.
function Invoke-SdkPinCheck([string]$Context) {
    $script:pinOk = $false; $script:pinError = $false
    try { $doc = Get-SdkPinList $PinFile } catch { "sdk pin check: ERROR $($_.Exception.Message)"; $script:pinError = $true; return }
    # -SdkBin via pwsh -File arrives as one string: split on , and ; and drop trailing backslashes (review nits).
    $stageDirs = @(if ($SdkBin.Count) { $SdkBin | ForEach-Object { $_ -split '[,;]' } | Where-Object { $_.Trim() } | ForEach-Object { [System.IO.Path]::GetFullPath($_.Trim()).TrimEnd('\') } } else { Get-SdkStageDirs $Preset })
    if ($stageDirs.Count -eq 0) { "sdk pin check: ERROR no SDK bin folder found (CMakeCache rexglue_DIR / REXSDK)"; $script:pinError = $true; return }
    foreach ($d in $stageDirs) {
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { "sdk pin check: ERROR folder not found: $d"; $script:pinError = $true; return }
    }
    # A preset folder that was never built has nothing staged yet (fine for a build; -CheckOnly can't verify it).
    $exeDirExists = Test-Path -LiteralPath $ExeDir -PathType Container
    if (-not $exeDirExists -and $CheckOnly) { "sdk pin check: ERROR folder not found: $ExeDir"; $script:pinError = $true; return }
    $stage = @(foreach ($d in $stageDirs) { Get-ChildItem -LiteralPath $d -Filter 'rex*.dll' -File | Sort-Object Name | ForEach-Object FullName })
    $staged = @(if ($exeDirExists) { Get-ChildItem -LiteralPath $ExeDir -Filter 'rex*.dll' -File | Sort-Object Name | ForEach-Object FullName })
    if ($stage.Count + $staged.Count -eq 0) { "sdk pin check: ERROR no rex*.dll in $ExeDir or $($stageDirs -join ', ')"; $script:pinError = $true; return }
    $stagedRes = @(Test-SdkPinFiles -Paths $staged -Doc $doc)
    $stageRes = @(Test-SdkPinFiles -Paths $stage -Doc $doc)
    "sdk pin check ($Context): staged in $ExeDir -> $(if ($staged.Count) { Format-SdkPinSummary $stagedRes $doc } else { 'none staged' })"
    foreach ($d in $stageDirs) {
        $sub = @($stageRes | Where-Object { (Split-Path $_.Path -Parent) -ieq $d })
        "sdk pin check ($Context): would stage from $d -> $(if ($sub.Count) { Format-SdkPinSummary $sub $doc } else { 'no rex*.dll' })"
        $tool = Join-Path $d 'rexglue.exe'
        if (Test-Path -LiteralPath $tool) {
            $t = Test-SdkPinFiles -Paths @($tool) -Doc $doc
            "sdk pin check ($Context): codegen tool $tool -> $(if ($t.Pinned) { 'pinned ' + ($t.PinIds -join '/') } else { 'UNPINNED sha256 ' + $t.Sha256.Substring(0, 12) + ' (reported only, not enforced)' })"
        }
    }
    $bad = @(@($stagedRes) + @($stageRes) | Where-Object { -not $_.Pinned })
    # Mixed pins (review finding 2): the DLLs of one set (staged, or one would-stage folder) must share a pin entry.
    $sets = @(, $stagedRes) + @(foreach ($d in $stageDirs) { , @($stageRes | Where-Object { (Split-Path $_.Path -Parent) -ieq $d }) })
    $mixed = @(foreach ($set in $sets) {
        $set = @($set | Where-Object { $_.Pinned })
        if ($set.Count -lt 2) { continue }
        $common = @($set[0].PinIds); foreach ($r in $set) { $common = @($common | Where-Object { @($r.PinIds) -contains $_ }) }
        if ($common.Count -eq 0) { $set }
    })
    if ($bad.Count -eq 0 -and $mixed.Count -eq 0) { "sdk pin check ($Context): PASS"; $script:pinOk = $true; return }
    if ($mixed.Count) { "sdk pin check ($Context): MIXED pins (DLLs from different SDK builds): " + (($mixed | ForEach-Object { '{0} -> {1}' -f $_.Path, ($_.PinIds -join '/') }) -join '; ') }
    if ($overrideGiven -and $CheckOnly) {
        # Review nit: -CheckOnly builds nothing, so it reports the override without writing to the override log.
        "sdk pin check ($Context): WOULD BE OVERRIDDEN by -AllowUnpinnedSdk: $($AllowUnpinnedSdk.Trim()) (check only, not logged)"
        foreach ($b in $bad) { "  unpinned: $($b.Name) sha256 $($b.Sha256) ($($b.Path))" }
        $script:pinOk = $true
        return
    }
    if ($overrideGiven) {
        $who = "$env:USERNAME pid $PID"
        $entry = '{0}  build.ps1 -Run {1} -Preset {2}{3}  by {4}  reason: {5}  unpinned: {6}' -f (Get-Date).ToString('s'), $Run, $Preset,
            $(if ($CheckOnly) { ' -CheckOnly' } else { '' }), $who, $AllowUnpinnedSdk.Trim(),
            ((@($bad | ForEach-Object { '{0} sha256 {1} ({2})' -f $_.Name, $_.Sha256, $_.Path }) + @($mixed | ForEach-Object { 'MIXED {0} -> {1} ({2})' -f $_.Name, ($_.PinIds -join '/'), $_.Path })) -join '; ')
        $logDir = Join-Path $repo 'logs'
        $null = New-Item -ItemType Directory -Force -Path $logDir
        Add-Content -LiteralPath (Join-Path $logDir 'sdk_pin_overrides.log') -Value $entry
        "sdk pin check ($Context): OVERRIDDEN by -AllowUnpinnedSdk: $($AllowUnpinnedSdk.Trim())"
        foreach ($b in $bad) { "  unpinned: $($b.Name) sha256 $($b.Sha256) ($($b.Path))" }
        "  logged to logs\sdk_pin_overrides.log"
        $script:pinOk = $true
        return
    }
    if ($mixed.Count) { "REFUSED: rex*.dll from different pin entries ($Context); stage DLLs from one SDK build, or pass -AllowUnpinnedSdk `"reason`"" }
    Write-SdkPinRefusal -Results $bad -Context $Context -Doc $doc -PinFile $PinFile
}

if ($CheckOnly) {
    Invoke-SdkPinCheck 'check only'
    if ($script:pinError) { exit 2 }
    if ($script:pinOk) { exit 0 } else { exit 3 }
}

. "$repo\tools\env.ps1" *> $null
. "$repo\tools\runlock.ps1"
$ErrorActionPreference = 'Continue'
Set-Location $repo
$lock = Enter-RunLock -Name "build.ps1 $Preset $Run"
$buildExit = 0
try {
# Checked inside the lock: no other build / SDK build / restage can change these files until we release it.
Invoke-SdkPinCheck 'before build'
if (-not $script:pinOk) {
    $buildExit = if ($script:pinError) { 2 } else { 3 }
    "build.ps1: REFUSED, nothing built or copied (exit $buildExit)"
} else {
$log = "logs\build_$Run.log"
$t0 = Get-Date
cmake --build "out/build/$Preset" -j 16 *>&1 | Out-File $log
$buildExit = $LASTEXITCODE
"build exit: $buildExit  elapsed: $([int]((Get-Date) - $t0).TotalSeconds)s"

# The SDK DLLs are only staged when the exe relinks, so an SDK-only rebuild would leave stale copies.
$outDir = "out\build\$Preset"
foreach ($dll in Get-ChildItem "$env:REXSDK\bin\rex*.dll" -ErrorAction SilentlyContinue) {
    $staged = Join-Path $outDir $dll.Name
    if (-not (Test-Path $staged) -or (Get-FileHash $staged).Hash -ne (Get-FileHash $dll.FullName).Hash) {
        Copy-Item $dll.FullName $staged -Force
        "staged updated SDK DLL: $($dll.Name)"
    }
}
Get-Content $log | Select-String 'error|warning:|FAILED|Linking' | Select-Object -First 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) }
Get-SdkPinLine -ExeDir (Join-Path $repo $outDir) -PinFile $PinFile
}
} finally { Exit-RunLock $lock }
exit $buildExit
