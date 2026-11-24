# S106 C4: the checks behind tools\exe\lane_exe.ps1, as plain functions so the selftest can plant a fault in each one (dot-source this file).
# Nothing here builds, locks or writes outside the paths it is given. Evidence for every rule: docs\research\s106\C4.md, docs\systems\worktree_game_build.md.
. "$PSScriptRoot\lane_exe_lib2.ps1"   # S106 C4F: the fix round's functions (slot claim, publish, slot init, generated sync, options)

# What a slot worktree lacks (all gitignored) and gets by COPY from the main checkout: Name, Source (relative to main), Dest (relative to the slot), Kind, Required.
function Get-LaneExeInputs {
    @(
        [pscustomobject]@{ Name = 'xex';        Source = 'game_data\extracted\default.xex';                                    Dest = 'game_data\extracted\default.xex';                                    Kind = 'file'; Required = $true;  Why = "codegen's only game_data input" }
        [pscustomobject]@{ Name = 'stb';        Source = 'thirdparty\rexglue-sdk-src\thirdparty\stb';                          Dest = 'thirdparty\rexglue-sdk-src\thirdparty\stb';                          Kind = 'dir';  Required = $true;  Why = '16 relative #includes: a compile error without it' }
        [pscustomobject]@{ Name = 'xxHash';     Source = 'thirdparty\rexglue-sdk-src\thirdparty\xxHash';                       Dest = 'thirdparty\rexglue-sdk-src\thirdparty\xxHash';                       Kind = 'dir';  Required = $false; Why = '__has_include fallback exists' }
        [pscustomobject]@{ Name = 'streamline'; Source = 'thirdparty\streamline\sdk\include';                                  Dest = 'thirdparty\streamline\sdk\include';                                  Kind = 'dir';  Required = $true;  Why = 'without it the exe builds with NO DLSS code (RDR_HAS_STREAMLINE unset) and nothing fails' }
        [pscustomobject]@{ Name = 'ffx_dll';    Source = 'thirdparty\FidelityFX-SDK\ffx-api\bin\amd_fidelityfx_vk.dll';        Dest = 'thirdparty\FidelityFX-SDK\ffx-api\bin\amd_fidelityfx_vk.dll';        Kind = 'file'; Required = $true;  Why = 'POST_BUILD copies it only if it exists: an exe without FSR' }
        [pscustomobject]@{ Name = 'generated';  Source = 'generated\default';                                                  Dest = 'generated\default';                                                  Kind = 'gen';  Required = $true;  Why = 'codegen output (365 MB): seeded once (marker beside the folder); the slot''s own codegen owns it after' }
    )
}

# The inputs the main checkout cannot give: a list of "<name>: <path> (<why>)" strings. Empty = every REQUIRED input exists.
function Test-LaneExeSources([string]$Main) {
    $miss = @()
    foreach ($i in (Get-LaneExeInputs)) {
        if (-not $i.Required) { continue }
        $p = Join-Path $Main $i.Source
        $ok = if ($i.Kind -eq 'file') { Test-Path -LiteralPath $p -PathType Leaf } else { Test-Path -LiteralPath $p -PathType Container }
        if ($ok -and $i.Kind -ne 'file') { $ok = [bool](Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Select-Object -First 1) }
        if (-not $ok) { $miss += "$($i.Name): $p missing or empty ($($i.Why))" }
    }
    return $miss
}

