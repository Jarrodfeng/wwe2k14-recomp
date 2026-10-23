# Session 55 part 2: the Quick Match sweep (tools\s55\qm_sweep.ps1) in small batches, so a build (which takes the run
# lock exclusively) waits at most one batch instead of the whole sweep. Appends each batch's report lines to -Out.
#   & .\tools\s55\qm_sweep_chunks.ps1 -Types 12,13,14,... [-Chunk 2] [-Parallel 2] [-Out out\s55\sweep_after.txt]
param([int[]]$Types, [int]$Chunk = 2, [int]$Parallel = 2, [string]$Out = 'C:\Users\Jarrod\wwe2k14-recomp\out\s55\sweep_after.txt',
      [string]$Exe = 'C:\Users\Jarrod\wwe2k14-recomp\out\s55\sweep_bin\wwe2k14.exe', [string]$Extra = '')
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
for ($i = 0; $i -lt $Types.Count; $i += $Chunk) {
    $batch = $Types[$i..([Math]::Min($i + $Chunk, $Types.Count) - 1)]
    "== batch $($batch -join ',') started $(Get-Date -Format 'HH:mm:ss')" | Out-File -Append -Encoding utf8 $Out
    & "$R\tools\s55\qm_sweep.ps1" -Types $batch -Parallel $Parallel -Exe $Exe -Extra $Extra 2>&1 |
        Where-Object { "$_" -match '^\d{3}: type|checks:|CRASH|crash|refus' } | Out-File -Append -Encoding utf8 $Out
}
"== all batches done $(Get-Date -Format 'HH:mm:ss')" | Out-File -Append -Encoding utf8 $Out
