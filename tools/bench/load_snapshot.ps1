# Session 101 lane 302: what else was running on the PC when a game run started and ended (run 7202: 47-62 s C: stalls while other agents
# compiled; nothing in the run record said so). Dot-source it:  . tools\bench\load_snapshot.ps1
#   Get-LoadSnapshot            -> one object: time, free RAM, and the heavy processes (name, count, CPU seconds so far, working set)
#   Format-LoadSnapshot $snap   -> one line: "clang x3 (cpu 412 s, 1800 MB); ninja x1 (...); free 21034 MB"
#   Save-RunLoad $log $start $end -> writes out\harness\run_load\<log>.txt (start snapshot, end snapshot, and the CPU seconds each process
#                                    group used BETWEEN them) and returns the short line run_list prints under the result.
# Processes counted: clang, clang++, clang-cl, lld-link, ninja, cmake, cl, link, sccache, rplay, fview, wwe2k14, python, py, pwsh, powershell.
# Read the end-minus-start CPU of clang/ninja with the run's [perf][sys] lines (sys_cpu high, proc_cpu low = the PC was busy with them).
$script:LoadNames = @('clang', 'clang++', 'clang-cl', 'lld-link', 'ninja', 'cmake', 'cl', 'link', 'sccache', 'rplay', 'fview', 'wwe2k14', 'python', 'py', 'pwsh', 'powershell')

function Get-LoadSnapshot {
    $snap = [ordered]@{ time = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); free_mb = $null; groups = [ordered]@{}; pids = @{} }
    try { $snap.free_mb = [int]((Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreePhysicalMemory / 1024) } catch { }
    try {
        foreach ($g in (Get-Process -ErrorAction SilentlyContinue | Where-Object { $script:LoadNames -contains $_.ProcessName } | Group-Object ProcessName)) {
            $cpu = 0.0; $ws = 0L
            foreach ($p in $g.Group) { $c = 0.0; try { $c = [double]$p.TotalProcessorTime.TotalSeconds } catch { }; $cpu += $c; $snap.pids["$($g.Name):$($p.Id)"] = $c; try { $ws += $p.WorkingSet64 } catch { } }
            $snap.groups[$g.Name] = [ordered]@{ count = $g.Count; cpu_s = [math]::Round($cpu, 1); ws_mb = [int]($ws / 1MB) }
        }
    } catch { }
    return [pscustomobject]$snap
}

function Format-LoadSnapshot($Snap) {
    $parts = foreach ($k in $Snap.groups.Keys) { $v = $Snap.groups[$k]; "$k x$($v.count) (cpu $($v.cpu_s) s, $($v.ws_mb) MB)" }
    $s = if ($parts) { $parts -join '; ' } else { '(none of the watched processes)' }
    return "$s; free $($Snap.free_mb) MB"
}

function Save-RunLoad([string]$Log, $Start, $End) {
    $short = ''
    try {
        $root = if ($env:WWE2K14_ROOT) { $env:WWE2K14_ROOT } else { 'C:\Users\Jarrod\wwe2k14-recomp' }
        $dir = Join-Path $root 'out\harness\run_load'
        New-Item -ItemType Directory -Force $dir | Out-Null
        $lines = @("run: $Log", "start $($Start.time): $(Format-LoadSnapshot $Start)", "end   $($End.time): $(Format-LoadSnapshot $End)")
        $busy = @()
        $delta = @{}   # CPU seconds per process name between the snapshots: per PID (a PID in both: the difference; new in the end snapshot: all of it).
        foreach ($k in $End.pids.Keys) {   # (a process that exited in between is in neither delta: its CPU is not counted, so this is a lower bound)
            $name2 = $k.Split(':')[0]
            $a = if ($Start.pids.ContainsKey($k)) { $Start.pids[$k] } else { 0 }
            $delta[$name2] = [double]$delta[$name2] + [math]::Max(0.0, $End.pids[$k] - $a)
        }
        foreach ($k in ($delta.Keys | Sort-Object)) {
            $d = [math]::Round($delta[$k], 1)
            $lines += "  cpu used by $k processes alive at the end, between the snapshots: $d s"
            if ($k -ne 'wwe2k14' -and $d -ge 5) { $busy += "$k +$d s" }
        }
        $name = if ($Log) { ($Log -replace ',', '+') } else { 'nolog_' + (Get-Date -Format 'HHmmss') }
        [IO.File]::WriteAllText((Join-Path $dir "$name.txt"), (($lines -join "`r`n") + "`r`n"))
        $short = "    [load] other CPU during the run: $(if ($busy) { $busy -join ', ' } else { 'none >= 5 s' }); free RAM $($Start.free_mb) -> $($End.free_mb) MB; out\harness\run_load\$name.txt"
    } catch { }
    return $short
}
