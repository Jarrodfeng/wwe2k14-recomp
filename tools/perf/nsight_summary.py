"""Session 22 item 2: the key numbers of Nsight GPU Trace exports side by side (one column per traced frame).

    py tools/perf/nsight_summary.py DIR [DIR ...]

DIR = a folder holding BASE\\GPUTRACE_FRAME.xls (tab-separated: metric, then one value per traced frame), as written by
tools\\perf\\s20_nsight_trace.ps1 with --auto-export (move BASE out of out\\harness\\step20\\nsight after each trace: the
next trace overwrites it). Reads our own trace exports only."""
import os
import sys

ROWS = [
    ('frame ms (base clock)', 'gpc__cycles_elapsed.avg', 'ms'),
    ('graphics engine active %', 'gr__cycles_active.avg.pct_of_peak_sustained_elapsed', ''),
    ('FE stalled on wait-for-idle %', 'fe__cycles_stalled_cmd_wfi_queue_sync.avg.pct_of_peak_sustained_elapsed', ''),
    ('go-idle commands', 'fe__output_ops_cmd_go_idle_queue_sync.sum', ''),
    ('pixel shader barriers', 'fe__pixel_shader_barriers.sum', ''),
    ('draws', 'fe__draw_count.sum', ''),
    ('dispatches', 'gr__dispatch_count.sum', ''),
    ('SM throughput %', 'sm__throughput.avg.pct_of_peak_sustained_elapsed', ''),
    ('warps active: pixel %', 'tpc__warps_active_shader_ps_realtime.avg.pct_of_peak_sustained_elapsed', ''),
    ('warps active: vertex %', 'tpc__warps_active_shader_vtg_realtime.avg.pct_of_peak_sustained_elapsed', ''),
    ('warps active: compute %', 'tpc__warps_active_shader_cs_realtime.avg.pct_of_peak_sustained_elapsed', ''),
    ('SM idle, no warps %', 'tpc__warps_inactive_sm_idle_realtime.avg.pct_of_peak_sustained_elapsed', ''),
    ('L2 throughput %', 'lts__throughput.avg.pct_of_peak_sustained_elapsed', ''),
    ('DRAM throughput %', 'dramc__throughput.avg.pct_of_peak_sustained_elapsed', ''),
    ('L1 / texture %', 'l1tex__throughput.avg.pct_of_peak_sustained_elapsed', ''),
    ('PCIe %', 'pcie__throughput.avg.pct_of_peak_sustained_elapsed', ''),
]


def load(d):
    vals = {}
    clock = None
    for line in open(os.path.join(d, 'BASE', 'GPUTRACE_FRAME.xls'), encoding='utf-8', errors='replace'):
        p = line.rstrip('\r\n').split('\t')
        if len(p) < 2:
            continue
        name = p[0].split('TriageAC.')[-1]
        try:
            v = [float(x) for x in p[1:] if x != '']
        except ValueError:
            continue
        vals.setdefault(name, v)
        if name == 'gpc__cycles_elapsed.avg.per_second':
            clock = v
    return vals, clock


def main():
    dirs = sys.argv[1:]
    data = [load(d) for d in dirs]
    print(f'{"metric":32}' + ''.join(f' | {os.path.basename(os.path.normpath(d)):>26}' for d in dirs))
    for label, key, kind in ROWS:
        cells = []
        for vals, clock in data:
            v = vals.get(key)
            if v is None:
                cells.append(f'{"-":>26}')
                continue
            if kind == 'ms' and clock:
                v = [c / (g * 1e9) * 1e3 for c, g in zip(v, clock)]
            fmt = '{:.0f}' if max(v) >= 1000 else '{:.1f}'
            cells.append(f'{" / ".join(fmt.format(x) for x in v):>26}')
        print(f'{label:32}' + ''.join(f' | {c}' for c in cells))


if __name__ == '__main__':
    main()
