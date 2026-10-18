# Session 31 (Jarrod): publish the code to GitHub, without our working notes.
#
# Local `main` keeps everything (CLAUDE.md, PLAYBOOK.md, SESSION*.md, docs\, the function database ...) exactly as
# before. This script builds a clean copy of main's committed tree in a temporary index (the working folder is never
# touched), checks it, and commits it on the local branch `public`, which is what GitHub's `main` gets:
#   - left out: CLAUDE.md, PLAYBOOK.md, MORNING_REPORT.md, SESSION*.md, docs\ (except docs\release\), .claude\,
#     .mcp.json, db\ (except db\ids\), tools\labelling\prompts\;
#   - refused: game files by extension (.pac .bik .bik2 .xex .xexp .iso .cac), symbols (.pdb .map), anything under
#     game_data\ or out\, generated\ other than generated\rexglue.cmake, and any file whose bytes equal a file under
#     game_data\ (same-size files hashed, as make_release.ps1 does);
#   - the commit is authored by the git identity configured here (Jarrod), message as given, no co-author line
#     (Jarrod: GitHub history is his alone).
#
#   pwsh -NoProfile -File tools\release\publish_github.ps1                  dry run: build + check the tree, list it
#   pwsh -NoProfile -File tools\release\publish_github.ps1 -Commit -Message "..."           + commit on `public`
#   ... -Commit -Fresh                     the commit has no parent: a new history (the first publish, Session 31)
#   ... -Push                              push `public` to origin main (with -Fresh: --force, replacing GitHub's
#                                          history). Only with Jarrod's OK (CLAUDE.md Safety).
param([switch]$Commit, [switch]$Fresh, [switch]$Push, [string]$Message = '', [string]$Remote = 'origin',
      [string]$Branch = 'main', [string]$ListFile = '')
$ErrorActionPreference = 'Stop'
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $R
function Fail([string]$m) { Write-Host "publish_github: FAIL: $m"; exit 1 }

if (git status --porcelain --untracked-files=no) { Fail 'uncommitted changes to tracked files: commit first' }
$head = (git rev-parse HEAD).Trim()

# Which files stay out (regexes on the repo path, forward slashes).
$exclude = @('^CLAUDE\.md$', '^PLAYBOOK\.md$', '^MORNING_REPORT\.md$', '^SESSION[^/]*\.md$', '^docs/(?!release/)',
             '^\.claude/', '^\.mcp\.json$', '^db/(?!ids/)',
             '^tools/labelling/prompts/')  # the AI labelling prompts: notes, not code (Jarrod, Session 31)
$all = git ls-tree -r --name-only $head
$drop = @($all | Where-Object { $p = $_; $exclude | Where-Object { $p -match $_ } })
$keep = @($all | Where-Object { $drop -notcontains $_ })

# The tree, through a temporary index (the real index and the working folder stay as they are).
$tmp = Join-Path $env:TEMP "wwe2k14_publish_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$index = Join-Path $tmp 'index'
$env:GIT_INDEX_FILE = $index
try {
  git read-tree $head
  if ($drop.Count) {
    $listPath = Join-Path $tmp 'drop.txt'
    [IO.File]::WriteAllLines($listPath, [string[]]$drop)
    git rm --cached -q --pathspec-from-file=$listPath | Out-Null
  }
  $tree = (git write-tree).Trim()
} finally {
  Remove-Item Env:GIT_INDEX_FILE
}
Remove-Item -Recurse -Force $tmp

# Checks.
$files = git ls-tree -r -l $tree | ForEach-Object {
  $f = $_ -split "`t", 2; $meta = $f[0] -split '\s+'
  [pscustomobject]@{ Path = $f[1]; Blob = $meta[2]; Size = [int64]$meta[3] }
}
$bad = @($files | Where-Object {
  $_.Path -match '\.(pac|bik|bik2|xex|xexp|iso|cac|pdb|map)$' -or $_.Path -match '^(game_data|out)/' -or
  ($_.Path -match '^generated/' -and $_.Path -ne 'generated/rexglue.cmake') })
if ($bad.Count) { Fail ("forbidden paths: " + (($bad | Select-Object -First 10).Path -join ', ')) }
$gameFiles = @(Get-ChildItem -Recurse -File -Path (Join-Path $R 'game_data') -ErrorAction SilentlyContinue)
$bySize = @{}
foreach ($g in $gameFiles) { if (-not $bySize.ContainsKey($g.Length)) { $bySize[$g.Length] = @() }; $bySize[$g.Length] += $g.FullName }
$hashed = 0
foreach ($f in $files) {
  if ($f.Size -le 0 -or -not $bySize.ContainsKey($f.Size)) { continue }
  $blobTmp = [IO.Path]::GetTempFileName()
  cmd /c "git cat-file blob $($f.Blob) > `"$blobTmp`""
  $h = (Get-FileHash $blobTmp -Algorithm SHA256).Hash
  Remove-Item $blobTmp
  foreach ($g in $bySize[$f.Size]) {
    $hashed++
    if ((Get-FileHash $g -Algorithm SHA256).Hash -eq $h) { Fail "$($f.Path) has the same bytes as $g" }
  }
}
$bytes = ($files | Measure-Object Size -Sum).Sum
$summary = ("publish_github: tree {0} from main {1}: {2} files ({3:N1} MB), {4} left out; game-file scan: {5} game " +
            "files by size, {6} hashed, 0 matches") -f $tree.Substring(0, 12), $head.Substring(0, 7), $files.Count,
            ($bytes / 1MB), $drop.Count, $gameFiles.Count, $hashed
Write-Host $summary
if ($ListFile) { [IO.File]::WriteAllLines($ListFile, [string[]]($files.Path)); Write-Host "publish_github: file list -> $ListFile" }

if (-not $Commit) { Write-Host 'publish_github: dry run (no commit, no push)'; exit 0 }
if (-not $Message) { $Message = 'WWE 2K14 native PC recompilation ' + (Get-Content (Join-Path $R 'VERSION') -TotalCount 1).Trim() }
$parent = ''
if (-not $Fresh) {
  $parent = (git rev-parse --verify --quiet refs/heads/public)
  if (-not $parent) { Fail 'no `public` branch yet: the first publish needs -Fresh' }
}
$commitArgs = @('commit-tree', $tree, '-m', $Message)
if ($parent) { $commitArgs += @('-p', $parent.Trim()) }
$c = (& git @commitArgs).Trim()
git update-ref refs/heads/public $c
Write-Host "publish_github: commit $($c.Substring(0, 12)) on branch public$(if ($Fresh) { ' (new history)' } else { '' })"

if (-not $Push) { Write-Host "publish_github: not pushed (add -Push)"; exit 0 }
$pushArgs = @('push', $Remote, "public:$Branch")
if ($Fresh) { $pushArgs = @('push', '--force', $Remote, "public:$Branch") }
& git @pushArgs
if ($LASTEXITCODE) { Fail "git push failed ($LASTEXITCODE)" }
Write-Host "publish_github: pushed public -> $Remote/$Branch"
