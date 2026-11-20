# "Today's commands exactly" (lane CQW; docs\agents\admit_wiring.md): with WWE_NO_ADMIT=1 every wired site gives the parallel count and the compile
# command lines of the commit before the wiring, byte for byte. Reads only (git show + the PowerShell parser); runs no build, no admit.
#   pwsh -NoProfile -File tools\admit\wiring_dry.ps1 [-Base SHA]      (default: the parent of the commit that added tools\admit\Admit.ps1)
# Prints one dry line per tool-build script and tree (BASE / HEAD): the parallel count the loop gets (WWE_RIG_JOBS unset and =3), the sha of the -Parallel
# block's code (its tokens, comments dropped: the compile command lines), the sha of the rest of the script's code with the wiring taken back out (the
# dot-source of Admit.ps1, the Request-BuildParallel around the count, the try / finally Release-BuildParallel around the loop). BASE and HEAD lines must be equal. For the Python pools: every pool line of
# BASE must be a `with _admit.hold(<kind>, <the base's count>) as n, Pool(n)` line in HEAD (with WWE_NO_ADMIT=1 hold yields the count:
# tools\admit\wiring_test.py Y3); only the pool lines are compared (2026-11-20), so a later edit elsewhere in those files does not fail this check.
# -Plant: HEAD's build_rsc count and one renderer compile line are changed in memory: both must FAIL (exit 1), or the check is blind.
param([string]$Base, [switch]$Plant)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Base) {
  $add = @(& git -C $repo log --diff-filter=A --format=%H -- tools/admit/Admit.ps1)
  if (-not $add) { throw 'cannot find the commit that added tools/admit/Admit.ps1 (pass -Base)' }
  $Base = (& git -C $repo rev-parse "$($add[-1])^").Trim()
}
$env:WWE_NO_ADMIT = '1'
Remove-Item Env:WWE_BUILD_PARALLEL -ErrorAction SilentlyContinue
. (Join-Path $PSScriptRoot 'Admit.ps1')
$table = Get-Content -Raw (Join-Path $PSScriptRoot 'tool_build_parallel.json') | ConvertFrom-Json
$scripts = @($table.scripts.PSObject.Properties | Where-Object { $_.Value.wired } | ForEach-Object { $_.Name })   # every wired build script (the generated table)

# the three fall-open modes the counts are read in: off, no exe, an exe that fails (exit 3, no output)
$failCmd = Join-Path ([IO.Path]::GetTempPath()) ("cqw_dry_fail_{0}.cmd" -f $PID)
Set-Content -LiteralPath $failCmd -Value '@exit /b 3' -Encoding ascii
$modes = @(@{ name = 'off'; noadmit = $true; exe = $null },
           @{ name = 'noexe'; noadmit = $false; exe = (Join-Path ([IO.Path]::GetTempPath()) 'cqw_dry_no_such_admit.exe') },
           @{ name = 'failexe'; noadmit = $false; exe = $failCmd })
# ... and the fourth: the REAL admit.exe in its observe mode (this checkout's build, else the main checkout's), state in a temp folder: the full count,
# and a history row (queue_mode observe, would_grant) for every ask
$realExe = Join-Path $repo ('{1}\tools\admit\admit{0}' -f '.exe', 'out')
if (-not (Test-Path -LiteralPath $realExe)) { $env:WWE_ADMIT_EXE = $null; Remove-Item Env:WWE_ADMIT_EXE -ErrorAction SilentlyContinue; $realExe = Get-AdmitExe }
$obsState = Join-Path ([IO.Path]::GetTempPath()) ("cqw_dry_observe_{0}" -f $PID)
if ($realExe) {
  New-Item -ItemType Directory -Force $obsState | Out-Null
  $modes += @{ name = 'observe'; noadmit = $false; exe = $realExe; observe = $true }
  Write-Host "observe mode: the real admit.exe $realExe, state $obsState"
} else { Write-Host 'NOTE observe mode not shown: no admit.exe in this checkout or the main checkout (build it: tools\admit\build_admit.ps1)' }