# Reparse points (junctions, symlinks) anywhere in the slot but .git, and hard-linked files inside the copied-in folders. Empty = link-free.
function Test-LaneSlotLinks([string]$Slot) {
    $found = @()
    foreach ($it in (Get-ChildItem -LiteralPath $Slot -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)) {
        if ($it.FullName -like '*\.git\*' -or $it.Name -eq '.git') { continue }
        $found += "reparse point: $($it.FullName)"
    }
    foreach ($sub in @('generated', 'game_data', 'thirdparty')) {
        $d = Join-Path $Slot $sub
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { continue }
        foreach ($f in (Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            if ($f.LinkType -eq 'HardLink') { $found += "hard link: $($f.FullName)" }
        }
    }
    return $found
}

# File count + total bytes of a folder tree.
function Get-LaneTreeSize([string]$Dir) {
    $n = 0; $b = [int64]0
    foreach ($f in (Get-ChildItem -LiteralPath $Dir -Recurse -File -Force -ErrorAction SilentlyContinue)) { $n++; $b += $f.Length }
    return [pscustomobject]@{ Files = $n; Bytes = $b }
}

# A copy of generated\default must hold exactly the source's file count and bytes (a torn or short copy passes codegen's existence-only test). $null = ok, else the reason.
function Test-LaneGeneratedCopy([string]$Src, [string]$Dst) {
    if (-not (Test-Path -LiteralPath $Dst -PathType Container)) { return "generated copy missing: $Dst" }
    $a = Get-LaneTreeSize $Src; $b = Get-LaneTreeSize $Dst
    if ($a.Files -eq 0) { return "generated source is empty: $Src" }
    if ($a.Files -ne $b.Files -or $a.Bytes -ne $b.Bytes) { return "generated copy differs from the source: files $($b.Files) vs $($a.Files), bytes $($b.Bytes) vs $($a.Bytes)" }
    return $null
}

function Get-LaneFileSha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Same-option test: two option maps (hashtable / PSCustomObject / $null) with the same keys and values.
function ConvertTo-LaneOptionMap($X) {
    $h = @{}
    if ($null -eq $X) { return $h }
    if ($X -is [System.Collections.IDictionary]) { foreach ($k in $X.Keys) { $h["$k"] = "$($X[$k])" } }
    else { foreach ($p in $X.PSObject.Properties) { $h[$p.Name] = "$($p.Value)" } }
    return $h
}
function Test-LaneOptionsEqual($A, $B) {
    $ha = ConvertTo-LaneOptionMap $A; $hb = ConvertTo-LaneOptionMap $B
    if ($ha.Count -ne $hb.Count) { return $false }
    foreach ($k in $ha.Keys) { if (-not $hb.ContainsKey($k) -or $hb[$k] -ne $ha[$k]) { return $false } }
    return $true
}

# The state of a lane exe folder: New (absent or empty), Reuse (same commit, preset, options; the exe matches the recorded sha256) or Refuse (anything else: a lane folder is never replaced in place).
function Get-LaneFolderState([string]$Folder, [string]$Commit, [string]$Preset, $Options) {
    if (-not (Test-Path -LiteralPath $Folder)) { return [pscustomobject]@{ State = 'New'; Why = '' } }
    if (-not (Get-ChildItem -LiteralPath $Folder -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) { return [pscustomobject]@{ State = 'New'; Why = 'empty folder' } }
    $infoPath = Join-Path $Folder 'exe_info.json'
    $exe = Join-Path $Folder 'wwe2k14.exe'
    if (-not (Test-Path -LiteralPath $infoPath)) { return [pscustomobject]@{ State = 'Refuse'; Why = "folder holds files but no exe_info.json: $Folder" } }
    try { $info = [System.IO.File]::ReadAllText($infoPath) | ConvertFrom-Json } catch { return [pscustomobject]@{ State = 'Refuse'; Why = "exe_info.json unreadable: $($_.Exception.Message)" } }
    if (-not (Test-Path -LiteralPath $exe)) { return [pscustomobject]@{ State = 'Refuse'; Why = "exe_info.json but no wwe2k14.exe in $Folder" } }
    if ((Get-LaneFileSha $exe) -ne "$($info.sha256)".ToLowerInvariant()) { return [pscustomobject]@{ State = 'Refuse'; Why = "wwe2k14.exe in $Folder differs from the sha256 recorded in exe_info.json (a lane folder is never replaced in place)" } }
    if ("$($info.commit)" -ne $Commit) { return [pscustomobject]@{ State = 'Refuse'; Why = "folder holds commit $($info.commit), asked for $Commit" } }
    if ("$($info.preset)" -ne $Preset) { return [pscustomobject]@{ State = 'Refuse'; Why = "folder was built with preset $($info.preset), asked for $Preset" } }
    if (-not (Test-LaneOptionsEqual $info.options $Options)) { return [pscustomobject]@{ State = 'Refuse'; Why = "folder was built with other WWE2K14_* options than the main cache now holds" } }
    return [pscustomobject]@{ State = 'Reuse'; Why = 'same commit, preset, options; exe sha256 matches' }
}

# The WWE2K14_* BOOL options (ON / OFF) of the main build folder's cache, as an ordered map. Other values (e.g. a literal `$gl`) go to $Skipped.
function Get-LaneMainOptions([string]$Main, [string]$Preset, [ref]$Skipped) {
    $o = [ordered]@{}
    $c = Join-Path $Main "out\build\$Preset\CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $c)) { return $o }
    foreach ($l in [System.IO.File]::ReadLines($c)) {
        if ($l -match '^(WWE2K14_[A-Za-z0-9_]+):BOOL=(.*)$') {
            if ($Matches[2] -in 'ON', 'OFF') { $o[$Matches[1]] = $Matches[2] } elseif ($Skipped) { $Skipped.Value += "$($Matches[1])=$($Matches[2])" }
        }
    }
    return $o
}
