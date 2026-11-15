# S102 TS1: replay a capture with rplay on the Surface, N passes in a row, and say per pass whether the Vulkan device was lost.
#
# *** THIS IS A SURFACE RUN (it uses the Surface's GPU for minutes). The main session decides when to run it; builders and reviewers never do. ***
# -Push / -SelfTest / -Help / -ExeOnly below do not run a capture (-Help runs `rplay.exe --help`, which rplay ignores and answers with its own 3-frame triangle test).
#
#   pwsh -NoProfile -File tools\surface\rplay_remote.ps1 -Capture out\captures\brock_losswin_rt -Passes 5 -Frames 300 -Flags '--validation 0 --warm-frames 30' -Out out\s102\rplay_remote\a
#   pwsh -NoProfile -File tools\surface\rplay_remote.ps1 -Capture out\captures\brock_losswin_rt -Passes 5 -Base          (+ the kept levers of base_flags.txt that rplay understands)
#   pwsh -NoProfile -File tools\surface\rplay_remote.ps1 -Capture out\captures\brock_losswin_rt -PushOnly                (exe + capture to C:\wwe\rplay, no run)
#   pwsh -NoProfile -File tools\surface\rplay_remote.ps1 -ExeOnly -Help                                                  (exe push + `rplay.exe --help` there)
#   pwsh -NoProfile -File tools\surface\rplay_remote.ps1 -SelfTest                                                       (offline: flag mapping, push plan, log reading; no ssh)
#
# Pushes (skipping what is already there: size + SHA256 compared, files that differ or are missing go in ONE tar stream over ssh):
#   <RemoteRoot>\rplay.exe            -Exe (default: out\renderer\surface\rplay.exe if built, else out\renderer\dev\rplay.exe). Both import only kernel32 / user32 / gdi32:
#                                     our shaders are compiled into the exe, so NO dll / shader file is needed (the capture holds the rest).
#   <RemoteRoot>\captures\<NAME>\     -Capture (game-derived: copied to Jarrod's own Surface, never committed).
# Runs `rplay.exe --capture captures\<NAME> --frames N <flags>` -Passes times, one after the other, each in a fresh process (headless unless -Windowed), each with a hard
# timeout (-PassTimeoutSeconds, then taskkill of rplay.exe). Per pass one line into <Out>\passes.log:
#   pass k/N exit E, DEVICE_LOST yes|no, frames F, S s [, TIMEOUT]
# and <Out>\pass_k.log fetched back. After the passes it lists the Surface's GPU watchdog reports newer than the first pass (wer_lib.ps1) into <Out>\watchdog\.
# Refuses to run while wwe2k14.exe or rplay.exe is already running on the Surface (a bench job is on the GPU): nothing is killed.
# rplay has no --loop: -Passes is the loop (a device loss is a process-level event; a fresh process per pass is the repeatable unit). rplay does not know -Frames 0 = all;
# without -Frames the whole capture is played (rplay's default).
param([string]$Capture = '', [string]$Exe = '', [int]$Passes = 3, [int]$Frames = 0, [string]$Flags = '', [switch]$Base, [switch]$Windowed,
      [int]$PassTimeoutSeconds = 900, [string]$Out = '', [string]$RemoteRoot = 'C:/wwe/rplay', [string]$HostName = 'surface',
      [switch]$PushOnly, [switch]$ExeOnly, [switch]$Help, [switch]$SelfTest, [switch]$NoWer)
$ErrorActionPreference = 'Stop'
$R = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$RplaySrc = Join-Path $R 'src\renderer\player\rplay.cpp'
$sshOpts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4')
. (Join-Path $PSScriptRoot 'wer_lib.ps1')   # Invoke-Timed (every ssh / scp here runs as its own process with a hard timeout) and Save-JobWer

