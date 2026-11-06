# Session 92 (Jarrod: "our folder has bloated significantly, what is safe to remove"): the approved disk clean-up
# (PLAYBOOK Quick start "Disk clean-up") as a script. NOTHING is deleted: everything goes to the Recycle Bin (Jarrod empties
# it), after a dry run that writes a manifest with sizes. What it takes (and only that):
#   A. merged SDK worktrees: every `git worktree` of thirdparty\rexglue-sdk-src under thirdparty\sdk-wt\ (or nested under
#      thirdparty\rexglue-sdk-src\thirdparty\sdk-wt\) whose branch is merged into wwe2k14-local, minus -Keep. Each must hold
#      no reparse point (the build junctions exist only while fast_build runs; a link left = skipped, never moved). After the
#      move: `git worktree prune`. (tools\bisect\wt_build.ps1 -Clean deletes permanently - this script never does.)
#   B. idle builder exe folders out\harness\bisect\exe\<name>\ (minus -Keep): the binaries (*.exe *.dll *.pdb), captures\
#      and logs\timeline_*.csv / logs\snap_*.bin - their run logs (wwe2k14_NNN.log, events, frames csv) stay: summaries cite them.
#   C. out\build\win-amd64-release\logs: timeline_*.csv older than 24 h and snap_*.bin.
# The Recycle Bin silently deletes what it has no room for, so the total is checked against the bin's free room (C:'s
# MaxCapacity minus what it holds) with a 10% margin before anything moves; over it = stop. Runs under a SHARED run-lock
# hold with -Go (a timed run's exclusive hold and this never overlap).
#   & .\tools\harness\disk_cleanup.ps1                       dry run: the manifest (out\cleanup\manifest_<time>.txt) + sizes
#   & .\tools\harness\disk_cleanup.ps1 -Go                   move the dry run's list to the Recycle Bin
#   -Keep 's92-m4fc,live_s92m4fc'                            names (worktree folder / exe folder) never touched
param([switch]$Go, [string]$Keep = 's92-m4fc,live_s92m4fc', [switch]$NoLock)
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\Jarrod\wwe2k14-recomp'
$sdk = "$root\thirdparty\rexglue-sdk-src"
$keepSet = @($Keep -split '[ ,]+' | Where-Object { $_ })
if ($Go -and -not $NoLock) {
  # Re-enter under a shared hold of the run lock (tools\runlock.ps1).
  & pwsh -NoProfile -File "$root\tools\runlock.ps1" run -Shared -Name disk_cleanup -- pwsh -NoProfile -File $PSCommandPath -Go -NoLock -Keep $Keep
  exit $LASTEXITCODE
}
Add-Type -AssemblyName Microsoft.VisualBasic
function SizeOf([string]$p) {
  if (Test-Path -LiteralPath $p -PathType Leaf) { return (Get-Item -LiteralPath $p).Length }
  $o = robocopy $p "$env:TEMP\__nul_measure" /L /S /NFL /NDL /NJH /BYTES /R:0 /W:0 /XJ 2>$null
  $line = $o | Where-Object { $_ -match '^\s*Bytes\s*:' } | Select-Object -First 1
  if ($line -match 'Bytes\s*:\s*(\d+)') { return [double]$Matches[1] } else { return 0 }
}
function HasLinks([string]$p) {
  if (-not (Test-Path -LiteralPath $p -PathType Container)) { return $false }
  return [bool](Get-ChildItem -LiteralPath $p -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | Select-Object -First 1)
}
$items = New-Object System.Collections.Generic.List[object]
function Add([string]$group, [string]$path, [string]$why) { $items.Add([pscustomobject]@{ Group = $group; Path = $path; Why = $why; Bytes = (SizeOf $path) }) }

