# Lane 310 (S101): change every place a release version lives, in one step. Jarrod picks the number; nothing here chooses it.
#
#   pwsh -NoProfile -File tools\release\bump_version.ps1 -Version v0.3.2                      dry run: prints what would change
#   pwsh -NoProfile -File tools\release\bump_version.ps1 -Version v0.3.2 -Apply               writes the files (commit them yourself)
#   pwsh -NoProfile -File tools\release\bump_version.ps1 -Version v0.3.2 -Apply -Draft docs\release\v0.3.2_draft.md
#       ... also puts the draft's "## v0.3.2" section into docs\release\CHANGELOG.md (when the CHANGELOG has none yet)
#   -AndroidVersion android-v0.3.2   the phone's number when it differs from "android-<Version>" (the phone may move alone)
#   -SkipAndroid / -SkipPc           change only one side
#
# The places (found 2026-11-13 by grep; the Android manifest holds NO version: build_apk.ps1 / check_apk.py read android\VERSION):
#   1. VERSION                      "vMAJOR.MINOR.PATCH" (release.ps1 compares it with -Version; CMake stamps it into the exe + launcher)
#   2. android\VERSION              "android-vMAJOR.MINOR.PATCH" (build_apk.ps1: versionCode = major*10000 + minor*100 + patch, versionName = this line;
#                                   minor and patch under 100; the new code must be above the old one or the APK cannot update an installed one)
#   3. docs\release\CHANGELOG.md    a "## <Version>" section on top (release.ps1 reads it as the notes' "What's new")
# Nothing else: README.md / CHANGELOG.md in the zip get {{VERSION}} / {{COMMIT}} stamped by make_release.ps1; the notes template by release.ps1.
# After this: look at the diff, remove every [CHECK BEFORE RELEASE] tag from the CHANGELOG section and KNOWN_ISSUES.md (release.ps1 refuses
# while any is left), commit, merge to main, then release.ps1.
param([Parameter(Mandatory)][string]$Version, [string]$AndroidVersion = '', [string]$Draft = '',
      [switch]$Apply, [switch]$SkipAndroid, [switch]$SkipPc, [string]$Root = '')
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
function Fail([string]$m) { Write-Host "bump_version: FAIL: $m"; exit 1 }
function Num([string]$v, [string]$prefix) {
    $m = [regex]::Match($v, '^' + [regex]::Escape($prefix) + 'v(\d+)\.(\d+)\.(\d+)$')
    if (-not $m.Success) { return $null }
    return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value)
}
function ReadLine1([string]$path) { (Get-Content -LiteralPath $path -TotalCount 1).Trim() }
# Files in this repo are CRLF on Windows: keep each file's own line ending.
function WriteKeeping([string]$path, [string]$text) {
    $old = [IO.File]::ReadAllText($path)
    $nl = if ($old.Contains("`r`n")) { "`r`n" } else { "`n" }
    [IO.File]::WriteAllText($path, ($text -replace "`r`n", "`n").Replace("`n", $nl), (New-Object Text.UTF8Encoding $false))
}

if ($Version -notmatch '^v\d+\.\d+\.\d+$') { Fail "-Version must look like v0.3.2 (a playtest/pre-release tag is not handled here), got '$Version'" }
if (-not $AndroidVersion) { $AndroidVersion = "android-$Version" }
$pc = Num $Version ''
$ph = Num $AndroidVersion 'android-'
if (-not $ph) { Fail "-AndroidVersion must look like android-v0.3.2, got '$AndroidVersion'" }
if ($ph[1] -gt 99 -or $ph[2] -gt 99) { Fail "the Android version needs minor and patch under 100 (versionCode = major*10000 + minor*100 + patch)" }

$verFile = Join-Path $Root 'VERSION'
$andFile = Join-Path $Root 'android\VERSION'
$logFile = Join-Path $Root 'docs\release\CHANGELOG.md'
$changes = @()

if (-not $SkipPc) {
    $cur = ReadLine1 $verFile
    $a = Num $cur ''
    if ($a -and ($pc[0] * 1000000 + $pc[1] * 1000 + $pc[2]) -le ($a[0] * 1000000 + $a[1] * 1000 + $a[2])) { Fail "VERSION is already $cur; $Version is not newer" }
    $changes += "VERSION: $cur -> $Version"
}
if (-not $SkipAndroid) {
    $cur = ReadLine1 $andFile
    $a = Num $cur 'android-'
    $oldCode = if ($a) { $a[0] * 10000 + $a[1] * 100 + $a[2] } else { 0 }
    $newCode = $ph[0] * 10000 + $ph[1] * 100 + $ph[2]
    if ($newCode -le $oldCode) { Fail "android\VERSION is already $cur (versionCode $oldCode); $AndroidVersion gives $newCode, which would not install over it" }
    $changes += "android\VERSION: $cur -> $AndroidVersion (versionCode $oldCode -> $newCode; AndroidManifest.xml holds none)"
}

# The CHANGELOG section (PC side).
$logText = [IO.File]::ReadAllText($logFile)
$hasSection = [regex]::IsMatch($logText, '(?m)^## ' + [regex]::Escape($Version) + '[ \t]*\r?$')
$section = ''
if (-not $SkipPc) {
    if ($hasSection) {
        $changes += "docs\release\CHANGELOG.md: '## $Version' already there (left alone)"
    } else {
        if ($Draft) {
            $dp = if ([IO.Path]::IsPathRooted($Draft)) { $Draft } else { Join-Path $Root $Draft }
            if (-not (Test-Path -LiteralPath $dp)) { Fail "no draft file $dp" }
            $dm = [regex]::Match([IO.File]::ReadAllText($dp), '(?ms)^## ' + [regex]::Escape($Version) + '[ \t]*\r?\n(.*?)(?=^## |^---\s*$|^### (?:Left off|Known issues|Not on this branch|Release mechanics)|\z)')
            if (-not $dm.Success) { Fail "$Draft has no '## $Version' section" }
            $section = "## $Version`n`n" + $dm.Groups[1].Value.Trim() + "`n`n"
            $tags = ([regex]::Matches($section, '\[CHECK BEFORE RELEASE')).Count
            $changes += "docs\release\CHANGELOG.md: new '## $Version' section from $Draft ($($section.Length) chars, $tags [CHECK BEFORE RELEASE] tags left in it: remove them)"
        } else {
            $section = "## $Version`n`n**${Version}: TODO headline.**`n`n"
            $changes += "docs\release\CHANGELOG.md: new '## $Version' heading with a TODO line (write the text, or re-run with -Draft FILE)"
        }
    }
}

Write-Host "bump_version: $(if ($Apply) { 'APPLYING' } else { 'dry run (add -Apply)' }) in $Root"
$changes | ForEach-Object { Write-Host "  $_" }
if (-not $Apply) { exit 0 }

if (-not $SkipPc) { WriteKeeping $verFile "$Version`n" }
if (-not $SkipAndroid) { WriteKeeping $andFile "$AndroidVersion`n" }
if ($section) {
    $at = [regex]::Match($logText, '(?m)^## ')
    if (-not $at.Success) { Fail 'CHANGELOG.md has no "## " heading to insert before' }
    WriteKeeping $logFile ($logText.Substring(0, $at.Index) + $section + $logText.Substring($at.Index))
}
Write-Host 'bump_version: done. Next: edit the CHANGELOG section (no [CHECK BEFORE RELEASE] tags left), KNOWN_ISSUES.md the same, commit, merge to main, release.ps1.'
