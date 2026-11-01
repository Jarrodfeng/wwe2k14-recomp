# Session 78 (HD 520 arc, item 1): limits the next wwe2k14.exe that starts to the given logical processors, to stand in
# for a 2-core / 4-thread laptop CPU on this PC. Waits up to -WaitSeconds for the process, sets its affinity as soon
# as it appears (before the game threads start in earnest), prints the pid and mask, and exits. Run it in the
# background just before the run. Mask 0xF = logical 0-3 = cores 0 and 1 with their SMT siblings on this Ryzen.
#   pwsh -File tools\perf\pin_affinity.ps1 -Mask 0xF
param([int64]$Mask = 0xF, [int]$WaitSeconds = 600)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$before = @(Get-Process wwe2k14 -ErrorAction SilentlyContinue | ForEach-Object Id)
while ((Get-Date) -lt $deadline) {
    $p = Get-Process wwe2k14 -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
    if ($p) {
        try { $p.ProcessorAffinity = [IntPtr]$Mask; "pinned pid $($p.Id) to mask 0x{0:X}" -f $Mask }
        catch { "could not pin pid $($p.Id): $_" }
        exit 0
    }
    Start-Sleep -Milliseconds 20
}
"no new wwe2k14.exe within $WaitSeconds s"
exit 1
