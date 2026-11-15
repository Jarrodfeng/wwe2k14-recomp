# Session 102 W1/W2: the cmake -D arguments of the release configures, in one place (make_release.ps1 and prove_build_flags.ps1 dot-source this).
# Every switch is passed EXPLICITLY on every configure, empty/OFF included: a cache value from an earlier -GuestProfdata or wide configure sticks in an
# incremental build folder otherwise (review B). Pure functions, no side effects.

function ConvertTo-CMakePath([string]$p) { return ($p -replace '\\', '/') }   # NOTE the doubled backslash: -replace takes a regex ('\' alone is invalid)

# -Wide: the AVX2 exe's folder. -Profdata F (full path, may be empty), -Narrow: profile for the guest code only. -OmitExplicit: PLANTED FAULT for the
# test (drops the explicit empty/OFF values, the way the first version did).
function Get-ReleaseConfigureArgs([switch]$Wide, [string]$Profdata = '', [switch]$Narrow, [switch]$OmitExplicit) {
    $a = @()
    $isa = if ($Wide) { 'x86-64-v3' } else { '' }
    if ($Wide -or -not $OmitExplicit) { $a += "-DWWE2K14_GUEST_ISA=$isa"; $a += "-DWWE2K14_OURS_ISA=$isa" }
    if ($Profdata) {
        $a += "-DWWE2K14_GUEST_PGO_USE=$(ConvertTo-CMakePath $Profdata)"
        $a += $(if ($Narrow) { '-DWWE2K14_PGO_OURS=OFF' } else { '-DWWE2K14_PGO_OURS=ON' })
    } elseif (-not $OmitExplicit) {
        $a += '-DWWE2K14_GUEST_PGO_USE='
        $a += '-DWWE2K14_PGO_OURS=OFF'
    }
    if (-not $OmitExplicit) { $a += '-DWWE2K14_GUEST_PGO_GEN=OFF' }
    return , $a
}

# Review G: profile entries the compiler found out of date in a build log (clang: -Wprofile-instr-out-of-date). Returns the count.
function Get-ProfileStaleCount([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath)) { throw "build log not found: $LogPath" }
    return @(Select-String -LiteralPath $LogPath -Pattern 'profile-instr-out-of-date|profile data may be out of date' ).Count
}