# Admit.ps1 holds functions only: any other top-level statement would run in every build script's scope when it dot-sources the helper
$bad = 0
$atok = $null; $aerr = $null
$aast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Admit.ps1'), [ref]$atok, [ref]$aerr)
$top = @($aast.EndBlock.Statements | Where-Object { $_ -isnot [System.Management.Automation.Language.FunctionDefinitionAst] })
$top += @(@($aast.BeginBlock, $aast.ProcessBlock, $aast.ParamBlock) | Where-Object { $_ })
if ($aerr.Count -or $top.Count) { Write-Host "FAIL tools/admit/Admit.ps1 has $($top.Count) top-level statement(s) besides functions ($(($top | ForEach-Object { $_.Extent.Text.Split("`n")[0] }) -join ' | '))"; $bad++ }
else { Write-Host "SAME  tools/admit/Admit.ps1: functions only at the top level ($(@($aast.EndBlock.Statements).Count) functions)" }

function Sha([string]$s) { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($s))).Replace('-', '').Substring(0, 16) }
function CodeOf([string]$text) {
  # the code tokens of a script, comments dropped, one token per line (kind + text)
  $tok = $null; $err = $null
  [void][System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$tok, [ref]$err)
  ($tok | Where-Object { $_.Kind -ne 'Comment' } | ForEach-Object { "$($_.Kind) $($_.Text)" }) -join "`n"
}
function Loop($ast) {
  $loops = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'ForEach-Object' -and
      @($n.CommandElements | Where-Object { $_ -is [System.Management.Automation.Language.CommandParameterAst] -and $_.ParameterName -eq 'Parallel' }).Count }, $true))
  if ($loops.Count -ne 1) { throw "expected one ForEach-Object -Parallel loop, found $($loops.Count)" }
  $c = $loops[0]; $el = @($c.CommandElements)
  $tl = $null; $blk = $null
  for ($i = 0; $i -lt $el.Count; $i++) {
    if ($el[$i] -is [System.Management.Automation.Language.CommandParameterAst]) {
      if ($el[$i].ParameterName -eq 'ThrottleLimit') { $tl = $el[$i + 1] }
      if ($el[$i].ParameterName -eq 'Parallel') { $blk = $el[$i + 1] }
    }
  }
  [pscustomobject]@{ Throttle = $tl; Block = $blk }
}
function DryLine([string]$rel, [string]$text) {
  $tok = $null; $err = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$tok, [ref]$err)
  if ($err.Count) { throw "$rel does not parse: $($err[0].Message)" }
  $l = Loop $ast
  $tlText = $l.Throttle.Extent.Text
  # the wiring taken back out: Request-BuildParallel -Want X  ->  X ; the dot-source statement of Admit.ps1 (and its ';') -> nothing
  $inner = $tlText
  $m = [regex]::Match($tlText, '^\(Request-BuildParallel -Want (.+)\)$')
  if ($m.Success) { $inner = $m.Groups[1].Value }
  $s = $l.Throttle.Extent.StartOffset; $e = $l.Throttle.Extent.EndOffset
  $plain = $text.Substring(0, $s) + $inner + $text.Substring($e)
  $plain = [regex]::Replace($plain, "\. \(Join-Path \`$PSScriptRoot '\.\.\\admit\\Admit\.ps1'\); (try \{ )?", '')
  $plain = [regex]::Replace($plain, '(?m)^\} \} finally \{ Release-BuildParallel \}$', '}')   # (round 2: the loop sits in try / finally Release-BuildParallel)
  # the count in each fall-open mode (WWE_NO_ADMIT=1 / no admit.exe / a failing admit.exe), WWE_RIG_JOBS unset and =3
  $par = @()
  foreach ($mode in $modes) {
    if ($mode.noadmit) { $env:WWE_NO_ADMIT = '1' } else { Remove-Item Env:WWE_NO_ADMIT -ErrorAction SilentlyContinue }
    $env:WWE_ADMIT_EXE = $mode.exe
    if ($mode.observe) { $env:WWE_ADMIT_MODE = 'observe'; $env:WWE_ADMIT_DIR = $obsState; $env:WWE_ADMIT_CONFIG = (Join-Path $PSScriptRoot 'admit.json') }
    foreach ($rj in @($null, '3')) {
      if ($rj) { $env:WWE_RIG_JOBS = $rj } else { Remove-Item Env:WWE_RIG_JOBS -ErrorAction SilentlyContinue }
      # (6>$null: the failing exe's loud line). Then Release-BuildParallel, as every wired script's finally does: it ends an observed record too, which
      # writes its history row. (That a second ask in the SAME process gets its full count WITHOUT a release in between is the row after this loop.)
      $par += [string](Invoke-Expression $tlText 6>$null)
      Release-BuildParallel 6>$null
    }
    foreach ($v in 'WWE_ADMIT_MODE', 'WWE_ADMIT_DIR', 'WWE_ADMIT_CONFIG') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  }
  Remove-Item Env:WWE_RIG_JOBS -ErrorAction SilentlyContinue
  $env:WWE_NO_ADMIT = '1'
  $pairs = for ($i = 0; $i -lt $par.Count; $i += 2) { "$($par[$i])/$($par[$i + 1])" }
  '{0}  parallel={1}  block={2}  rest={3}' -f $rel, ($pairs -join ','), (Sha (CodeOf $l.Block.Extent.Text)), (Sha (CodeOf $plain))
}