# A. merged SDK worktrees
$merged = @(git -C $sdk branch --merged wwe2k14-local --format '%(refname:short)')
$wt = git -C $sdk worktree list --porcelain
$cur = @{}
$list = @()
foreach ($l in $wt) {
  if ($l -like 'worktree *') { $cur = @{ path = $l.Substring(9).Replace('/', '\') } }
  elseif ($l -like 'branch *') { $cur.branch = $l.Substring(7) -replace '^refs/heads/', '' }
  elseif ($l -eq '' -and $cur.path) { $list += [pscustomobject]$cur; $cur = @{} }
}
if ($cur.path) { $list += [pscustomobject]$cur }
foreach ($w in $list) {
  $p = $w.path
  if ($p -notmatch '\\sdk-wt\\') { continue }
  $leaf = Split-Path $p -Leaf
  if ($keepSet -contains $leaf) { continue }
  if (-not $w.branch -or $merged -notcontains $w.branch) { continue }
  if (HasLinks $p) { "SKIP (holds a link - run its build to the end or wt_build.ps1 -Clean -DryRun): $p"; continue }
  $dirty = @(git -C $p status --porcelain --untracked-files=no 2>$null)
  if ($dirty.Count) { "SKIP (uncommitted changes to tracked files: $($dirty.Count)): $p"; continue }
  Add 'A merged SDK worktree' $p "branch $($w.branch) merged into wwe2k14-local"
}
# B. idle builder exe folders
foreach ($d in Get-ChildItem -LiteralPath "$root\out\harness\bisect\exe" -Directory -ErrorAction SilentlyContinue) {
  if ($keepSet -contains $d.Name) { continue }
  foreach ($f in Get-ChildItem -LiteralPath $d.FullName -File -Include *.exe, *.dll, *.pdb -ErrorAction SilentlyContinue) { Add 'B builder exe' $f.FullName 'binary' }
  foreach ($f in Get-ChildItem -LiteralPath $d.FullName -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.exe', '.dll', '.pdb' }) {
    if (-not ($items | Where-Object Path -eq $f.FullName)) { Add 'B builder exe' $f.FullName 'binary' }
  }
  if (Test-Path -LiteralPath "$($d.FullName)\captures") {
    if (HasLinks "$($d.FullName)\captures") { "SKIP (link): $($d.FullName)\captures" } else { Add 'B builder exe' "$($d.FullName)\captures" 'captures' }
  }
  foreach ($f in Get-ChildItem -LiteralPath "$($d.FullName)\logs" -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'timeline_*.csv' -or $_.Name -like 'snap_*.bin' }) {
    Add 'B builder exe' $f.FullName 'timeline / snapshot'
  }
}
# C. the main build's old timelines and snapshots
$cut = (Get-Date).AddHours(-24)
foreach ($f in Get-ChildItem -LiteralPath "$root\out\build\win-amd64-release\logs" -File -ErrorAction SilentlyContinue) {
  if (($f.Name -like 'timeline_*.csv' -and $f.LastWriteTime -lt $cut) -or $f.Name -like 'snap_*.bin') { Add 'C main logs' $f.FullName 'old timeline / snapshot' }
}

$total = ($items | Measure-Object Bytes -Sum).Sum
$dir = "$root\out\cleanup"
New-Item -ItemType Directory -Force $dir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$man = "$dir\manifest_$stamp$(if ($Go) { '_moved' } else { '_dryrun' }).txt"
$by = $items | Group-Object Group | ForEach-Object { '{0}: {1} items, {2:N1} GB' -f $_.Name, $_.Count, (($_.Group | Measure-Object Bytes -Sum).Sum / 1GB) }
@("disk_cleanup $(if ($Go) { 'MOVE to the Recycle Bin' } else { 'DRY RUN' }) $(Get-Date -Format 'yyyy-MM-dd HH:mm'), keep: $Keep", $by, ('total {0:N1} GB' -f ($total / 1GB)), '') +
  ($items | Sort-Object Group, Path | ForEach-Object { '{0,8:N2} GB  {1}  ({2})' -f ($_.Bytes / 1GB), $_.Path.Replace("$root\", ''), $_.Why }) | Set-Content -LiteralPath $man
$by
'total {0:N1} GB in {1} items; manifest {2}' -f ($total / 1GB), $items.Count, $man.Replace("$root\", '')
if (-not $Go) { 'dry run: nothing moved. Move with: & .\tools\harness\disk_cleanup.ps1 -Go'; exit 0 }

# The Recycle Bin's room on C: (MaxCapacity MB of the volume, minus what it holds now), 10% margin.
$guid = ((Get-Volume -DriveLetter C).UniqueId -replace '.*Volume(\{[^}]+\}).*', '$1')
$cap = (Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\BitBucket\Volume\$guid" -ErrorAction SilentlyContinue).MaxCapacity
if (-not $cap) { 'STOP: the Recycle Bin capacity of C: could not be read - nothing moved'; exit 4 }
$held = SizeOf 'C:\$Recycle.Bin'
$room = [double]$cap * 1MB - $held
if ($total -gt 0.9 * $room) { 'STOP: {0:N1} GB to move, the Recycle Bin has room for {1:N1} GB (with 10% margin) - empty it first; nothing moved' -f ($total / 1GB), (0.9 * $room / 1GB); exit 5 }
$moved = 0
foreach ($it in $items) {
  try {
    if (Test-Path -LiteralPath $it.Path -PathType Container) {
      [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($it.Path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } elseif (Test-Path -LiteralPath $it.Path) {
      [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($it.Path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
    $moved += $it.Bytes
  } catch { "FAILED: $($it.Path): $($_.Exception.Message)" }
}
git -C $sdk worktree prune 2>&1 | ForEach-Object { "  git worktree prune: $_" }
'moved {0:N1} GB to the Recycle Bin (empty it to free the space); manifest {1}' -f ($moved / 1GB), $man.Replace("$root\", '')
