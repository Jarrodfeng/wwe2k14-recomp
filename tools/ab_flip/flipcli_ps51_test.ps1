$src = Get-Content 'C:\Users\Jarrod\wwe2k14-recomp\tools\surface\bench.ps1' -Raw
$fn = [regex]::Match($src, '(?s)function Get-AbFlipCli.*?\n\}').Value
Invoke-Expression $fn
$AbFlip = 'period=60/settle=15/anchor=2/off=wwe2k14_rdr_ko_draw_none=0/on=wwe2k14_rdr_ko_draw_none=1'
foreach ($arm in 'A','B') {
  $fl = @('--a=1') + $(if ($AbFlip) { @(Get-AbFlipCli $AbFlip) + @("--wwe2k14_ab_flip=$AbFlip" + $(if ($arm -eq 'B') { '/first=on' } else { '' })) } else { @() })
  "arm $arm count $($fl.Count): " + ($fl -join ' | ')
  if ($fl.Count -ne 3) { 'FAIL'; exit 1 }
}
$old = @('--a=1') + $((Get-AbFlipCli $AbFlip) + @('--x=y'))
"old form count $($old.Count) (plant: must be 2, the concatenation bug)"
if ($old.Count -ne 2) { 'FAIL plant'; exit 1 }
'PASS'
