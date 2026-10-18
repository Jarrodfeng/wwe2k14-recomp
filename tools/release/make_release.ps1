# Session 25 item 3: build the release zip, and fail if anything forbidden is inside.
#
#   pwsh -NoProfile -File tools\release\make_release.ps1 -Version v0.1.0-playtest [-Playtest] [-Incremental]
#
# 1. Refuses uncommitted changes to tracked files, a -Version different from the VERSION file (commit VERSION first),
#    and an SDK patch export (patches\rexglue-sdk\full) that doesn't rebuild the SDK branch exactly.
#    -Playtest: the version must say "playtest" (the launcher then turns the performance report on by default).
# 2. Builds from the committed state in its own folder, out\release\build (deleted first unless -Incremental), with
#    -DWWE2K14_RELEASE=ON (no "+dev" in the version), under the run lock. The SDK DLLs come from the same SDK as
#    tools\build.ps1 and must pass its pin check.
# 3. Collects exactly these files (nothing is picked up by wildcard):
#      wwe2k14.exe, wwe2k14_launcher.exe, rexruntime.dll, rexgpu-xenos.dll (build outputs)
#      data\ids\characters.json, data\ids\arenas.json (our id tables, db\ids)
#      README.md, LICENSE, THIRD_PARTY_NOTICES.md, CHANGELOG.md (docs\release)
#    The list of runtime files was found from the imports of each binary (llvm-objdump -p) and a run from an empty
#    folder (Session 25 Item 3 check).
#    Third-party redistributables (Jarrod, 2026-10-17), copied unmodified and checked byte for byte against their
#    source:
#      msvcp140.dll, msvcp140_atomic_wait.dll, vcruntime140.dll, vcruntime140_1.dll: the Microsoft Visual C++
#        runtime, app-local next to the exe (Visual Studio 2022's Distributable Code list: "any of the files within
#        [VisualStudioFolder]\VC\redist", unmodified);
#      nvidia\sl.interposer.dll, sl.common.dll, sl.dlss.dll (NVIDIA Streamline, MIT), nvidia\nvngx_dlss.dll (NVIDIA RTX
#        SDK licence: distributable in an application, with NVIDIA's terms passed on and NVIDIA DLSS credited) and
#        NVIDIA's licence files. The game asks Streamline for kFeatureDLSS only; DLSS ran from a folder holding just
#        these four (run 1758).
# 4. The forbidden-content scan (any hit fails the release, nothing is zipped):
#      extensions .pac .bik .bik2 .xex .iso .cac (and .pdb / .map: symbols are not shipped);
#      a source path under game_data\, generated\, or out\ other than out\release\build's four binaries;
#      NVIDIA files (nvngx_*, sl.*) other than the redistributables above, which must equal their source byte for byte;
#      a SHA-256 match with any file under game_data\ (only same-size files are hashed: exact and fast).
#    The finished zip's entry list is scanned again.
# 5. Writes out\release\wwe2k14-recomp-<Version>.zip, its .sha256 and a copy of the docs.
#
#   -SelfTest   test the scan itself (no build, no zip): a scratch stage with three planted forbidden files (a game
#               file's bytes under a harmless name, an nvngx_*.dll, a .bik2) must give three hits; exit 0 = the scan
#               caught all three.
param([string]$Version = '', [switch]$Playtest, [switch]$Incremental, [switch]$SelfTest)
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $R

function Fail([string]$why) { "make_release: FAILED: $why"; exit 1 }

$badExt = '.pac', '.bik', '.bik2', '.xex', '.iso', '.cac', '.pdb', '.map'

