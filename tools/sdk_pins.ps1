# SDK DLL pinning (Session 8, task 2): every ReXGlue DLL the game loads must be on the pinned list
# tools\sdk_pins.json (file name + SHA256 -> the SDK commit and patch file it was built from), so every run is
# traceable to an SDK commit and a stray experimental DLL can't sneak into a run unnoticed.
#
# Library (dot-source it):   . C:\Users\Jarrod\wwe2k14-recomp\tools\sdk_pins.ps1
#   Get-SdkPinList [-PinFile P]                      the parsed pin list (throws if missing/malformed)
#   Test-SdkPinFiles -Paths <files> [-PinFile P]     one result per file: Name, Path, Sha256, Pinned, PinIds
#   Get-SdkStageDirs [-Preset P]                     the SDK bin folder(s) a build stages rex*.dll from
#   Get-SdkPinLine [-ExeDir D] [-PinFile P]          one line naming the pin entry of the staged DLLs (or UNPINNED)
#   Write-SdkPinRefusal -Results R -Context TEXT     the "refused" explanation (file, hash, what is pinned, how to fix)
# Callers: tools\build.ps1 (refuses unpinned DLLs; -CheckOnly), tools\build_sdk.ps1 -Pin (adds entries),
# tools\run_and_triage.ps1 (pin line per run), tools\codegen_repro\run_codegen.py (rexglue.exe pin).

$script:SdkPinRepo = 'C:\Users\Jarrod\wwe2k14-recomp'
$script:SdkPinDefaultFile = Join-Path $script:SdkPinRepo 'tools\sdk_pins.json'

function Get-SdkPinList([string]$PinFile = '') {
    if (-not $PinFile) { $PinFile = $script:SdkPinDefaultFile }
    if (-not (Test-Path -LiteralPath $PinFile)) { throw "pin list not found: $PinFile" }
    $doc = [System.IO.File]::ReadAllText($PinFile) | ConvertFrom-Json
    if (-not $doc.pins) { throw "pin list has no 'pins' array: $PinFile" }
    foreach ($p in $doc.pins) {
        if (-not $p.id -or -not $p.files) { throw "pin entry without id/files in $PinFile" }
        foreach ($f in $p.files) {
            if (-not $f.name -or ($f.sha256 -notmatch '^[0-9a-fA-F]{64}$')) { throw "pin '$($p.id)' has a file entry without name/sha256 in $PinFile" }
        }
    }
    return $doc
}

function Get-SdkFileSha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Test-SdkPinFiles {
    param([string[]]$Paths, [string]$PinFile = '', $Doc = $null)
    if (-not $Doc) { $Doc = Get-SdkPinList $PinFile }
    foreach ($path in $Paths) {
        $name = Split-Path $path -Leaf
        $sha = Get-SdkFileSha256 $path
        $ids = @($Doc.pins | Where-Object { @($_.files | Where-Object { $_.name -ieq $name -and $_.sha256.ToLowerInvariant() -eq $sha }).Count -gt 0 } | ForEach-Object id)
        [pscustomobject]@{ Name = $name; Path = (Resolve-Path -LiteralPath $path).Path; Sha256 = $sha; Pinned = ($ids.Count -gt 0); PinIds = $ids }
    }
}