# ---- pure functions (the self-test covers these) -------------------------------------------------------------------------------------------------------------------------
# The flags rplay.cpp parses: name -> $true when it takes a value ("--x" && i + 1 < argc), $false for a bare flag. rplay IGNORES an unknown flag silently, so a typo would run the default.
function Get-RplayFlagTable([string]$Src) {
  $t = @{}
  foreach ($m in [regex]::Matches((Get-Content -LiteralPath $Src -Raw), 'arg == "--([a-z0-9][a-z0-9-]*)"( && i \+ 1 < argc)?')) { $t[$m.Groups[1].Value] = [bool]$m.Groups[2].Success }
  $t
}
# base_flags.txt (wwe2k14_rdr_lazy_banks=true ...) -> rplay flags where rplay has a flag of that name (underscores -> dashes, 'wwe2k14_rdr_' / 'wwe2k14_' dropped). true -> bare flag, or `1` when it takes a value
# (rplay reads on / 1 / not-off alike); false -> left out; a number -> that value. Returns @{ Flags = tokens; Unmapped = names }.
function Get-BaseRplayFlags([string]$BaseFile, [hashtable]$Table) {
  $tok = New-Object System.Collections.Generic.List[string]; $un = New-Object System.Collections.Generic.List[string]
  foreach ($ln in Get-Content -LiteralPath $BaseFile) {
    $ln = $ln.Trim(); if (-not $ln -or $ln.StartsWith('#')) { continue }
    $k, $v = $ln -split '=', 2
    $n = ($k -replace '^wwe2k14_rdr_', '' -replace '^wwe2k14_', '').Replace('_', '-')
    if ($v -eq 'false' -or $v -eq '0') { continue }
    if (-not $Table.ContainsKey($n)) { $un.Add($k); continue }
    if ($Table[$n]) { $tok.Add("--$n"); $tok.Add($(if ($v -eq 'true') { '1' } else { $v })) } else { $tok.Add("--$n") }
  }
  @{ Flags = @($tok); Unmapped = @($un) }
}
# A flag string -> checked tokens. Refuses characters a cmd line could misread and any --flag rplay does not parse (silently ignored otherwise).
function Test-RplayFlags([string[]]$Tokens, [hashtable]$Table) {
  foreach ($t in $Tokens) {
    if ($t -notmatch '^[A-Za-z0-9_.=:,/\\+-]+$') { throw "flag token '$t' holds a character that is not allowed (letters, digits and _ . = : , / \ + - only)" }
    if ($t -match '^--(.+)$' -and $Table -and -not $Table.ContainsKey($Matches[1])) { throw "rplay has no flag '$t' (it would be ignored silently): see src\renderer\player\rplay.cpp" }
  }
}
# What to send: files missing there or different there. $Local / $Remote are @{ rel = 'size|HASH' }. Also lists the extra files there (never deleted).
function Get-PushPlan([hashtable]$Local, [hashtable]$Remote) {
  $send = @($Local.Keys | Where-Object { -not $Remote.ContainsKey($_) -or $Remote[$_] -ne $Local[$_] } | Sort-Object)
  @{ Send = $send; Same = @($Local.Keys).Count - $send.Count; Extra = @($Remote.Keys | Where-Object { -not $Local.ContainsKey($_) } | Sort-Object) }
}
# One pass's log -> facts. Loss = rplay's / the renderer's own lines (src\renderer: `[rdr][DEVICE_LOST] =====`, `VkResult -4`, `VK_ERROR_DEVICE_LOST`, `frame_submit_failed`).
function Read-PassLog([string]$LogPath) {
  $o = @{ Lost = $false; Frames = $null; ValErr = $null; Lines = 0 }
  if (-not (Test-Path -LiteralPath $LogPath)) { return $o }
  foreach ($ln in Get-Content -LiteralPath $LogPath) {
    $o.Lines++
    if ($ln -match 'DEVICE_LOST|VkResult -4\b|frame_submit_failed') { $o.Lost = $true }
    if ($ln -match '\[rplay\] capture: (\d+) frames') { $o.Frames = [int]$Matches[1] }
    if ($ln -match 'validation errors (\d+)') { $o.ValErr = [int]$Matches[1] }
  }
  $o
}
function Format-PassLine([int]$K, [int]$N, $Exit, [hashtable]$Facts, [double]$Secs, [bool]$TimedOut) {
  $fr = if ($null -ne $Facts.Frames) { "$($Facts.Frames)" } else { 'none (no "[rplay] capture: N frames" line)' }
  "pass $K/$N exit $Exit, DEVICE_LOST $(if ($Facts.Lost) { 'YES' } else { 'no' }), frames $fr, $([math]::Round($Secs, 1)) s" + $(if ($TimedOut) { ', TIMEOUT (killed)' } else { '' }) + $(if ($null -ne $Facts.ValErr -and $Facts.ValErr -gt 0) { ", validation errors $($Facts.ValErr)" } else { '' })
}

