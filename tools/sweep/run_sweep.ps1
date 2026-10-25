# Session 10 (M5 sweep): runs one list from tools\sweep\make_lists.py through tools\bench\run_list.ps1 (frozen user
# data, run lock, shared lanes, crash triage), and records which run log belongs to which list item.
# Usage (one line):
#   pwsh -NoProfile -File C:\Users\Jarrod\wwe2k14-recomp\tools\sweep\run_sweep.ps1 -List arenas -Parallel 2 -WaitSeconds 600
#   ... -Only arena_04,arena_44     (a subset, by item name; -Skip the opposite)
#   ... -Tag t1                     (a label for this batch's record file)
#   ... -Extra '--a=1 --b=2'        (Session 11: flags for every run, ONE string, passed to run_list -Extra)
#   ... -Script PATH                (Session 11: run every item with this input script instead of the list's)
#   ... -KeepShaderStorage DIR      (Session 61: each run's shader storage copied to DIR, for the shader list)
# Output: the run_list result lines (one per item, input order) and out\harness\sweep\runs\<list>_<stamp>[_<tag>].json
# = {list, tag, started, ended, items: [{name, extra, meta, line, log}]}; tools\sweep\results.py reads these files.
param([Parameter(Mandatory)][string]$List, [int]$Parallel = 1, [int]$WaitSeconds = 720,
      [string[]]$Only = @(), [string[]]$Skip = @(), [string]$Tag = '', [string]$Extra = '', [string]$Script = '',
      [string]$KeepShaderStorage = '',   # Session 61: run_list -KeepShaderStorage (each run's shader storage kept)
      [string]$Exe = '')   # Session 61: run_list -Exe (a copy of the build, so builds need not wait; add --game_data_root)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$listFile = "$R\out\harness\sweep\lists\$List.json"
if (-not (Test-Path -LiteralPath $listFile)) { "ABORT: no list $listFile (run py tools\sweep\make_lists.py)"; exit 2 }
$items = @(([IO.File]::ReadAllText($listFile) | ConvertFrom-Json).items)
# -Only / -Skip accept comma-separated names too (pwsh -File passes 'a,b' as one string).
$only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$skip = @($Skip | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($only.Count) { $items = @($items | Where-Object { $only -contains $_.name }) }
if ($skip.Count) { $items = @($items | Where-Object { $skip -notcontains $_.name }) }
if (-not $items.Count) { "ABORT: no items selected from $List"; exit 2 }
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$runDir = "$R\out\harness\sweep\runs"
[void][IO.Directory]::CreateDirectory($runDir)
$recName = "${List}_$stamp$(if ($Tag) { "_$Tag" })"
$rec = Join-Path $runDir "$recName.json"
$started = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
Write-Host "[sweep] $List`: $($items.Count) runs, -Parallel $Parallel, -WaitSeconds $WaitSeconds; record $rec"
$scripts = @($items | ForEach-Object { if ($Script) { $Script } else { "$($_.script)" } })
$extras = @($items | ForEach-Object { "$($_.extra)" })
# Write the record before the runs too (lines empty), so an interrupted batch still names its items.
function Save-Record($lines, [string]$ended) {
    $out = [ordered]@{ list = $List; tag = $Tag; started = $started; ended = $ended; parallel = $Parallel; extra = $Extra; script = $Script
                       items = @(for ($i = 0; $i -lt $items.Count; $i++) {
                           $ln = if ($i -lt @($lines).Count) { "$($lines[$i])" } else { '' }
                           $log = if ($ln -match '-> (wwe2k14_\d+)') { $Matches[1] } else { '' }
                           [ordered]@{ name = $items[$i].name; extra = $items[$i].extra; meta = $items[$i].meta
                                       line = $ln; log = $log } }) }
    [IO.File]::WriteAllText($rec, ($out | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}
$exeArg = if ($Exe) { @{ Exe = $Exe } } else { @{} }
Save-Record @() ''
$out = & "$R\tools\bench\run_list.ps1" -Scripts $scripts -ScriptExtras $extras -Parallel $Parallel -WaitSeconds $WaitSeconds -Extra $Extra -KeepShaderStorage $KeepShaderStorage @exeArg
$lines = @($out | ForEach-Object { "$_" } | Where-Object { $_ -match ' -> ' -or $_ -match '^\S+\.txt ->' })
if ($lines.Count -ne $items.Count) {
    Write-Host "[sweep] WARNING: $($lines.Count) result lines for $($items.Count) items; raw output follows"
    $out | ForEach-Object { Write-Host "  $_" }
}
Save-Record $lines (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
for ($i = 0; $i -lt $items.Count; $i++) { "{0,-14} {1}" -f $items[$i].name, $(if ($i -lt $lines.Count) { $lines[$i] } else { '(no line)' }) }
"[sweep] record: $rec"
