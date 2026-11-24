# S109 WARM: the pure pieces of tools\exe\tip_exe.ps1 (docs\research\s109\WARM_plan.md), dot-sourced by it and by tip_exe_selftest.ps1.
# Functions only (dot-sourcing runs nothing). No lock, no build, no game: files under <main>\out\exe and read-only git.

function Get-TipNames([string]$Preset) {
    # The check exe (stream-free) and the stream-keeping exe each have their own pointer and folder prefix. Any other preset: $null.
    switch ($Preset) {
        'win-amd64-release-off' { return [pscustomobject]@{ Pointer = 'tip.json'; Prefix = 'tip'; Rig = $true } }
        'win-amd64-release' { return [pscustomobject]@{ Pointer = 'tip-stream.json'; Prefix = 'tipstream'; Rig = $false } }
    }
    return $null
}

function Read-TipPointer([string]$Path) {
    # -> the pointer object, or $null (missing, unreadable, or without an exe path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try { $p = [IO.File]::ReadAllText($Path) | ConvertFrom-Json } catch { return $null }
    if (-not $p -or -not "$($p.exe)") { return $null }
    return $p
}

function Write-TipJson([string]$Path, $Obj) {
    # Written beside and renamed over the old file, so a reader sees the old or the new file, never half of one. A failed rename leaves the old file and no .tmp.
    $tmp = "$Path.$PID.tmp"
    try {
        [IO.File]::WriteAllText($tmp, ($Obj | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($tmp, $Path, $true)
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
}

function Write-TipLog([string]$ExeRoot, [string]$Line) {
    # out\exe\tip.log: one line per event. Never throws.
    try { [IO.File]::AppendAllText((Join-Path $ExeRoot 'tip.log'), "$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) [$PID] $Line`n") } catch { }
}

function Get-TipHead([string]$Main) {
    $h = "$(& git -C $Main rev-parse HEAD 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $h -notmatch '^[0-9a-f]{40}$') { return '' }
    return $h
}

function Test-TipExeCurrent([string]$Main, [string]$Exe, [string]$Judge) {
    # The stale-exe judge in HEAD mode (no --want): exit 0 = no code path differs between the exe's stamp and HEAD (tools / docs commits do not count).
    $out = & py -I $Judge --exe $Exe --repo $Main 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $out.Trim() }
}

function Get-TipRigNeed([string]$Main) {
    # -> {Need, Why}. The rig (out\tools\frame_rig.exe, built from the main checkout by tools\scene\build_rig.ps1) is rebuilt when its sources at HEAD differ
    # from its stamp's commit: every rig source is under src\ or tools\scene\ (build_rig.ps1 $srcs, $newestHdr). Other commits (docs, tools) do not count.
    $rig = Join-Path $Main 'out\tools\frame_rig.exe'; $st = Join-Path $Main 'out\tools\frame_rig.stamp'
    if (-not (Test-Path -LiteralPath $rig)) { return [pscustomobject]@{ Need = $true; Why = 'no rig' } }
    if (-not (Test-Path -LiteralPath $st)) { return [pscustomobject]@{ Need = $true; Why = 'no frame_rig.stamp' } }
    $m = [regex]::Match([IO.File]::ReadAllText($st), 'git\s+([0-9a-fA-F]{7,40})')
    if (-not $m.Success) { return [pscustomobject]@{ Need = $true; Why = 'the stamp names no commit' } }
    $c = $m.Groups[1].Value.ToLowerInvariant()
    $null = & git -C $Main rev-parse --verify -q "$c^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) { return [pscustomobject]@{ Need = $true; Why = "the stamp's commit $c is unknown here" } }
    $d = @(& git -C $Main diff --name-only $c HEAD -- src tools/scene 2>$null | Where-Object { "$_".Trim() })
    if ($d.Count) { return [pscustomobject]@{ Need = $true; Why = "$($d.Count) file(s) under src / tools\scene differ from the rig's commit $($c.Substring(0, 9)): $(($d | Select-Object -First 3) -join ', ')" } }
    return [pscustomobject]@{ Need = $false; Why = "the rig's commit $($c.Substring(0, 9)) has HEAD's src and tools\scene" }
}

function Test-TipSrcClean([string]$Main) {
    # $true when the main checkout has no uncommitted edit (tracked or untracked) under src / tools\scene: a rig built then draws exactly HEAD's sources.
    $s = @(& git -C $Main status --porcelain -- src tools/scene 2>$null | Where-Object { "$_".Trim() })
    return ($LASTEXITCODE -eq 0 -and -not $s.Count)
}
