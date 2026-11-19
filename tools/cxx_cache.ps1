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
#    header added or shadowed in another worktree is invisible to it - that was the poison. Proof: tools\gate\cxx_cache_proof.py (lane h1_cache).
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

# ---- S103 lane DIB: dependency manifests (the compiler's own list of what each exe was built from) ------------------------------------------------------
# Every compile through this helper passes -MMD (a .d file beside each object: the exact sources and non-system headers, with the -D / -I of THAT compile).
# At each link the build script's `clang++` call (shadowed below) writes <exe>.deps.txt and adds the exe's files to out\deps\<build script>[@flags].deps.txt;
# tools\gate\deps.py reads them: the gate's tool cache and proof key use their CONTENT instead of "all of src\ + tools\". A build that cannot name its inputs
# (an object without a .d, a source compiled and linked in one call, a generated header under out\, a link input that is not a .o, a generator run other than
# compile_shaders.py, a failed link, a manifest write that threw) writes an INVALID manifest and the gate falls back to the old whole-tree key.
# S103 DIB2 (review of DIB): the script id carries the build arguments (-Release -> `...build.ps1@release`: dev and release no longer share one manifest); every
# entry records the file's sha1 (`path<TAB>sha1`: a reader whose tree differs from the build's treats the manifest as stale); manifests are written atomically (temp +
# Move-Item) and only when their content changes; <exe>.deps.txt is deleted before every link (a failed link leaves none); compile_shaders.py and the shader sources
# are inputs of any build that runs the generator. WWE_NO_DEPS=1 turns the manifests off. Format and soundness: tools\gate\deps.py.
$cxxRest = @($cxxRest) + @('-MMD')
$script:cxxHelperPath = $PSCommandPath
$script:cxxScriptId = ''; $script:cxxCaller = ''
$script:cxxDepsDir = Join-Path $script:cxxRepoPrefix 'out\deps'
$script:cxxRealClang = (Get-Command 'clang++' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not ($env:WWE_NO_DEPS -and $env:WWE_NO_DEPS -ne '0')) {
  $cxCs = @(Get-PSCallStack)
  if ($cxCs.Count -gt 1 -and $cxCs[1].ScriptName -and $cxCs[1].ScriptName.StartsWith($script:cxxRepoPrefix, 'OrdinalIgnoreCase')) {
    $script:cxxCaller = $cxCs[1].ScriptName
    $cxTag = (@($cxCs[1].InvocationInfo.BoundParameters.Keys | ForEach-Object { "$_".ToLower() } | Sort-Object) -join '+')
    $script:cxxScriptId = ($script:cxxCaller.Substring($script:cxxRepoPrefix.Length) -replace '[\\/]', '_') + $(if ($cxTag) { '@' + $cxTag } else { '' })
    New-Item -ItemType Directory -Force $script:cxxDepsDir | Out-Null
    Remove-Item -LiteralPath (Join-Path $script:cxxDepsDir ($script:cxxScriptId + '.deps.txt')) -Force -ErrorAction SilentlyContinue   # this run rewrites it
  }
}
function Get-CxxDepFull([string]$p) {
  [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $script:cxxRepoPrefix $p }))
}
function ConvertTo-CxxDepEntry([string]$p) {
  # one path from a .d file (or an extra input) -> a manifest entry: repo-relative posix, thirdparty/..., abs:..., or '!out' for a generated file under out\
  $full = Get-CxxDepFull $p
  $rel = $null
  if ($full.StartsWith($script:cxxRepoPrefix, 'OrdinalIgnoreCase')) { $rel = $full.Substring($script:cxxRepoPrefix.Length).Replace('\', '/') }
  else { $i = $full.ToLower().IndexOf('\thirdparty\'); if ($i -ge 0) { $rel = $full.Substring($i + 1).Replace('\', '/') } }
  if ($rel) {
    if ($rel -like 'out/*') { return '!out' }
    return $rel
  }
  return 'abs:' + $full.Replace('\', '/')
}
function Get-CxxSha1([string]$full) {
  try {
    $h = [Security.Cryptography.SHA1]::Create(); $fs = [IO.File]::OpenRead($full)
    try { return ([BitConverter]::ToString($h.ComputeHash($fs)) -replace '-', '').ToLower() } finally { $fs.Dispose(); $h.Dispose() }
  } catch { return '' }
}
function Write-CxxManifestFile([string]$path, $items) {
  # items: entry -> sha1 ('' = none). Atomic (temp + Move-Item) and only when the content changed (a rewrite with the same content moves nobody's stamp).
  $text = '# wwe2k14-deps v1' + "`n" + ((@($items.Keys | Sort-Object) | ForEach-Object { if ($items[$_]) { "$_`t$($items[$_])" } else { $_ } }) -join "`n") + "`n"
  if (Test-Path -LiteralPath $path) { if ([IO.File]::ReadAllText($path) -ceq $text) { return } }
  $tmp = $path + '.' + $PID + '.tmp'
  [IO.File]::WriteAllText($tmp, $text)
  Move-Item -LiteralPath $tmp -Destination $path -Force
}
function Set-CxxDepsInvalid([string]$why) {
  if (-not $script:cxxScriptId) { return }
  try {
    $p = Join-Path $script:cxxDepsDir ($script:cxxScriptId + '.deps.txt'); $tmp = $p + '.' + $PID + '.tmp'
    [IO.File]::WriteAllText($tmp, "# wwe2k14-deps INVALID: $why`n"); Move-Item -LiteralPath $tmp -Destination $p -Force
  } catch { }
}
function Get-CxxGeneratorExtras {
  # the build script's own text decides: it runs compile_shaders.py (its inputs: the generator and every shader source, not the spv\ headers it writes), or it
  # runs some other `& py ...` generator (INVALID: the manifest cannot name what that reads)
  $txt = ''
  try { $txt = Get-Content -Raw -LiteralPath $script:cxxCaller } catch { }
  $extra = @(); $why = $null
  foreach ($m in [regex]::Matches($txt, '(?m)^[^#\r\n]*?&\s*(?:py|python)(?:\.exe)?\s+([^\r\n]*)')) {
    if ($m.Groups[1].Value -match 'compile_shaders\.py') {
      $extra += (Join-Path $script:cxxRepoPrefix 'tools\renderer\compile_shaders.py')
      $sh = Join-Path $script:cxxRepoPrefix 'src\renderer\shaders'
      if (Test-Path -LiteralPath $sh) { $extra += @(Get-ChildItem -LiteralPath $sh -Recurse -File | Where-Object { $_.FullName -notlike '*\shaders\spv\*' } | ForEach-Object { $_.FullName }) }
    } else { $why = "the build script runs a generator ($($m.Groups[1].Value.Trim())) whose inputs a manifest cannot name" }
  }
  return @{ extra = $extra; why = $why }
}
function Write-CxxDeps {
  param([string]$Exe, [string[]]$Objects, [string[]]$Extra = @())
  if (-not $script:cxxScriptId) { return }
  $exeMan = $Exe + '.deps.txt'
  try {
    $map = @{}; $why = $null
    foreach ($o in $Objects) {
      $d = [IO.Path]::ChangeExtension($o, '.d')
      if (-not (Test-Path -LiteralPath $d)) { $why = "no .d file for $o (compiled before manifests existed: delete it to recompile)"; break }
      $list = (Get-Content -Raw -LiteralPath $d) -replace '\\\r?\n', ' '
      $ix = $list.IndexOf(': '); if ($ix -lt 0) { $why = "unreadable .d for $o"; break }
      foreach ($item in (($list.Substring($ix + 2)) -split '(?<!\\)\s+')) {
        $path = ($item -replace '\\ ', ' ').Trim()
        if ($path) { $e = ConvertTo-CxxDepEntry $path; $map[$e] = Get-CxxDepFull $path }
      }
    }
    if (-not $why) {
      $gen = Get-CxxGeneratorExtras
      if ($gen.why) { $why = $gen.why }
      foreach ($x in (@($Extra) + @($gen.extra) + @($script:cxxCaller, $script:cxxHelperPath, (Join-Path $script:cxxRepoPrefix 'tools\env.ps1')))) { if ($x) { $map[(ConvertTo-CxxDepEntry $x)] = Get-CxxDepFull $x } }
      if ($map.ContainsKey('!out')) { $why = 'an input is a generated file under out\' }
    }
    if ($why) {
      Remove-Item -LiteralPath $exeMan -Force -ErrorAction SilentlyContinue
      Set-CxxDepsInvalid "$why (exe $Exe)"
      return
    }
    $items = @{}
    foreach ($k in $map.Keys) { $items[$k] = if ($k.StartsWith('abs:')) { '' } else { Get-CxxSha1 $map[$k] } }
    Write-CxxManifestFile $exeMan $items
    $sman = Join-Path $script:cxxDepsDir ($script:cxxScriptId + '.deps.txt')
    if (Test-Path -LiteralPath $sman) {
      $old = @(Get-Content -LiteralPath $sman)
      if ($old.Count -and $old[0] -ne '# wwe2k14-deps v1') { return }   # sticky INVALID: one exe of this script could not be named
      foreach ($l in ($old | Select-Object -Skip 1)) { if ($l) { $e, $h = $l -split "`t", 2; if (-not $items.ContainsKey($e)) { $items[$e] = $h } } }
    }
    Write-CxxManifestFile $sman $items
  } catch {
    Remove-Item -LiteralPath $exeMan -Force -ErrorAction SilentlyContinue
    Set-CxxDepsInvalid "the manifest could not be written ($($_.Exception.Message)) (exe $Exe)"
  }
}
function clang++ {
  # shadows clang++ for the build script that dot-sourced this helper: a LINK (no -c / -E / -S) writes the dependency manifests; everything passes through
  $a = @($args)
  $isLink = [bool]$script:cxxScriptId -and -not ($a | Where-Object { $_ -in @('-c', '-E', '-S', '-fsyntax-only') })
  $exe = $null
  # a reproducible link (lld /Brepro: no link time stamp, the debug GUID is a hash of the content): the same objects give the SAME exe bytes, so the gate, which keys a check on the
  # BYTES of the exes it runs (S103 DIB2 bug 1), does not miss on a relink that changed nothing (build_rig.ps1 relinks frame_rig.exe on every run)
  if ($isLink -and ($a -contains '-fuse-ld=lld') -and -not ($a | Where-Object { "$_" -like '*Brepro*' })) { $a += '-Wl,/Brepro' }
  if ($isLink) {
    $oi = [Array]::IndexOf($a, '-o')
    if ($oi -ge 0 -and $oi + 1 -lt $a.Count) {
      $exe = Get-CxxDepFull ([string]$a[$oi + 1])
      Remove-Item -LiteralPath ($exe + '.deps.txt') -Force -ErrorAction SilentlyContinue   # a failed or unparsable link must not leave the manifest of the exe it replaces
    }
  }
  & $script:cxxRealClang @a
  $rc = $LASTEXITCODE
  if ($script:cxxScriptId -and $rc -ne 0) { Set-CxxDepsInvalid "a clang++ call failed (exit $rc)" }
  elseif ($isLink) {
    if (-not $exe) { Set-CxxDepsInvalid 'a link without -o' }
    else {
      $objs = @($a | Where-Object { $_ -is [string] -and $_ -like '*.o' })
      $srcs = @($a | Where-Object { $_ -is [string] -and $_ -match '\.(cpp|cc|c)$' })
      $other = @($a | Where-Object { $_ -is [string] -and ($_ -match '\.(res|obj|rsp)$' -or $_.StartsWith('@') -or ($_ -match '\.(lib|a)$' -and (Get-CxxDepFull $_).StartsWith($script:cxxRepoPrefix, 'OrdinalIgnoreCase'))) })
      if ($srcs.Count) { Set-CxxDepsInvalid "a source linked directly in one call (exe $exe)" }
      elseif ($other.Count) { Set-CxxDepsInvalid "a link input that is not a .o ($($other[0])): its inputs are not named (exe $exe)" }
      elseif (-not $objs.Count) { Set-CxxDepsInvalid "a link with no .o inputs (exe $exe)" }
      else { Write-CxxDeps -Exe $exe -Objects $objs }
    }
  }
  $global:LASTEXITCODE = $rc
}