function CheckScripts([bool]$plant, [bool]$quiet) {
  $n = 0
  foreach ($rel in $scripts) {
    $b = ((& git -C $repo show "${Base}:$rel") -join "`n").TrimEnd()   # (git show's lines lose the last newline: both texts end at their last character)
    $h = ((Get-Content -Raw (Join-Path $repo $rel)) -replace "`r`n", "`n").TrimEnd()
    if ($plant -and $rel -eq 'tools/scene/build_rsc.ps1') { $h = $h.Replace('-Want 16', '-Want 8') }   # PLANT: a changed count
    if ($plant -and $rel -eq 'tools/renderer/build.ps1') { $h = $h.Replace('-c $rel -o $relObj', '-c $rel -O3 -o $relObj') }   # PLANT: a changed compile line
    $lb = DryLine $rel $b
    $lh = DryLine $rel $h
    $wired = $h -match 'Request-BuildParallel'
    if (-not $quiet) { Write-Host "BASE $lb"; Write-Host "HEAD $lh" }
    if ($lb -ne $lh) { if (-not $quiet) { Write-Host "FAIL $rel differs from $Base with WWE_NO_ADMIT=1" }; $n++ }
    elseif (-not $wired) { if (-not $quiet) { Write-Host "FAIL $rel does not call Request-BuildParallel" }; $n++ }
  }
  $n
}
$bad += CheckScripts ([bool]$Plant) $false
function ObserveRows {
  $h = Join-Path $obsState 'history.tsv'
  if (-not (Test-Path $h)) { return @() }
  $lines = @(Get-Content $h | Where-Object { $_.Trim() })
  if ($lines.Count -lt 2) { return @() }
  $head = $lines[0].Split("`t")
  @($lines[1..($lines.Count - 1)] | ForEach-Object { $c = $_.Split("`t"); $o = @{}; for ($i = 0; $i -lt $head.Count; $i++) { $o[$head[$i]] = $c[$i] }; $o })
}
if ($realExe -and -not $Plant) {
  # every HEAD ask in observe mode (28 scripts x WWE_RIG_JOBS unset / 3) left one history row: queue_mode observe, outcome observed, a would_grant
  $rows = @(ObserveRows)
  $obs = @($rows | Where-Object { $_['queue_mode'] -eq 'observe' -and $_['outcome'] -eq 'observed' -and "$($_['would_grant'])" -match '^\d+$' })
  $want = 2 * $scripts.Count
  if ($obs.Count -ne $want) { Write-Host "FAIL observe mode: $($obs.Count) history rows with queue_mode observe / outcome observed / a would_grant, want $want (one per ask; $($rows.Count) rows in all)"; $bad++ }
  else {
    $wg = ($obs | ForEach-Object { "$($_['would_grant'])" } | Group-Object | Sort-Object Name | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', '
    Write-Host "SAME  observe mode, the REAL admit.exe: every script got its full count (the parallel= column above) and wrote one history row per ask ($want rows, queue_mode observe, outcome observed; would_grant: $wg)"
  }
  # two wired builds in the SAME PowerShell process, the first not released: in observe mode the second still gets its full count (nothing nests)
  $env:WWE_ADMIT_EXE = $realExe; $env:WWE_ADMIT_MODE = 'observe'; $env:WWE_ADMIT_DIR = $obsState; $env:WWE_ADMIT_CONFIG = (Join-Path $PSScriptRoot 'admit.json')
  Remove-Item Env:WWE_NO_ADMIT -ErrorAction SilentlyContinue
  $first = Request-BuildParallel -Want 16 6>$null
  $second = Request-BuildParallel -Want 12 6>$null
  Release-BuildParallel 6>$null
  foreach ($v in 'WWE_ADMIT_EXE', 'WWE_ADMIT_MODE', 'WWE_ADMIT_DIR', 'WWE_ADMIT_CONFIG') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  $env:WWE_NO_ADMIT = '1'
  if ($first -eq 16 -and $second -eq 12) { Write-Host "SAME  observe mode, the REAL admit.exe: a second wired build in the same process, the first still holding, gets its full count (16 then 12, nothing nests)" }
  else { Write-Host "FAIL observe mode: the first ask got $first of 16, the second (same process, first not released) $second of 12"; $bad++ }
}
if (-not $Plant) {
  # the planted twin, every run: a changed count (build_rsc) and a changed compile line (renderer) must both be seen, or this check is blind
  $caught = CheckScripts $true $true
  if ($caught -ne 2) { Write-Host "FAIL wiring_dry is blind: the planted count / compile-line change gave $caught failing site(s), want 2"; $bad++ }
  else { Write-Host 'PLANT caught: a changed count (build_rsc -Want 8) and a changed compile line (renderer -O3) each fail the comparison' }
}

# the Python pools: each pool line of BASE `with POOL(ARG) as V:` must be, in HEAD,
# `with _admit.hold('<kind>', WANT, timeout_s=T) as N, POOL(ARG') as V:` where WANT is the base's size (ARG without `max_workers=`) and ARG' is ARG with that
# size replaced by N. With WWE_NO_ADMIT=1, no exe or a failing exe, hold yields WANT (tools\admit\wiring_test.py Y3): today's pool, exactly.
# 2026-11-20: only the POOL lines are compared. The first version judged the whole diff of each file against the frozen base, so any later edit to one of
# the six files failed this check (lane LNK factored two helpers out of game_syntax.py: "11 removed vs 18 added code lines"). A base pool that HEAD no
# longer has at all was removed since (NOTE); a held pool HEAD has beyond the base's is new since (NOTE: tools\admit\wiring_lint.py judges new pools); a
# base pool that is in HEAD un-held, or held with another want or another call, FAILs.
$pySites = @('tools/gate/game_syntax.py', 'tools/q/build_ours.py', 'tools/q/ours_overlay.py', 'tools/scene/keep_stream_off_link.py', 'tools/perf/guest_flags_exp.py', 'tools/w3/diff/run.py')
$poolRe = '^\s*with (?<call>[\w.]*Pool(?:Executor)?)\((?<arg>.*)\) as (?<v>\w+):$'
$heldRe = "^\s*with _admit\.hold\('[\w.]+', (?<want>.+?), timeout_s=\d+\) as (?<n>\w+), (?<call>[\w.]*Pool(?:Executor)?)\((?<arg>.*)\) as (?<v>\w+):$"
function PyMatches([string[]]$lines, [string]$re) {
  @($lines | ForEach-Object { [regex]::Match((("$_" -replace '\s+#.*$', '').TrimEnd()), $re) } | Where-Object { $_.Success })
}
function CheckPyPools([string[]]$baseLines, [string[]]$headLines) {
  $basePools = PyMatches $baseLines $poolRe
  $held = PyMatches $headLines $heldRe
  $bare = PyMatches $headLines $poolRe
  $used = @{}
  $r = @{ same = 0; notes = @(); why = '' }
  if ($basePools.Count -eq 0) { $r.why = 'the base has no pool line (wrong base?)'; return $r }
  foreach ($m in $basePools) {
    $call = $m.Groups['call'].Value; $v = $m.Groups['v'].Value; $arg = $m.Groups['arg'].Value
    $size = $arg -replace '^max_workers=', ''
    $hit = -1
    for ($i = 0; $i -lt $held.Count; $i++) {
      if ($used.ContainsKey($i)) { continue }
      $p = $held[$i]
      $expArg = if ($arg.StartsWith('max_workers=')) { 'max_workers=' + $p.Groups['n'].Value } else { $p.Groups['n'].Value }
      if ($p.Groups['call'].Value -eq $call -and $p.Groups['v'].Value -eq $v -and $p.Groups['want'].Value -eq $size -and $p.Groups['arg'].Value -eq $expArg) { $hit = $i; break }
    }
    if ($hit -ge 0) { $used[$hit] = $true; $r.same++; continue }
    $line = $m.Value.Trim()
    if (@($bare | Where-Object { $_.Groups['call'].Value -eq $call -and $_.Groups['v'].Value -eq $v -and $_.Groups['arg'].Value -eq $arg }).Count) { $r.why = "the base pool '$line' is in HEAD without a hold"; return $r }
    $near = @(for ($i = 0; $i -lt $held.Count; $i++) { if (-not $used.ContainsKey($i) -and $held[$i].Groups['call'].Value -eq $call -and $held[$i].Groups['v'].Value -eq $v) { $held[$i] } })
    if ($near.Count) { $r.why = "the base pool '$line' is held in HEAD with another want or size: '$($near[0].Value.Trim())' (want must be '$size')"; return $r }
    $r.notes += "removed since the base: '$line'"
  }
  for ($i = 0; $i -lt $held.Count; $i++) { if (-not $used.ContainsKey($i)) { $r.notes += "new since the base (wiring_lint judges it): '$($held[$i].Value.Trim())'" } }
  return $r
}
$pyLines = 0
foreach ($rel in $pySites) {
  $baseLines = @(& git -C $repo show "${Base}:$rel" 2>$null)
  $headLines = @(Get-Content -LiteralPath (Join-Path $repo $rel))
  $r = CheckPyPools $baseLines $headLines
  $pyLines += $r.same
  Write-Host ('{0}  {1}  {2} pool line(s): today''s size as the want and the fall-open value{3}' -f $(if ($r.why) { 'FAIL' } else { 'SAME' }), $rel, $r.same, $(if ($r.why) { " ($($r.why))" } else { '' }))
  foreach ($n in $r.notes) { Write-Host "NOTE  $rel  $n" }
  if ($r.why) { $bad++ }
}
# the planted twins of the Python part, every run: a held pool with another want, and a pool whose hold was taken off, must each FAIL, or this part is blind
$gsBase = @(& git -C $repo show "${Base}:tools/gate/game_syntax.py" 2>$null)
$gsHead = @(Get-Content -LiteralPath (Join-Path $repo 'tools/gate/game_syntax.py'))
$twinWant = @($gsHead | ForEach-Object { $_ -replace "(_admit\.hold\('syntax\.worker', )4(, timeout_s=)", '${1}5${2}' })
$twinBare = @($gsHead | ForEach-Object { $_ -replace "with _admit\.hold\('syntax\.worker', 4, timeout_s=\d+\) as nw, ThreadPoolExecutor\(max_workers=nw\) as ex:", 'with ThreadPoolExecutor(max_workers=4) as ex:' })
$pyCaught = @((CheckPyPools $gsBase $twinWant).why, (CheckPyPools $gsBase $twinBare).why | Where-Object { $_ }).Count
if ((-join $twinWant) -eq (-join $gsHead) -or (-join $twinBare) -eq (-join $gsHead)) { Write-Host 'FAIL wiring_dry: the Python planted twins changed nothing (game_syntax.py''s fixed-list pool line moved: update the twins)'; $bad++ }
elseif ($pyCaught -ne 2) { Write-Host "FAIL wiring_dry is blind on the Python pools: the planted want / un-held pool gave $pyCaught failing check(s), want 2"; $bad++ }
else { Write-Host 'PLANT caught (Python pools): a changed want (4 -> 5) and a pool with its hold taken off each fail the comparison' }
if ($realExe) {
  # the Python pools on the REAL exe in observe mode: each kind at today's want gets the full want and one history row with a would_grant
  $env:WWE_ADMIT_EXE = $realExe; $env:WWE_ADMIT_MODE = 'observe'; $env:WWE_ADMIT_DIR = $obsState; $env:WWE_ADMIT_CONFIG = (Join-Path $PSScriptRoot 'admit.json')
  Remove-Item Env:WWE_NO_ADMIT -ErrorAction SilentlyContinue
  $o = & py -I (Join-Path $PSScriptRoot 'dry_observe.py') 2>&1
  $code = $LASTEXITCODE
  $o | Where-Object { "$_" -match '^(SAME|FAIL)' } | ForEach-Object { Write-Host $_ }
  if ($code -ne 0) { $bad++ }
  foreach ($v in 'WWE_ADMIT_EXE', 'WWE_ADMIT_MODE', 'WWE_ADMIT_DIR', 'WWE_ADMIT_CONFIG') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  $env:WWE_NO_ADMIT = '1'
  Remove-Item -Recurse -Force -LiteralPath $obsState -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath $failCmd -ErrorAction SilentlyContinue
if ($bad) { Write-Host "FAIL wiring_dry: $bad site(s) differ from $Base"; exit 1 }
Write-Host "PASS wiring_dry: $($scripts.Count) tool builds (WWE_NO_ADMIT=1, no exe, a failing exe$(if ($realExe) { ', the real exe in observe mode' })) and $pyLines Python pool line(s) in $($pySites.Count) files give today's counts and command lines (base $($Base.Substring(0, 10))); Admit.ps1 has no top level"
