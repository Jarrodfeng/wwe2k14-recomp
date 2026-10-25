# Session 65: rebuild our patched ReXGlue SDK from public upstream + patches\rexglue-sdk\full\*.diff (for a helper
# without our SDK checkout). Same method as export_series.py's verify: upstream v0.10.0 (f5337cd) in a temporary index,
# every diff applied in order with `git apply --cached --binary`, the tree written, a commit made on branch
# wwe2k14-local, checked against the expected tree, then checked out (+ submodules from their public repositories).
#   pwsh -File tools\sdk_patches\apply_series.ps1 [-Sdk DIR] [-ExpectTree HASH] [-NoSubmodules]
# -Sdk defaults to thirdparty\rexglue-sdk-src (cloned from GitHub at v0.10.0 if missing). -ExpectTree: the tree hash
# printed by export_series.py's PASS line (full hash in the developer package's README_FIRST.md); a mismatch fails.
param([string]$Sdk = '', [string]$ExpectTree = '', [switch]$NoSubmodules)
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Sdk) { $Sdk = "$R\thirdparty\rexglue-sdk-src" }
$base = 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2'   # upstream rexglue/rexglue-sdk tag v0.10.0
if (-not (Test-Path "$Sdk\.git")) {
    git clone --quiet --depth 1 --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git $Sdk
    if ($LASTEXITCODE) { throw 'clone of upstream rexglue-sdk v0.10.0 failed' }
}
$diffs = Get-ChildItem "$R\patches\rexglue-sdk\full" -Filter '*.diff' | Sort-Object Name
$tmp = New-Item -ItemType Directory -Force (Join-Path ([IO.Path]::GetTempPath()) "apply_series_$PID")
$env:GIT_INDEX_FILE = "$tmp\index"
try {
    git -C $Sdk read-tree $base
    if ($LASTEXITCODE) { throw "base $base not in $Sdk" }
    foreach ($d in $diffs) {
        git -C $Sdk apply --cached --binary $d.FullName
        if ($LASTEXITCODE) { throw "$($d.Name) does not apply" }
    }
    $tree = (git -C $Sdk write-tree).Trim()
} finally {
    Remove-Item env:GIT_INDEX_FILE
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
"applied $($diffs.Count) diffs on v0.10.0: tree $tree"
if ($ExpectTree -and -not $tree.StartsWith($ExpectTree)) { throw "tree $tree is not the expected $ExpectTree" }
# A fixed author for this generated commit (the machine may have no git identity set).
$env:GIT_AUTHOR_NAME = $env:GIT_COMMITTER_NAME = 'wwe2k14-recomp apply_series'
$env:GIT_AUTHOR_EMAIL = $env:GIT_COMMITTER_EMAIL = 'apply-series@localhost'
$commit = "$(git -C $Sdk commit-tree $tree -p $base -m "wwe2k14 patches: v0.10.0 + $($diffs.Count) steps (patches\rexglue-sdk\full)")".Trim()
if (-not $commit) { throw 'commit-tree failed' }
git -C $Sdk branch -f wwe2k14-local $commit
git -C $Sdk checkout --quiet -f wwe2k14-local
"branch wwe2k14-local = $commit (tree $tree), checked out in $Sdk"
if (-not $NoSubmodules) {
    git -C $Sdk submodule update --init --recursive
    if ($LASTEXITCODE) { throw 'submodule update failed' }
    'submodules: done'
}
