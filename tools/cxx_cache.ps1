# The shared compile cache for the TOOL builds (Session 103, lane H1): tools\scene\build_rsc / build_rig / build_maps / build_post_rig / ... compile
# our renderer sources (front_end.cpp etc.) into out\tools\*_obj; a fresh worktree used to compile all of them from scratch, and several checks
# rebuild overlapping sets. This helper makes those compiles go through sccache so a worktree and the main checkout (and every other worktree) share
# the objects, SAFELY. Dot-source it AFTER tools\env.ps1 and after $repo is set:
#     . (Join-Path $repo 'tools\cxx_cache.ps1')
#     $incRel = ConvertTo-CxxRel $inc                                      (the -I list with paths inside the repo made relative to it)
#     ...  & $using:cxx0 @using:cxxRest -O1 -g -std=c++20 -c @using:defs ... @using:incRel ($_.Substring($using:repoLen)) -o ($o.Substring($using:repoLen))
#     Show-CxxCache                                                        (one line: compiles / hits / misses since the dot-source)
# It sets $cxx0 / $cxxRest (the compiler command: `sccache clang++` or plain `clang++`), the process environment the sccache server needs, and makes the
# repo root the current directory (so the relative paths resolve; every build script uses absolute paths otherwise, so nothing else notices).
#
# SAFETY (why this is not the S95 poison; docs\LESSONS.md "sccache direct mode poisons builds across worktrees"):
#  - PREPROCESSOR mode (SCCACHE_DIRECT=false): the key is the preprocessed text of this very compile (every header's CONTENT as the preprocessor
#    resolved it), the full argument list and the compiler binary. Direct mode keys on a recorded header LIST and re-hashes only those files, so a
#    header added or shadowed in another worktree is invisible to it - that was the poison. Proof: tools\compile\cxx_cache_proof.py (lane h1_cache).
#  - Paths are RELATIVE to the repo root (cwd = the root): the preprocessed text and the arguments are the same in every worktree and the main
#    checkout, so they share hits without SCCACHE_BASEDIRS. That also means ONE server for everyone (a basedir is fixed when a server starts, and
#    servers that share a cache folder do not see each other's new entries until they restart: measured here, S103 H1). thirdparty\ (stb, vulkan
#    headers) lives only in the main checkout for a worktree, so it stays an absolute path in every tree (ConvertTo-CxxRel leaves it alone).
#  - One shared cache dir OUTSIDE every worktree, size capped. Objects restored from the cache carry the debug paths (CodeView) of the tree that
#    compiled them: cosmetic; code, data, relocations and symbols are identical to an uncached build (the proof compares them).
# Switches: WWE_NO_CCACHE=1 turns it off (plain clang++, nothing else changes); WWE_CCACHE_DIR (default %USERPROFILE%\.cache\wwe2k14-sccache);
# WWE_CCACHE_SIZE (default 10G). Not used by tools\owned\fast_build.ps1 (the game build keeps its own cache and port range).
$cxx0 = 'clang++'; $cxxRest = @(); $cxxCacheOn = $false; $script:cxxStats0 = $null; $script:cxxWhyOff = ''
$script:cxxRepoPrefix = ((Resolve-Path $repo).Path.TrimEnd('\') + '\')
Set-Location -LiteralPath $script:cxxRepoPrefix.TrimEnd('\')
function ConvertTo-CxxRel([string[]]$a) {
  $r = $script:cxxRepoPrefix; $keep = $r + 'thirdparty\'
  @($a | ForEach-Object { if ($_.StartsWith($r, 'OrdinalIgnoreCase') -and -not $_.StartsWith($keep, 'OrdinalIgnoreCase')) { $_.Substring($r.Length) } else { $_ } })
}
function Get-CxxCacheStats {
  $t = @(& $script:cxxSccache --show-stats 2>&1 | ForEach-Object { "$_" })
  $g = { param($p) $m = $t | Select-String -Pattern $p | Select-Object -First 1; if ($m) { [int64]$m.Matches[0].Groups[1].Value } else { 0 } }
  @{ requests = (& $g '^Compile requests executed\s+(\d+)'); hits = (& $g '^Cache hits\s+(\d+)'); misses = (& $g '^Cache misses\s+(\d+)') }
}
function Show-CxxCache {
  if (-not $script:cxxCacheOn) { Write-Host "[cxx-cache] off ($($script:cxxWhyOff))"; return }
  $n = Get-CxxCacheStats; $b = $script:cxxStats0
  $req = $n.requests - $b.requests; $h = $n.hits - $b.hits; $m = $n.misses - $b.misses
  $rate = if ($h + $m -gt 0) { '{0:N0}%' -f (100.0 * $h / ($h + $m)) } else { 'n/a' }
  Write-Host "[cxx-cache] on: $req compiles, $h hits, $m misses (hit rate $rate); cache $($env:SCCACHE_DIR) (cap $($env:SCCACHE_CACHE_SIZE)); WWE_NO_CCACHE=1 turns it off"
}
if ($env:WWE_NO_CCACHE -and $env:WWE_NO_CCACHE -ne '0') { $script:cxxWhyOff = 'WWE_NO_CCACHE=1' }
else {
  $cxCmd = Get-Command sccache -ErrorAction SilentlyContinue
  $script:cxxSccache = if ($cxCmd) { $cxCmd.Source } elseif (Test-Path "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe") { "$env:LOCALAPPDATA\Microsoft\WinGet\Links\sccache.exe" } else { '' }
  if (-not $script:cxxSccache) { $script:cxxWhyOff = 'sccache is not installed (winget install Mozilla.sccache); compiling without a cache' }
  else {
    $cxDir = if ($env:WWE_CCACHE_DIR) { $env:WWE_CCACHE_DIR } else { Join-Path $env:USERPROFILE '.cache\wwe2k14-sccache' }
    $cxSize = if ($env:WWE_CCACHE_SIZE) { $env:WWE_CCACHE_SIZE } else { '10G' }
    $cxVer = (& $script:cxxSccache --version 2>&1 | Select-Object -First 1)
    $cxDirect = if ($env:WWE_CCACHE_PROOF_DIRECT -eq '1') { 'true' } else { 'false' }   # builds are ALWAYS preprocessor mode and ignore an inherited SCCACHE_DIRECT; only the proof's one direct-mode arm sets WWE_CCACHE_PROOF_DIRECT
    $cxSha = [Security.Cryptography.SHA1]::Create()
    $cxHb = $cxSha.ComputeHash([Text.Encoding]::UTF8.GetBytes("$cxDir|$cxSize|$cxVer|$cxDirect|v3"))   # the whole configuration: the first port to try
    $cxPort0 = 4900 + ([BitConverter]::ToUInt32($cxHb, 0) % 500)
    New-Item -ItemType Directory -Force $cxDir | Out-Null
    $env:SCCACHE_DIR = $cxDir; $env:SCCACHE_CACHE_SIZE = $cxSize; $env:SCCACHE_DIRECT = $cxDirect
    Remove-Item Env:SCCACHE_BASEDIRS -ErrorAction SilentlyContinue
    # The server on a port may be someone else's (the proofs' temp-dir / direct-mode servers, another tool): attach only to one that is OURS = its cache location
    # is our dir, no base directories, and the mode line matches; else try the next port (4 tries), else compile uncached.
    $cxWant = if ($cxDirect -eq 'true') { 'yes' } else { 'no' }
    foreach ($cxTry in 0..3) {
      $cxPort = $cxPort0 + $cxTry; $env:SCCACHE_SERVER_PORT = [string]$cxPort
      $cxOwn = $env:WWE_SWEEP_OWNER; $env:WWE_SWEEP_OWNER = $null   # QTF: the long-lived shared server must NOT inherit the opt-in owner variable (proc_sweep would see it as an orphan)
      & $script:cxxSccache --start-server *> $null   # (fails when this exact server already runs: the probe below decides)
      if ($cxOwn) { $env:WWE_SWEEP_OWNER = $cxOwn }
      $cxSt = @(& $script:cxxSccache --show-stats 2>&1 | ForEach-Object { "$_" })
      if ($LASTEXITCODE -ne 0) { continue }
      $cxLoc = ($cxSt | Select-String -Pattern '^Cache location\s+(.*)$' | Select-Object -First 1)
      $cxLocT = if ($cxLoc) { ($cxLoc.Matches[0].Groups[1].Value -replace '\\\\', '\') } else { '' }
      $cxMode = ($cxSt | Select-String -Pattern '^Use direct/preprocessor mode\?\s+(\w+)' | Select-Object -First 1)
      $cxBase = ($cxSt | Select-String -Pattern '^Base directories\s+\(none\)' | Select-Object -First 1)
      if ($cxLocT.ToLower().Contains($cxDir.ToLower()) -and $cxBase -and (-not $cxMode -or $cxMode.Matches[0].Groups[1].Value -eq $cxWant)) { $script:cxxCacheOn = $true; break }
      if ($LASTEXITCODE -eq 0) { $script:cxxWhyOff = "the sccache server on port $cxPort is not ours (cache '$cxLocT'); compiling without a cache" }
    }
    if ($script:cxxCacheOn) { $cxx0 = $script:cxxSccache; $cxxRest = @('clang++'); $script:cxxStats0 = Get-CxxCacheStats }
    elseif (-not $script:cxxWhyOff) { $script:cxxWhyOff = "no sccache server of ours started on ports $cxPort0..$($cxPort0 + 3); compiling without a cache" }
  }
}

# Every compile through this helper passes -MMD (a .d file beside each object: the exact sources and non-system headers of THAT compile); the tool build scripts
# (tools\fview\build.ps1, tools\renderer\build.ps1) read them for their own staleness test.
$cxxRest = @($cxxRest) + @('-MMD')