function Get-LocalMap([string]$Dir) {
  $m = @{}; $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
  foreach ($f in Get-ChildItem -LiteralPath $base -Recurse -File) { $m[$f.FullName.Substring($base.Length + 1).Replace('\', '/')] = "$($f.Length)|$((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash)" }
  $m
}

# ---- self test ---------------------------------------------------------------------------------------------------------------------------------------------------------------
if ($SelfTest) {
  $fails = 0
  function T([bool]$c, [string]$w) { if ($c) { Write-Host "ok   $w" } else { Write-Host "FAIL $w"; $script:fails++ } }
  function Throws([scriptblock]$b) { try { & $b; $false } catch { $true } }
  $tmp = Join-Path ([IO.Path]::GetTempPath()) ("rplay_remote_selftest_" + $PID); New-Item -ItemType Directory -Force $tmp | Out-Null
  try {
    $src = Join-Path $tmp 'rplay.cpp'
    Set-Content $src 'if (arg == "--validation" && i + 1 < argc) a; else if (arg == "--window") w; else if (arg == "--lazy-banks" && i + 1 < argc) l; else if (arg == "--crowd-fast" && i + 1 < argc) c; else if (arg == "--no-vsync") v;'
    $tab = Get-RplayFlagTable $src
    T ($tab.Count -eq 5 -and $tab['validation'] -and -not $tab['window'] -and $tab['lazy-banks']) 'flag table: value flags and bare flags told apart'
    if (Test-Path $RplaySrc) { $real = Get-RplayFlagTable $RplaySrc; T ($real.Count -gt 100 -and $real.ContainsKey('capture') -and $real.ContainsKey('frames') -and $real.ContainsKey('validation')) "the real rplay.cpp table has $($real.Count) flags incl. capture / frames / validation" }
    $bf = Join-Path $tmp 'base.txt'
    Set-Content $bf @('# c', 'wwe2k14_rdr_lazy_banks=true', 'wwe2k14_rdr_crowd_fast=true', 'wwe2k14_rdr_no_vsync=true', 'wwe2k14_rdr_provenance=false', 'wwe2k14_rdr_alias_fullsize=1', 'wwe2k14_display_720p=true')
    $m = Get-BaseRplayFlags $bf $tab
    T (($m.Flags -join ' ') -eq '--lazy-banks 1 --crowd-fast 1 --no-vsync') "base mapping: $($m.Flags -join ' ')"
    T (($m.Unmapped -join ',') -eq 'wwe2k14_rdr_alias_fullsize,wwe2k14_display_720p') "unmapped base levers named: $($m.Unmapped -join ',')"
    Test-RplayFlags @('--validation', '0', '--lazy-banks', '1') $tab; T $true 'known flags pass'
    T (Throws { Test-RplayFlags @('--validaton', '0') $tab }) 'PLANT a mistyped flag is refused (rplay would ignore it silently)'
    T (Throws { Test-RplayFlags @('--validation', '0&calc') $tab }) 'PLANT a cmd metacharacter is refused'
    T (Throws { Test-RplayFlags @('a b') $tab }) 'PLANT a token with a space is refused'
    $loc = @{ 'a.bin' = '10|AA'; 'b.bin' = '20|BB'; 'frames/c.bin' = '5|CC' }
    $plan = Get-PushPlan $loc @{ 'a.bin' = '10|AA'; 'b.bin' = '20|XX'; 'old.bin' = '1|ZZ' }
    T (($plan.Send -join ',') -eq 'b.bin,frames/c.bin' -and $plan.Same -eq 1 -and ($plan.Extra -join ',') -eq 'old.bin') 'push plan: same skipped, a differing hash and a missing file sent, an extra file kept'
    T ((Get-PushPlan $loc $loc).Send.Count -eq 0) 'push plan: everything there = nothing sent'
    T ((Get-PushPlan $loc @{}).Send.Count -eq 3) 'PLANT push plan: an empty remote = everything sent'
    $lg = Join-Path $tmp 'ok.log'; Set-Content $lg @('[rdr] device: x', '[rplay] capture: 300 frames, 100 draws', '[rplay] device X, 300 frames, validation errors 0')
    $f = Read-PassLog $lg; T (-not $f.Lost -and $f.Frames -eq 300 -and $f.ValErr -eq 0) 'a clean log: no loss, 300 frames'
    $lg2 = Join-Path $tmp 'lost.log'; Set-Content $lg2 @('[rdr][PROBLEM] frame_submit_failed: VkResult -4', '[rdr][DEVICE_LOST] ===== the Vulkan device is LOST')
    $f2 = Read-PassLog $lg2; T ($f2.Lost -and $null -eq $f2.Frames) 'PLANT a lost log: DEVICE_LOST yes, no frames line'
    T ((Format-PassLine 2 5 1 $f2 12.34 $true) -eq 'pass 2/5 exit 1, DEVICE_LOST YES, frames none (no "[rplay] capture: N frames" line), 12.3 s, TIMEOUT (killed)') 'the pass line format'
    T (-not (Read-PassLog (Join-Path $tmp 'nothere.log')).Lost) 'a missing log reads as no facts'
    # the local hashing used by the push (the real function below is the same code path)
    $d = Join-Path $tmp 'cap'; New-Item -ItemType Directory -Force "$d\frames" | Out-Null; Set-Content "$d\x.bin" 'abc'; Set-Content "$d\frames\f0.bin" 'defg'
    $lm = Get-LocalMap $d; T ($lm.Count -eq 2 -and $lm.ContainsKey('frames/f0.bin') -and $lm['x.bin'] -match '^\d+\|[0-9A-F]{64}$') 'local map: relative forward-slash names, size|SHA256'
  } finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
  if ($fails) { Write-Host "$fails FAILED"; exit 1 } else { Write-Host 'ALL OK'; exit 0 }
}

# ---- ssh helpers -----------------------------------------------------------------------------------------------------------------------------------------------------------
# Runs a script on the Surface through ssh stdin ([scriptblock]::Create of the text), arguments on the command line (no spaces in them).
function Invoke-RemoteScript([string]$Body, [string]$ArgText = '', [int]$TimeoutSec = 300) {
  # Invoke-Timed (wer_lib.ps1): async stdin write, async output reads, hard timeout, process tree killed on timeout (a blocking stdin write / pipeline hung the job runner once).
  $r = Invoke-Timed 'ssh' ($sshOpts + @($HostName, "powershell -NoProfile -NonInteractive -Command `"& ([scriptblock]::Create([Console]::In.ReadToEnd())) $ArgText`"")) -Stdin $Body -TimeoutSec $TimeoutSec
  if ($r.TimedOut) { throw "ssh script timed out after $TimeoutSec s (killed)" }
  if ($r.Exit -ne 0) { throw "ssh script failed (exit $($r.Exit)): $(($r.Err -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1))" }
  @($r.Out -split "`r?`n" | Where-Object { $_ -ne '' })
}
$RemoteMapScript = @'
param([string]$Dir)
if (Test-Path -LiteralPath $Dir) { $b = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\'); Get-ChildItem -LiteralPath $b -Recurse -File | ForEach-Object { "F|" + $_.FullName.Substring($b.Length + 1).Replace('\','/') + "|" + $_.Length + "|" + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } }
'@
function Get-RemoteMap([string]$WinDir) {
  $m = @{}
  foreach ($l in (Invoke-RemoteScript $RemoteMapScript "-Dir $WinDir" 600)) { $f = $l -split '\|'; if ($f[0] -eq 'F' -and $f.Count -eq 4) { $m[$f[1]] = "$($f[2])|$($f[3])" } }
  $m
}
function Push-Dir([string]$LocalDir, [string]$RemoteWin, [string]$RemoteFwd, [string]$What) {
  $lm = Get-LocalMap $LocalDir
  $rm = Get-RemoteMap $RemoteWin
  $plan = Get-PushPlan $lm $rm
  Write-Host "$What`: $($lm.Count) files here, $($plan.Same) already on the Surface, $($plan.Send.Count) to send$(if ($plan.Extra.Count) { ", $($plan.Extra.Count) extra there (kept)" })"
  if ($plan.Send.Count) {
    $null = Invoke-RemoteScript "New-Item -ItemType Directory -Force '$RemoteWin' | Out-Null" '' 60
    $list = Join-Path ([IO.Path]::GetTempPath()) "rplay_push_$PID.txt"
    [IO.File]::WriteAllLines($list, [string[]]$plan.Send)
    $base = (Resolve-Path -LiteralPath $LocalDir).Path.TrimEnd('\')
    # raw bytes through cmd's pipe (a PowerShell pipeline would re-encode them): tar here -> tar there (Windows' own tar.exe)
    $cmd = "tar -cf - -C `"$base`" -T `"$list`" | ssh $($sshOpts -join ' ') $HostName tar -xf - -C $RemoteFwd"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $tr = Invoke-Timed 'cmd' -RawArgs "/c $cmd" -TimeoutSec 3600
    Remove-Item $list -ErrorAction SilentlyContinue
    if ($tr.TimedOut) { throw "$What`: the tar transfer timed out after 3600 s (killed)" }
    if ($tr.Exit -ne 0) { throw "$What`: the tar transfer failed (exit $($tr.Exit)): $(($tr.Err -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1))" }
    $rm2 = Get-RemoteMap $RemoteWin
    $bad = @($plan.Send | Where-Object { -not $rm2.ContainsKey($_) -or $rm2[$_] -ne $lm[$_] })
    if ($bad.Count) { throw "$What`: $($bad.Count) files differ after the transfer, first: $($bad[0])" }
    Write-Host "$What`: sent $($plan.Send.Count) files in $([math]::Round($sw.Elapsed.TotalSeconds, 1)) s, verified by size + SHA256"
  }
}

# ---- main --------------------------------------------------------------------------------------------------------------------------------------------------------------------
if ($Passes -lt 1 -or $Passes -gt 50) { throw '-Passes wants 1..50' }
if ($RemoteRoot -notmatch '^[A-Za-z]:/[A-Za-z0-9_./-]+$') { throw "-RemoteRoot wants a forward-slash path without spaces, e.g. C:/wwe/rplay (got $RemoteRoot)" }
$remWin = $RemoteRoot.Replace('/', '\')
if (-not $Exe) { $Exe = @('out\renderer\surface\rplay.exe', 'out\renderer\dev\rplay.exe') | ForEach-Object { Join-Path $R $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) { throw 'no rplay.exe (build: pwsh -NoProfile -File tools\renderer\build.ps1 -Surface)' }
$table = if (Test-Path $RplaySrc) { Get-RplayFlagTable $RplaySrc } else { $null }
$tokens = @($Flags -split '\s+' | Where-Object { $_ })
if ($Base) {
  if (-not $table) { throw '-Base needs src\renderer\player\rplay.cpp (the flag table)' }
  $bm = Get-BaseRplayFlags (Join-Path $PSScriptRoot 'base_flags.txt') $table
  Write-Host "base levers rplay understands: $($bm.Flags -join ' ')"
  if ($bm.Unmapped.Count) { Write-Host "base levers rplay has no flag for (not passed): $($bm.Unmapped -join ', ')" }
  $tokens = @($bm.Flags) + $tokens
}
Test-RplayFlags $tokens $table
$name = ''
if ($Capture) {
  $Capture = (Resolve-Path -LiteralPath $Capture).Path
  $name = Split-Path $Capture -Leaf
  if ($name -notmatch '^[A-Za-z0-9_.-]+$') { throw "capture folder name '$name' must be letters / digits / _ . - (it becomes a folder on the Surface)" }
  foreach ($need in 'images.bin', 'geometry.bin', 'textures.bin') { if (-not (Test-Path (Join-Path $Capture $need))) { throw "$Capture is not a capture (no $need)" } }
} elseif (-not $ExeOnly) { throw '-Capture is needed (or -ExeOnly)' }

Write-Host "$(Get-Date -Format 'HH:mm:ss') rplay_remote: exe $Exe -> $HostName`:$RemoteRoot"
$null = Invoke-RemoteScript "New-Item -ItemType Directory -Force '$remWin' | Out-Null; New-Item -ItemType Directory -Force '$remWin\captures','$remWin\logs' | Out-Null" '' 60
# the exe: one file, size + hash, scp
$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash; $exeLen = (Get-Item -LiteralPath $Exe).Length
$have = Invoke-RemoteScript "if (Test-Path '$remWin\rplay.exe') { (Get-Item '$remWin\rplay.exe').Length.ToString() + '|' + (Get-FileHash '$remWin\rplay.exe' -Algorithm SHA256).Hash }" '' 120
if (@($have)[0] -eq "$exeLen|$exeHash") { Write-Host "rplay.exe: already there ($([math]::Round($exeLen / 1MB, 1)) MB, same SHA256)" }
else {
  # not while THIS rplay.exe is running there (a running exe cannot be replaced); a game / another rplay elsewhere is no reason (the copy touches no GPU)
  $busy = @(Invoke-RemoteScript "Get-Process rplay -ErrorAction SilentlyContinue | Where-Object { `$_.Path -eq '$remWin\rplay.exe' } | ForEach-Object { `$_.Id }" '' 60)
  if ($busy.Count) { throw "rplay.exe is running there (pid $($busy -join ', ')): not replacing it" }
  $sc = Invoke-Timed 'scp' (@('-q') + $sshOpts + @($Exe, "${HostName}:$RemoteRoot/rplay.exe")) -TimeoutSec 600
  if ($sc.TimedOut -or $sc.Exit -ne 0) { throw "scp of rplay.exe failed (exit $($sc.Exit)$(if ($sc.TimedOut) { ', timed out after 600 s' }))" }
  $chk = Invoke-RemoteScript "(Get-Item '$remWin\rplay.exe').Length.ToString() + '|' + (Get-FileHash '$remWin\rplay.exe' -Algorithm SHA256).Hash" '' 120
  if (@($chk)[0] -ne "$exeLen|$exeHash") { throw 'rplay.exe differs after the copy' }
  Write-Host "rplay.exe: pushed ($([math]::Round($exeLen / 1MB, 1)) MB), verified by size + SHA256"
}

if ($Help) {
  $busy = @(Invoke-RemoteScript "Get-Process rplay,wwe2k14 -ErrorAction SilentlyContinue | ForEach-Object { `$_.ProcessName }" '' 60)
  if ($busy.Count) { Write-Host "SKIPPED rplay.exe --help: the Surface is busy ($($busy -join ', ') running)" }
  else {
    Write-Host '--- rplay.exe --help on the Surface (rplay ignores the flag and runs its 3-frame triangle test):'
    $o = Invoke-RemoteScript "cmd /c 'cd /d $remWin && rplay.exe --help 2>&1'; 'exit ' + `$LASTEXITCODE" '' 120
    $o | ForEach-Object { Write-Host "  $_" }
  }
}
if ($ExeOnly) { return }

Push-Dir $Capture "$remWin\captures\$name" "$RemoteRoot/captures/$name" "capture $name"
if ($PushOnly) { return }

# ---- the passes --------------------------------------------------------------------------------------------------------------------------------------------------------------
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
if (-not $Out) { $Out = Join-Path $R "out\s102\rplay_remote\$stamp" }
New-Item -ItemType Directory -Force $Out | Out-Null
Set-Content -LiteralPath (Join-Path $Out 'KEEP') -Value ("rplay_remote results (device-loss passes on the Surface). until " + (Get-Date).AddDays(14).ToString('yyyy-MM-dd'))
$passLog = Join-Path $Out 'passes.log'
function PLog([string]$m) { $l = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"; Add-Content $passLog $l; Write-Host $l }
$busy = @(Invoke-RemoteScript "Get-Process rplay,wwe2k14 -ErrorAction SilentlyContinue | ForEach-Object { `$_.ProcessName }" '' 60)
if ($busy.Count) { throw "the Surface is busy ($($busy -join ', ') running): nothing started, nothing killed" }
$rargs = @('--capture', "captures\$name") + $(if ($Frames -gt 0) { @('--frames', "$Frames") } else { @() }) + $(if ($Windowed) { @('--window') } else { @() }) + $tokens
$logDirWin = "$remWin\logs\$stamp"
$null = Invoke-RemoteScript "New-Item -ItemType Directory -Force '$logDirWin' | Out-Null" '' 60
PLog "rplay_remote START: $Passes passes of rplay.exe $($rargs -join ' ') ($(if ($Windowed) { 'windowed' } else { 'headless' }); exe SHA256 $($exeHash.Substring(0, 12)); timeout $PassTimeoutSeconds s a pass)"
$firstStart = Get-Date; $lostPasses = 0; $failPasses = 0
for ($k = 1; $k -le $Passes; $k++) {
  $t0 = Get-Date
  $cmd = "cd /d $remWin && rplay.exe $($rargs -join ' ') > logs\$stamp\pass_$k.log 2>&1"
  $psi = New-Object Diagnostics.ProcessStartInfo 'ssh'
  foreach ($o in $sshOpts) { $psi.ArgumentList.Add($o) }
  $psi.ArgumentList.Add($HostName); $psi.ArgumentList.Add($cmd); $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $null = $p.StandardOutput.ReadToEndAsync(); $null = $p.StandardError.ReadToEndAsync()
  $timedOut = $false
  if (-not $p.WaitForExit($PassTimeoutSeconds * 1000)) {
    $timedOut = $true
    try { $p.Kill($true) } catch {}
    try { $null = Invoke-RemoteScript "taskkill /f /im rplay.exe 2>&1 | Out-Null" '' 60 } catch { PLog "  warning: taskkill after the timeout failed: $($_.Exception.Message)" }
  }
  $exit = if ($timedOut) { 'timeout' } else { $p.ExitCode }
  $secs = ((Get-Date) - $t0).TotalSeconds
  $localLog = Join-Path $Out "pass_$k.log"
  $sc = Invoke-Timed 'scp' (@('-q') + $sshOpts + @("${HostName}:$($RemoteRoot)/logs/$stamp/pass_$k.log", $localLog)) -TimeoutSec 120
  if ($sc.TimedOut -or $sc.Exit -ne 0) { PLog "  warning: the pass $k log could not be fetched (scp exit $($sc.Exit)$(if ($sc.TimedOut) { ', timed out' }))" }
  $facts = Read-PassLog $localLog
  if ($facts.Lost) { $lostPasses++ }
  if ($timedOut -or $exit -ne 0 -or $null -eq $facts.Frames) { $failPasses++ }
  PLog (Format-PassLine $k $Passes $exit $facts $secs $timedOut)
}
PLog "rplay_remote DONE: $Passes passes, DEVICE_LOST in $lostPasses, failed (non-zero exit, timeout or no frames line) $failPasses; logs in $Out"
if (-not $NoWer) {
  try {
    . (Join-Path $PSScriptRoot 'wer_lib.ps1')
    Save-JobWer "rplay_$stamp" $firstStart $Out ({ param($m) PLog $m }) 'C:\wwe\out\wer_stage' $HostName
  } catch { PLog "warning: the GPU watchdog listing failed: $($_.Exception.Message)" }
}
if ($lostPasses -or $failPasses) { exit 3 } else { exit 0 }