# The SDK bin folder(s) the exe build stages rex*.dll from: the one CMake was configured with (rexglue_DIR in the
# preset's CMakeCache; the POST_BUILD copy uses it) and $env:REXSDK\bin (what build.ps1's own staging loop copies;
# env.ps1's choice when REXSDK isn't set). Usually the same folder.
function Get-SdkStageDirs([string]$Preset = 'win-amd64-release') {
    $dirs = @()
    $cache = Join-Path $script:SdkPinRepo "out\build\$Preset\CMakeCache.txt"
    if (Test-Path -LiteralPath $cache) {
        $line = Select-String -LiteralPath $cache -Pattern '^rexglue_DIR:PATH=(.+)$' | Select-Object -First 1
        if ($line) {
            $cfg = $line.Matches[0].Groups[1].Value.Trim()
            $bin = [System.IO.Path]::GetFullPath((Join-Path $cfg '..\..\..\bin'))
            if (Test-Path -LiteralPath $bin) { $dirs += $bin }
        }
    }
    $rexsdk = $env:REXSDK
    if (-not $rexsdk) {
        foreach ($candidate in 'rexglue-sdk-vulkan', 'rexglue-sdk') {
            $sdk = Join-Path $script:SdkPinRepo "thirdparty\$candidate"
            if (Test-Path -LiteralPath "$sdk\lib\cmake\rexglue\rexglueConfig.cmake") { $rexsdk = $sdk; break }
        }
    }
    if ($rexsdk -and (Test-Path -LiteralPath "$rexsdk\bin")) { $dirs += [System.IO.Path]::GetFullPath("$rexsdk\bin") }
    $seen = @{}   # case-insensitive de-dup (hashtable keys ignore case)
    return @($dirs | Where-Object { $k = $_.TrimEnd('\'); if ($seen.ContainsKey($k)) { $false } else { $seen[$k] = 1; $true } } | ForEach-Object { $_.TrimEnd('\') })
}

function Get-SdkPinById($Doc, [string]$Id) { $Doc.pins | Where-Object { $_.id -eq $Id } | Select-Object -First 1 }

# Names the pin entry that covers ALL the given files (newest entry wins), else per-file pins, else UNPINNED.
function Format-SdkPinSummary($Results, $Doc) {
    $Results = @($Results)
    if ($Results.Count -eq 0) { return 'NO rex*.dll found' }
    $short = ($Results | ForEach-Object { '{0} {1}' -f $_.Name, $_.Sha256.Substring(0, 12) }) -join ', '
    $unpinned = @($Results | Where-Object { -not $_.Pinned })
    if ($unpinned.Count) {
        return 'UNPINNED (' + (($unpinned | ForEach-Object { '{0} sha256 {1}' -f $_.Name, $_.Sha256.Substring(0, 12) }) -join ', ') + ') [' + $short + ']'
    }
    # Pin ids that contain every file (plain list: PowerShell would unroll a 0/1-element array assigned from an if).
    $common = [System.Collections.Generic.List[string]]::new()
    foreach ($id in @($Results[0].PinIds)) { $common.Add([string]$id) }
    foreach ($r in $Results) { $ids = @($r.PinIds); foreach ($id in @($common)) { if ($ids -notcontains $id) { [void]$common.Remove($id) } } }
    if ($common.Count -gt 0) {
        $order = @($Doc.pins | ForEach-Object id)
        $id = $common | Sort-Object { [array]::IndexOf($order, $_) } | Select-Object -Last 1
        $p = Get-SdkPinById $Doc $id
        $commit = if ($p.sdk_commit) { $p.sdk_commit.Substring(0, [Math]::Min(7, $p.sdk_commit.Length)) } else { '?' }
        $patch = if ($p.patch) { Split-Path $p.patch -Leaf } else { 'no patch file' }
        # Caveats (fixer, review finding 3): a patch that only 'contained'-matched, or a tree dirty at pin time.
        $cav = @()
        if ($p.patch_match -and $p.patch_match -notmatch '^(exact|given)') { $cav += ($p.patch_match -split ':')[0] }
        if ($p.verified -and $p.verified -match '^WARNING') { $cav += '+uncommitted' }
        if ($cav.Count) { $patch += ' (' + ($cav -join ', ') + ')' }
        return "$id = SDK $commit ($($p.sdk_branch)), $patch [$short]"
    }
    return 'MIXED pins (' + (($Results | ForEach-Object { '{0} -> {1}' -f $_.Name, ($_.PinIds -join '/') }) -join ', ') + ') [' + $short + ']'
}

function Get-SdkPinLine([string]$ExeDir = 'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release', [string]$PinFile = '') {
    try {
        $doc = Get-SdkPinList $PinFile
        $dlls = @(Get-ChildItem -LiteralPath $ExeDir -Filter 'rex*.dll' -File -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object FullName)
        return 'sdk pin: ' + (Format-SdkPinSummary (Test-SdkPinFiles -Paths $dlls -Doc $doc) $doc)
    } catch { return "sdk pin: CHECK FAILED ($($_.Exception.Message))" }
}

# Which patches\rexglue-sdk\*.patch matches the SDK tree: 'exact' when `git diff P HEAD` is byte-identical to the
# newest patch for some first-parent ancestor P (how our patch files are made), 'contained' when it only
# reverse-applies (HEAD has more than that patch), else none. Returns @{ patch; match }.
function Find-SdkPatchMatch([string]$SdkDir, [string]$PatchDir = 'C:\Users\Jarrod\wwe2k14-recomp\patches\rexglue-sdk', [int]$Depth = 40) {
    $newest = Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if (-not $newest) { return @{ patch = $null; match = 'none: no patch files' } }
    $rel = 'patches/rexglue-sdk/' + $newest.Name
    $want = Get-SdkFileSha256 $newest.FullName
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $ancestors = @(git -C $SdkDir rev-list --first-parent -n $Depth HEAD~1 2>$null)
        foreach ($p in $ancestors) {
            git -C $SdkDir diff $p HEAD "--output=$tmp" 2>$null
            if ((Get-Item -LiteralPath $tmp).Length -eq $newest.Length -and (Get-SdkFileSha256 $tmp) -eq $want) {
                return @{ patch = $rel; match = "exact: git diff $($p.Substring(0, 7)) HEAD is byte-identical to the patch file" }
            }
        }
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    git -C $SdkDir apply -R --check $newest.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { return @{ patch = $rel; match = 'contained: the patch reverse-applies but HEAD has further changes (export a new patch file)' } }
    return @{ patch = $null; match = "none: newest patch $($newest.Name) does not match the tree" }
}

# Adds a pin entry for the given files (build_sdk.ps1 -Pin). Skips if an entry with the same commit and hashes exists.
function Add-SdkPin {
    param([Parameter(Mandatory)][string]$Note, [Parameter(Mandatory)][string[]]$Files, [string]$SdkDir = 'C:\Users\Jarrod\wwe2k14-recomp\thirdparty\rexglue-sdk-src',
          [string]$Id = '', [string]$Patch = '', [string]$PinFile = '', [string]$Built = '', [string]$AllowDirty = '')
    if (-not $PinFile) { $PinFile = $script:SdkPinDefaultFile }
    $doc = Get-SdkPinList $PinFile
    $commit = (git -C $SdkDir rev-parse HEAD).Trim()
    $branch = (git -C $SdkDir rev-parse --abbrev-ref HEAD).Trim()
    $dirty = @(git -C $SdkDir status --porcelain --ignore-submodules=dirty 2>$null | Where-Object { $_ })
    # Fixer (review finding 3): a dirty tree means the DLL is not the commit; refuse unless a reason is given.
    if ($dirty.Count -and -not $AllowDirty.Trim()) {
        throw ("the SDK tree has $($dirty.Count) uncommitted change(s) (" + (($dirty | Select-Object -First 3) -join '; ') +
               '); commit them first, or pass -PinAllowDirty "reason" to build_sdk.ps1')
    }
    $entries = @(foreach ($f in $Files) { $i = Get-Item -LiteralPath $f; [ordered]@{ name = $i.Name; sha256 = (Get-SdkFileSha256 $i.FullName); size = $i.Length } })
    # De-duplicate by name+hash (staged copy and install copy are normally identical); two different hashes for one name = error.
    $byName = @{}
    foreach ($e in $entries) {
        if ($byName.ContainsKey($e.name) -and $byName[$e.name].sha256 -ne $e.sha256) { throw "two different $($e.name) builds given (staged vs installed differ?): $($byName[$e.name].sha256) / $($e.sha256)" }
        $byName[$e.name] = $e
    }
    $pinSet = @($byName.Keys | Sort-Object | ForEach-Object { $byName[$_] })
    foreach ($p in $doc.pins) {
        if ($p.sdk_commit -eq $commit) {
            $same = @($pinSet | Where-Object { $n = $_.name; $h = $_.sha256; @($p.files | Where-Object { $_.name -ieq $n -and $_.sha256 -eq $h }).Count -gt 0 }).Count -eq $pinSet.Count
            if ($same) { return "sdk pin: already pinned as '$($p.id)' (same SDK commit and hashes); nothing added" }
        }
    }
    if ($Patch) { $pm = @{ patch = $Patch; match = 'given with -PinPatch' } } else { $pm = Find-SdkPatchMatch $SdkDir }
    if (-not $Id) { $Id = 'pin-' + $commit.Substring(0, 7) + '-' + (Get-Date).ToString('MMdd-HHmm') }
    if (@($doc.pins | Where-Object { $_.id -eq $Id }).Count) { $Id = $Id + '-' + (Get-Date).ToString('HHmmss') }
    $entry = [ordered]@{ id = $Id; sdk_commit = $commit; sdk_branch = $branch; patch = $pm.patch; patch_match = $pm.match;
        date = (Get-Date).ToString('yyyy-MM-dd'); built = $Built;
        verified = $(if ($dirty.Count) { "WARNING: SDK tree had $($dirty.Count) uncommitted change(s) at pin time (allowed: $($AllowDirty.Trim())): " +(($dirty | Select-Object -First 5) -join '; ') } else { 'SDK tree clean at pin time (submodule working-tree noise ignored)' });
        note = $Note; files = $pinSet }
    $list = [System.Collections.Generic.List[object]]::new()
    foreach ($p in $doc.pins) { $list.Add($p) }
    $list.Add([pscustomobject]$entry)
    $doc.pins = $list.ToArray()
    $json = $doc | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($PinFile, $json + "`n")
    "sdk pin: added '$Id' = SDK $($commit.Substring(0, 7)) ($branch), patch $($pm.patch) [$($pm.match)]"
    foreach ($f in $pinSet) { '  {0,-18} {1}' -f $f.name, $f.sha256 }
    if ($dirty.Count) { "  WARNING: the SDK tree is not clean ($($dirty.Count) change(s)); this DLL can't be rebuilt from the commit alone" }
}

function Write-SdkPinRefusal($Results, [string]$Context, $Doc, [string]$PinFile = '') {
    if (-not $PinFile) { $PinFile = $script:SdkPinDefaultFile }
    $known = @($Doc.pins | ForEach-Object { $p = $_; $p.files | ForEach-Object { '      {0,-18} {1}  pin {2} (SDK {3}, {4})' -f $_.name, $_.sha256, $p.id, $(if ($p.sdk_commit) { $p.sdk_commit.Substring(0, 7) } else { '?' }), $p.date } })
    foreach ($r in @($Results | Where-Object { -not $_.Pinned })) {
        "REFUSED: unpinned SDK file ($Context)"
        "  file:    $($r.Path)"
        "  sha256:  $($r.Sha256)"
        "  pinned for $($r.Name):"
        $mine = @($known | Where-Object { $_ -match ('^\s+' + [regex]::Escape($r.Name) + '\s') })
        if ($mine.Count) { $mine } else { '      (no pin for this file name)' }
    }
    "  pin list: $PinFile"
    '  to pin a DLL you built from a committed SDK tree: pwsh -File tools\build_sdk.ps1 -Run NAME -Pin "what this build is"'
    '  (adds the staged hashes with the SDK commit). To build anyway once: tools\build.ps1 ... -AllowUnpinnedSdk "reason"'
    '  (logged to logs\sdk_pin_overrides.log). A leftover staged DLL the build would replace also blocks the build.'
}