# The forbidden-content scan of $stage. $files: published path -> source; $buildOutputs: the published paths that
# may come from $build. Returns the hits (empty = clean).
function Get-ForbiddenHits([string]$stage, $files, [string[]]$buildOutputs, [string]$build, $redist = @{}) {
    $hits = [System.Collections.Generic.List[string]]::new()
    $tracked = @(git -C $R ls-files | ForEach-Object { (Join-Path $R $_).ToLowerInvariant() -replace '/', '\' })
    $staged = @(Get-ChildItem -Recurse -File $stage)
    foreach ($f in $staged) {
        $rel = $f.FullName.Substring($stage.Length + 1)
        if ($badExt -contains $f.Extension.ToLowerInvariant()) { $hits.Add("forbidden extension: $rel") }
        if ($redist.Contains($rel)) {
            # A third-party redistributable: allowed only as an unmodified copy of its source.
            if ((Get-FileHash -LiteralPath $f.FullName).Hash -ne (Get-FileHash -LiteralPath $redist[$rel]).Hash) {
                $hits.Add("redistributable differs from its source: $rel")
            }
            continue
        }
        if ($f.Name -match '^(nvngx_|sl\.)') { $hits.Add("NVIDIA file: $rel") }
        if (-not $files.Contains($rel)) { $hits.Add("not on the file list: $rel") }
    }
    foreach ($k in $files.Keys) {
        if ($redist.Contains($k)) { continue }  # checked above (byte for byte against its source)
        $src = [System.IO.Path]::GetFullPath($files[$k]).ToLowerInvariant()
        $isBuild = $buildOutputs -contains $k -and $src.StartsWith(([System.IO.Path]::GetFullPath($build)).ToLowerInvariant() + '\')
        foreach ($forbidden in 'game_data', 'generated', 'out') {
            $root = (Join-Path $R $forbidden).ToLowerInvariant() + '\'
            if ($src.StartsWith($root) -and -not $isBuild) { $hits.Add("from $forbidden\: $k <- $src") }
        }
        if (-not $isBuild -and $tracked -notcontains $src) { $hits.Add("not a committed file: $k <- $src") }
    }
    # Hash match against every file under game_data\ (same size only; the size list is cheap).
    $bySize = @{}
    foreach ($f in $staged) { $bySize[[string]$f.Length] = $true }
    $gameFiles = 0; $hashed = 0
    foreach ($g in Get-ChildItem -Recurse -File (Join-Path $R 'game_data') -ErrorAction SilentlyContinue) {
        $gameFiles++
        if (-not $bySize.ContainsKey([string]$g.Length)) { continue }
        $hashed++
        $gh = (Get-FileHash -LiteralPath $g.FullName -Algorithm SHA256).Hash
        foreach ($f in $staged | Where-Object { $_.Length -eq $g.Length }) {
            if ((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash -eq $gh) {
                $hits.Add("same bytes as a game file: $($f.Name) = $($g.FullName.Substring($R.Length + 1))")
            }
        }
    }
    Write-Host "scan: $($staged.Count) files staged; $gameFiles game files checked by size, $hashed hashed"
    return , $hits
}

if ($SelfTest) {
    $st = Join-Path $R 'out\release\selftest'
    if (Test-Path $st) { Remove-Item -Recurse -Force $st }
    $null = New-Item -ItemType Directory -Force -Path $st
    # The smallest game file (fast to hash), copied under a harmless name: only the hash check can catch it.
    $small = Get-ChildItem -Recurse -File (Join-Path $R 'game_data\extracted') | Where-Object Length -gt 64 | Sort-Object Length | Select-Object -First 1
    Copy-Item -LiteralPath $small.FullName (Join-Path $st 'notes.txt')
    Set-Content -LiteralPath (Join-Path $st 'nvngx_dlss.dll') -Value 'not a real dll'
    Set-Content -LiteralPath (Join-Path $st 'intro.bik2') -Value 'not a real movie'
    # An allowed redistributable whose bytes differ from its source (a tampered or wrong file).
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $st 'nvidia')
    Set-Content -LiteralPath (Join-Path $st 'nvidia\sl.dlss.dll') -Value 'not the real sl.dlss.dll'
    $planted = [ordered]@{ 'notes.txt' = Join-Path $R 'README.md'; 'nvngx_dlss.dll' = Join-Path $R 'README.md'; 'intro.bik2' = Join-Path $R 'README.md'
                           'nvidia\sl.dlss.dll' = Join-Path $R 'thirdparty\streamline\sdk\bin\x64\sl.dlss.dll' }
    $red = @{ 'nvidia\sl.dlss.dll' = Join-Path $R 'thirdparty\streamline\sdk\bin\x64\sl.dlss.dll' }
    $h = Get-ForbiddenHits $st $planted @() $st $red
    $h | ForEach-Object { "  hit: $_" }
    $want = @('same bytes as a game file: notes.txt', 'NVIDIA file: nvngx_dlss.dll', 'forbidden extension: intro.bik2',
              'redistributable differs from its source: nvidia\sl.dlss.dll')
    $missed = @($want | Where-Object { $w = $_; -not ($h | Where-Object { $_.StartsWith($w) }) })
    Remove-Item -Recurse -Force $st
    if ($missed.Count) { "make_release -SelfTest: FAILED, missed: $($missed -join '; ')"; exit 1 }
    "make_release -SelfTest: PASS (the scan caught all $($want.Count) planted files; game file planted: $($small.Length) bytes)"
    exit 0
}
if (-not $Version) { Fail "-Version is required" }

# ---- 1. state
$fileVersion = (Get-Content (Join-Path $R 'VERSION') -TotalCount 1).Trim()
if ($fileVersion -ne $Version) { Fail "VERSION says '$fileVersion', -Version is '$Version' (edit VERSION and commit it first)" }
if ($Playtest -and $Version -notmatch 'playtest') { Fail "-Playtest needs a version with 'playtest' in it" }
if (-not $Playtest -and $Version -match 'playtest') { Fail "'$Version' is a playtest version: pass -Playtest" }
$dirty = @(git status --porcelain --untracked-files=no)
if ($dirty.Count) { Fail "uncommitted changes:`n$($dirty -join "`n")" }
# The SDK changes travel only as patches\rexglue-sdk\full: they must rebuild the SDK branch exactly (Session 25: the old
# patch series silently didn't).
$sdkCheck = py "$R\tools\sdk_patches\export_series.py" --check 2>&1
if ($LASTEXITCODE) { Fail "the SDK patch export is out of date (run py tools\sdk_patches\export_series.py, commit full\):`n$($sdkCheck -join "`n")" }
$sdkCheck | Select-Object -Last 1
$commit = (git rev-parse --short HEAD).Trim()
"make_release: $Version from commit $commit$(if ($Playtest) { ' (playtest)' })"

# ---- 2. build
$build = Join-Path $R 'out\release\build'
. "$R\tools\env.ps1" *> $null
. "$R\tools\runlock.ps1"
$sdkCmake = (Select-String -Path "$R\out\build\win-amd64-release\CMakeCache.txt" -Pattern '^rexglue_DIR:PATH=(.*)$').Matches[0].Groups[1].Value
$lock = Enter-RunLock -Name "make_release $Version"
try {
    if (-not $Incremental -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
    $t0 = Get-Date
    $log = Join-Path $R "logs\release_build_$Version.log"
    cmake --preset win-amd64-release -B $build -DWWE2K14_RELEASE=ON "-Drexglue_DIR=$sdkCmake" *>&1 | Out-File $log
    if ($LASTEXITCODE) { Fail "configure failed (see $log)" }
    cmake --build $build -j 16 *>&1 | Out-File $log -Append
    if ($LASTEXITCODE) { Fail "build failed (see $log)" }
    foreach ($dll in Get-ChildItem "$env:REXSDK\bin\rex*.dll") { Copy-Item $dll.FullName (Join-Path $build $dll.Name) -Force }
    "build: $([int]((Get-Date) - $t0).TotalSeconds) s"
} finally { Exit-RunLock $lock }
$pin = pwsh -NoProfile -File "$R\tools\build.ps1" -CheckOnly -ExeDir $build
$pin | Select-Object -Last 1
if ($LASTEXITCODE) { Fail "SDK pin check failed:`n$($pin -join "`n")" }

# ---- 3. collect
$name = "wwe2k14-recomp-$Version"
$stage = Join-Path $R "out\release\stage\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$null = New-Item -ItemType Directory -Force -Path $stage
# published path -> source
$files = [ordered]@{
    'wwe2k14.exe'             = Join-Path $build 'wwe2k14.exe'
    'wwe2k14_launcher.exe'    = Join-Path $build 'wwe2k14_launcher.exe'
    'rexruntime.dll'          = Join-Path $build 'rexruntime.dll'
    'rexgpu-xenos.dll'        = Join-Path $build 'rexgpu-xenos.dll'
    'data\ids\characters.json'= Join-Path $R 'db\ids\characters.json'
    'data\ids\arenas.json'    = Join-Path $R 'db\ids\arenas.json'
    'README.md'               = Join-Path $R 'docs\release\README.md'
    'LICENSE'                 = Join-Path $R 'docs\release\LICENSE'
    'THIRD_PARTY_NOTICES.md'  = Join-Path $R 'docs\release\THIRD_PARTY_NOTICES.md'
    'CHANGELOG.md'            = Join-Path $R 'docs\release\CHANGELOG.md'
}
$buildOutputs = @('wwe2k14.exe', 'wwe2k14_launcher.exe', 'rexruntime.dll', 'rexgpu-xenos.dll')
# Third-party redistributables: published path -> source (copied unmodified; the scan compares them byte for byte).
$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { Fail "no Visual C++ redist folder (Microsoft.VC143.CRT) found" }
$nv = Join-Path $R 'thirdparty\streamline\sdk\bin\x64'
$redist = [ordered]@{}
foreach ($d in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { $redist[$d] = Join-Path $crt.FullName $d }
foreach ($d in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll', 'nvngx_dlss.license.txt') { $redist["nvidia\$d"] = Join-Path $nv $d }
$redist['nvidia\streamline.license.txt'] = Join-Path $R 'thirdparty\streamline\sdk\license.txt'
"redistributables: VC++ runtime from $($crt.FullName); NVIDIA from $nv"
foreach ($k in $redist.Keys) { $files[$k] = $redist[$k] }
foreach ($k in $files.Keys) {
    $src = $files[$k]
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Fail "missing $src" }
    $dst = Join-Path $stage $k
    $null = New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent)
    Copy-Item -LiteralPath $src $dst
}
# The version stamped into the docs.
foreach ($doc in 'README.md', 'CHANGELOG.md') {
    $p = Join-Path $stage $doc
    (Get-Content -Raw $p).Replace('{{VERSION}}', $Version).Replace('{{COMMIT}}', $commit) | Set-Content -NoNewline $p
}

# The full licence texts, from each library's own file (docs\release\third_party_licenses.txt: name | path[#head:N|#tail:N]).
$notices = Join-Path $stage 'THIRD_PARTY_NOTICES.md'
foreach ($line in Get-Content (Join-Path $R 'docs\release\third_party_licenses.txt')) {
    if ($line -match '^\s*(#|$)') { continue }
    $libName, $spec = ($line -split '\|', 2) | ForEach-Object { $_.Trim() }
    $path, $cut = $spec -split '#', 2
    $full = Join-Path $R $path
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { Fail "licence file missing: $path ($libName)" }
    $text = Get-Content -LiteralPath $full
    if ($cut -match '^head:(\d+)$') { $text = $text | Select-Object -First ([int]$Matches[1]) }
    if ($cut -match '^tail:(\d+)$') { $text = $text | Select-Object -Last ([int]$Matches[1]) }
    Add-Content -LiteralPath $notices -Value "`n## $libName`n`nFrom ``$path``$(if ($cut) { " ($cut lines)" }):`n`n``````text"
    Add-Content -LiteralPath $notices -Value $text
    Add-Content -LiteralPath $notices -Value '```'
}

# ---- 4. the forbidden-content scan
$hits = Get-ForbiddenHits $stage $files $buildOutputs $build $redist
if ($hits.Count) { Fail "forbidden content:`n  $($hits -join "`n  ")" }

# ---- 5. zip
$outDir = Join-Path $R 'out\release'
$zip = Join-Path $outDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Add-Type -AssemblyName System.IO.Compression.FileSystem
$za = [System.IO.Compression.ZipFile]::OpenRead($zip)
try { $entries = @($za.Entries | ForEach-Object FullName) } finally { $za.Dispose() }
$allowedRedist = @($redist.Keys | ForEach-Object { "$name/$_".Replace('\', '/') })
foreach ($e in $entries) {
    $ext = [System.IO.Path]::GetExtension($e).ToLowerInvariant()
    $leaf = Split-Path $e -Leaf
    if ($allowedRedist -contains $e.Replace('\', '/')) { continue }  # checked byte for byte in the scan
    if ($badExt -contains $ext -or $leaf -match '^(nvngx_|sl\.)') { Remove-Item -Force $zip; Fail "the zip holds $e" }
}
$sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$sha  $name.zip" | Set-Content -LiteralPath "$zip.sha256"
foreach ($doc in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md') { Copy-Item (Join-Path $stage $doc) (Join-Path $outDir $doc) -Force }
"zip: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, $($entries.Count) entries)"
"sha256: $sha"
"make_release: PASS ($Version, commit $commit)"
