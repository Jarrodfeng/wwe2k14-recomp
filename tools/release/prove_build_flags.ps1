# Session 102 lanes W1/W2: prove that the release switches reach the compiler, with CONFIGURE ONLY (no compile, no game, no run).
#   pwsh -NoProfile -File tools\release\prove_build_flags.ps1 [-Profdata out\pgo\guest_match.profdata]
# Configures scratch build folders from THIS checkout under out\prove_flags\ (a junction generated\default to the main checkout's is made for the
# duration and removed with rmdir, which does not follow it), under the BUILD slot of the run lock. The argument lists are the release's own
# (tools\release\release_configure_args.ps1, the same function make_release.ps1 uses):
#   base   : release args, no profile        -> guest and ours without -mavx2 ... and without a profile
#   wide   : v3 + profile + PGO_OURS         -> both TUs with every v3 flag and -fprofile-instr-use=<profile>
#   narr   : profile without PGO_OURS        -> guest with the profile, ours without
#   sticky : configured WIDE with a profile, then RECONFIGURED in the same folder with the base release args -> back to no v3, no profile (review B:
#            an incremental release folder must not keep the last configure's cache values)
# Planted twins (each must FAIL its check): base checked against the wide expectations; the sticky folder reconfigured with the explicit empty values
# left out (Get-ReleaseConfigureArgs -OmitExplicit, the first version) must still hold the profile and v3 flags.
# Exit 0 = all verdicts as required.
param([string]$Profdata = '')
$ErrorActionPreference = 'Stop'
$Here = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Main = 'C:\Users\Jarrod\wwe2k14-recomp'
. (Join-Path $PSScriptRoot 'release_configure_args.ps1')
if (-not $Profdata) { $Profdata = Join-Path $Main 'out\pgo\guest_match.profdata' }
if (-not (Test-Path -LiteralPath $Profdata)) { 'prove_build_flags: FAIL: profile not found: ' + $Profdata; exit 1 }
$prof = ConvertTo-CMakePath $Profdata
. "$Main\tools\env.ps1" *> $null
$sdkCmake = (Select-String -Path "$Main\out\build\win-amd64-release\CMakeCache.txt" -Pattern '^rexglue_DIR:PATH=(.*)$').Matches[0].Groups[1].Value
$scratch = Join-Path $Here 'out\prove_flags'
$gen = Join-Path $Here 'generated\default'
$madeJunction = $false
if (-not (Test-Path -LiteralPath $gen)) { New-Item -ItemType Junction -Path $gen -Target (Join-Path $Main 'generated\default') | Out-Null; $madeJunction = $true }
. "$Main\tools\runlock.ps1"
$lock = Enter-RunLock -Name 'prove_build_flags' -Mode Build
$rc = 0
try {
    function Conf([string]$name, [string[]]$more, [switch]$Keep) {
        $b = Join-Path $scratch $name
        if (-not $Keep -and (Test-Path $b)) { Remove-Item -Recurse -Force $b }
        $out = cmake -S $Here --preset win-amd64-release -B $b -DWWE2K14_RELEASE=ON "-Drexglue_DIR=$sdkCmake" @more 2>&1
        if ($LASTEXITCODE) { $out | Select-Object -Last 15; throw "configure $name failed" }
        return $b
    }
    $base = Conf 'base' (Get-ReleaseConfigureArgs)
    $wide = Conf 'wide' (Get-ReleaseConfigureArgs -Wide -Profdata $Profdata)
    $narr = Conf 'narr' (Get-ReleaseConfigureArgs -Profdata $Profdata -Narrow)
    $sticky = Conf 'sticky' (Get-ReleaseConfigureArgs -Wide -Profdata $Profdata)
    $chk = Join-Path $PSScriptRoot 'check_build_flags.py'
    "--- base (must pass: no v3, no profile)"; py -I $chk $base --guest base --ours base;                                 if ($LASTEXITCODE) { $rc = 1 }
    "--- wide (must pass: v3 + profile in guest AND ours)"; py -I $chk $wide --guest v3 --ours v3 --profdata $prof --pgo-ours yes; if ($LASTEXITCODE) { $rc = 1 }
    "--- narrow profile (must pass: profile in guest only)"; py -I $chk $narr --guest base --ours base --profdata $prof;   if ($LASTEXITCODE) { $rc = 1 }
    "--- PLANTED: the base folder against the wide expectations (must FAIL)"
    py -I $chk $base --guest v3 --ours v3 --profdata $prof --pgo-ours yes
    if ($LASTEXITCODE -eq 0) { 'prove_build_flags: the planted mismatch was NOT caught'; $rc = 1 }
    # review B: reconfigure the wide folder with the release's base args
    $null = Conf 'sticky' (Get-ReleaseConfigureArgs) -Keep
    "--- sticky: the wide folder reconfigured with the base release args (must pass: no v3, no profile)"
    py -I $chk $sticky --guest base --ours base; if ($LASTEXITCODE) { $rc = 1 }
    # planted: the same reconfigure without the explicit empty values -> the old values stick, so the same check must FAIL
    $null = Conf 'sticky2' (Get-ReleaseConfigureArgs -Wide -Profdata $Profdata)
    $null = Conf 'sticky2' (Get-ReleaseConfigureArgs -OmitExplicit) -Keep
    "--- PLANTED sticky: reconfigured WITHOUT the explicit empty values (must FAIL: the profile and v3 stay in the cache)"
    $sticky2 = Join-Path $scratch 'sticky2'
    py -I $chk $sticky2 --guest base --ours base
    if ($LASTEXITCODE -eq 0) { 'prove_build_flags: the planted sticky cache was NOT caught'; $rc = 1 }
} finally {
    Exit-RunLock $lock
    if ($madeJunction) { cmd /c rmdir "$gen" | Out-Null }
}
if ($rc) { 'prove_build_flags: FAIL' } else { 'prove_build_flags: PASS' }
exit $rc
