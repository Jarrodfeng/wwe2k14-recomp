# Shared by the wrestler tool build scripts (S101 lane 239). Why: the gate lanes wrestlers_ours and pieces_ours run in parallel and both build
# out\tools\wrestler_ours_check.exe (and pieces also classify_unit.exe) in place; the other lane's CPU half has the exe running for minutes, so the
# linker's write hit "unable to remove file: permission denied" and the lane failed on BUILD. The fix, three parts:
#   1. one named mutex per exe serialises the build step across processes (Enter-ExeBuild / Exit-ExeBuild);
#   2. a build that finds the exe newer than every source and this script does nothing (Test-ExeFresh): the second lane normally never links;
#   3. the build links to a private temp name and Publish-Exe moves it over the target; if the target is open in a running process (a running exe
#      cannot be overwritten or deleted but CAN be renamed), the old file is renamed aside first, so a running copy keeps going and a new run gets the new exe.
function Test-ExeFresh([string]$Exe, [string[]]$Sources) {
  if (-not (Test-Path $Exe)) { return $false }
  $t = (Get-Item $Exe).LastWriteTimeUtc
  foreach ($s in $Sources) { if ((Get-Item $s).LastWriteTimeUtc -gt $t) { return $false } }
  return $true
}
function Get-BuildSources([string]$Repo, [string[]]$Extra) {
  $files = @(Get-ChildItem -Path (Join-Path $Repo 'src') -Recurse -File -Include *.h, *.cpp, *.inl | ForEach-Object { $_.FullName })
  return $files + $Extra + @((Join-Path $PSScriptRoot 'atomic_build.ps1'))
}
function Enter-ExeBuild([string]$Exe) {
  $name = 'Global\wwe2k14_build_' + (($Exe.ToLower() -replace '[^a-z0-9]', '_'))
  $m = New-Object System.Threading.Mutex($false, $name)
  try { [void]$m.WaitOne([TimeSpan]::FromMinutes(40)) } catch [System.Threading.AbandonedMutexException] { }
  return $m
}
function Exit-ExeBuild($Mutex) { try { $Mutex.ReleaseMutex() } catch { }; $Mutex.Dispose() }
function Publish-Exe([string]$Tmp, [string]$Exe) {
  Get-ChildItem -Path ($Exe + '.old-*') -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }   # leftovers whose processes are gone
  try { Move-Item -Force $Tmp $Exe -ErrorAction Stop; return } catch { }
  $aside = $Exe + '.old-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
  Rename-Item $Exe $aside -ErrorAction Stop   # a running exe can be renamed
  Move-Item -Force $Tmp $Exe -ErrorAction Stop
  Remove-Item $aside -Force -ErrorAction SilentlyContinue   # still open: stays, the next build removes it
}
