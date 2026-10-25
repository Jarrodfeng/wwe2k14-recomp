# Session 65 (Jarrod: "a friend who wants to help out with the development of the android build ... Can we make a zip
# package of everything they'd need"; contents: "Code + Android notes"): a developer zip for a helper, made from
# committed files only (nothing gitignored can get in: no game_data\, generated\ C++, out\, thirdparty\).
#   pwsh -File tools\release\make_dev_package.ps1
# Contents: the code-only tree GitHub gets (tools\release\publish_github.ps1's exclusions, kept identical below, plus
# the plans and prompts that list misses) with ANDROID_PLAN.md and docs\research\android_port_s62.md; the SDK is rebuilt
# by the helper from upstream + patches\rexglue-sdk\full (tools\sdk_patches\apply_series.ps1); a README_FIRST.md from
# tools\release\dev_package_README.md. Then a content check (refuses on any game-type file).
# Output: out\share\wwe2k14-dev-YYYYMMDD.zip (local; Jarrod sends it himself). No APK: ANDROID_PLAN 9.3(a) is open.
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $R
function Fail([string]$m) { "make_dev_package: $m"; exit 1 }
if ((git branch --show-current) -ne 'main') { Fail 'package from main (merge the session branch first)' }
$dirty = git status --porcelain | Where-Object { $_ -notmatch '^\?\? ' }
if ($dirty) { Fail "uncommitted changes to tracked files:`n$($dirty -join "`n")" }

$stamp = Get-Date -Format 'yyyyMMdd'
$share = "$R\out\share"
$stage = "$share\wwe2k14-dev-$stamp"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\wwe2k14-recomp" | Out-Null

# 1. Committed files of HEAD, minus GitHub's exclusions (same list as publish_github.ps1), plus the Android notes.
$exclude = @('^CLAUDE\.md$', '^PLAYBOOK\.md$', '^MORNING_REPORT\.md$', '^SESSION[^/]*\.md$', '^docs/(?!release/)',
             '^\.claude/', '^\.mcp\.json$', '^db/(?!ids/)', '^tools/labelling/prompts/', '^tools/release/release\.ps1$',
             '^[^/]+_PLAN\.md$', '^[^/]+_prompt\.md$')
# The Android notes the helper needs, kept although GitHub leaves them out.
$keepAnyway = @('ANDROID_PLAN.md', 'docs/research/android_port_s62.md')
$tar = "$share\head_$stamp.tar"
git archive --format=tar -o $tar HEAD
tar -xf $tar -C "$stage\wwe2k14-recomp"
Remove-Item $tar
$files = git ls-files
$dropped = 0
foreach ($p in $files) {
    if ($p -in $keepAnyway) { continue }
    if ($exclude | Where-Object { $p -match $_ }) {
        Remove-Item -LiteralPath "$stage\wwe2k14-recomp\$p" -Force -ErrorAction SilentlyContinue
        $dropped++
    }
}
Get-ChildItem "$stage\wwe2k14-recomp" -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (Get-ChildItem $_.FullName -Force) } | Remove-Item -Force
"files: $($files.Count) committed, $dropped left out (working notes), $(@($keepAnyway).Count) Android note(s) kept"

# 2. The patched SDK: no bundle (our checkout is a shallow clone); the helper rebuilds it from upstream v0.10.0 + the
# committed diff chain with tools\sdk_patches\apply_series.ps1 and checks the tree against this one.
$sdk = "$R\thirdparty\rexglue-sdk-src"
$sdkCommit = (git -C $sdk rev-parse --short wwe2k14-local)
$sdkTree = (git -C $sdk rev-parse 'wwe2k14-local^{tree}')
py "$R\tools\sdk_patches\export_series.py" --check 2>&1 | Select-Object -Last 1

# 3. README_FIRST.md
$readme = Get-Content "$R\tools\release\dev_package_README.md" -Raw
$readme = $readme.Replace('{{DATE}}', (Get-Date -Format 'yyyy-MM-dd HH:mm')).Replace('{{GAME_COMMIT}}', (git rev-parse --short HEAD)).Replace('{{SDK_COMMIT}}', $sdkCommit).Replace('{{SDK_TREE}}', $sdkTree)
Set-Content "$stage\README_FIRST.md" $readme -NoNewline

# 4. Content check: nothing from the game (CLAUDE.md rule 1), no keys, no third-party APKs.
$bad = Get-ChildItem $stage -Recurse -File | Where-Object {
    $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    $_.Extension -match '^\.(xex|pac|bik2|bik|iso|apk|aab|jks|keystore|p12|pem|xsh|xpso|xnps|png|jpg|dds|wav|xma)$' -or
    ($rel -match '/generated/' -and $rel -notmatch '/generated/rexglue\.cmake$') -or
    $rel -match '/(game_data|out|thirdparty)/'
}
# Committed PNGs are our own (exe icon, launcher logo: CLAUDE.md exceptions); anything else is refused.
$allowed = @('wwe2k14-recomp/game_logo.png', 'wwe2k14-recomp/assets/launcher/wwe2k14_logo.png')
$bad = @($bad | Where-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') -notin $allowed })
if ($bad.Count) { Fail "refused, game-type or private files in the package:`n$(($bad | ForEach-Object { $_.FullName }) -join "`n")" }
"content check: PASS (no game files, no generated C++ apart from generated\rexglue.cmake, no keys or APKs)"

# 5. Zip
$zip = "$share\wwe2k14-dev-$stamp.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
$h = (Get-FileHash $zip -Algorithm SHA256).Hash
"package: $zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, SHA-256 $h)"
"SDK: wwe2k14-local $sdkCommit; game: main $(git rev-parse --short HEAD). Send it yourself; nothing was uploaded."
