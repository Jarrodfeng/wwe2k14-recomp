# S109 SCENE: run_list's door for scene requests (docs\systems\run_contract.md "Scene requests"; design docs\research\s109\SCENE_plan.md).
#   . tools\bench\run_list_scene.ps1; Resolve-RunListScene 'scene:Cena vs Lesnar; blood=on // at bell+120f: passive 1 on'
# A -Scripts entry that starts with `scene:` (the request inline, lines joined by ` // `) or ends with `.scene` (a request file) is lowered by
# tools\run\scene.py into a route under out\scenes\ and replaced by that route's path; every other entry is returned unchanged. A request the generator
# refuses (a combination neither start door serves, a moment with no in-route state, a route the lint refuses fast-forwarded) returns 'ABORT: ...'
# with the reason: the caller (run_list.ps1) prints it and exits 2, as for a missing script. $Exe / $Data are the run's -Exe / -Data: the generator
# only checks the request against them (an exe without the cvars the start needs; save=NAME against -Data), it never changes them.
function Resolve-RunListScene([string]$Arg, [string]$Exe = '', [string]$Data = '') {
    $isFile = $Arg -match '\.scene\s*$'
    if (-not ($isFile -or $Arg -match '^\s*scene:')) { return $Arg }
    if ($isFile -and -not (Test-Path -LiteralPath $Arg -PathType Leaf)) { return "ABORT: scene file not found: $Arg" }
    if (-not (Get-Command py -ErrorAction SilentlyContinue)) { return 'ABORT: a scene request needs py (not found)' }
    $a = @('-I', (Join-Path $PSScriptRoot '..\run\scene.py'), '--write', $Arg)
    if ($Exe) { $a += @('--exe', $Exe) }
    if ($Data) { $a += @('--data', $Data) }
    $out = @(& py @a 2>&1 | ForEach-Object { "$_" } | Where-Object { $_.Trim() })
    $code = $LASTEXITCODE
    $last = if ($out.Count) { $out[-1].Trim() } else { '' }
    if ($code -ne 0 -or $last -notmatch '\.txt$') { return "ABORT: scene request refused: $(if ($last) { $last } else { 'scene.py printed nothing' })" }
    return $last
}
