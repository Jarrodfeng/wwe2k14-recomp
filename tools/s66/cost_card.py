"""Session 66: the per-scene "iGPU cost card" - what a weak GPU would pay, independent of the 4090's clocks.

    py tools/s66/cost_card.py CARD_DIR                 print one card
    py tools/s66/cost_card.py BEFORE_DIR AFTER_DIR     print both and the delta per row (absolute, percent)
    py tools/s66/cost_card.py --scenes brock,match --tags s65_base[,s66_x]   cards out\\s66\\cards\\<tag>_<scene>
    add --json for machine-readable rows instead of the table

CARD_DIR = out\\s66\\cards\\<Tag>_<scene>\\ made by tools\\s66\\nsight_scene.ps1: BASE\\GPUTRACE_FRAME.xls (Nsight Graphics
GPU Trace export of a few frames) + meta.json (scene, start frame, companion run number).
Rows from Nsight (mean over the traced frames): frame ms (INDICATIVE only: the 4090's clock state moves it up to 25%),
estimated DRAM MB a frame = (read% + write%)/100 x 1008 GB/s x frame time (ESTIMATED: the % rows are Nsight's, the
peak is the RTX 4090 spec constant below), PCIe MB a frame (direct byte sums), L2 throughput %, L2 sector traffic %
(device aperture), wait-for-idle %, go-idle commands (drains), pixel shader barriers, draws, dispatches. Counts, bytes
and drains reproduce between runs; ms does not.
Rows from the game log of the companion run (no Nsight; --gpu_record_stats=30, [native][frame] / [native][truth] lines),
averaged over the 60-frame windows around the traced frames, weighted by overlap: truth passes, resolves, write-backs,
render pass begins, barrier batches, draws, dispatches.
Reads our own trace exports and run logs only (nothing from the game is stored). Exit 0."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'perf'))
from nsight_summary import load  # noqa: E402

R = r'C:\Users\Jarrod\wwe2k14-recomp'
LOG = R + r'\out\build\win-amd64-release\logs\wwe2k14_{}.log'
DRAM_PEAK_GBS = 1008.0  # RTX 4090 spec (21 Gbps x 384 bit); the estimate is only as good as this constant
CARDS = R + r'\out\s66\cards'

# (label, kind, decimals); kind 'exact' = a count or byte sum, 'est' = derived estimate, 'ind' = clock-dependent
ROWS = [
    ('frame ms (indicative)', 'ind', 2),
    ('DRAM MB/frame (estimated)', 'est', 1),
    ('  of it read (estimated)', 'est', 1),
    ('  of it write (estimated)', 'est', 1),
    ('PCIe read MB/frame', 'exact', 2),
    ('PCIe write MB/frame', 'exact', 2),
    ('L2 throughput %', 'ind', 1),
    ('L2 sectors, device aperture %', 'ind', 1),
    ('wait-for-idle stall %', 'ind', 1),
    ('go-idle commands (drains)', 'exact', 1),
    ('pixel shader barriers', 'exact', 1),
    ('draws (Nsight)', 'exact', 1),
    ('dispatches (Nsight)', 'exact', 1),
    ('truth passes / frame', 'exact', 2),
    ('resolves / frame', 'exact', 2),
    ('write-backs / frame', 'exact', 2),
    ('render pass begins / frame', 'exact', 2),
    ('barrier batches / frame', 'exact', 2),
    ('draws / frame (log)', 'exact', 1),
    ('dispatches / frame (log)', 'exact', 2),
]


def mean(v):
    return sum(v) / len(v) if v else None


def nsight_rows(carddir):
    vals, clock = load(carddir)

    def get(key):
        for name, v in vals.items():
            if name == key or name.endswith('.' + key):
                return v
        return None

    out = {}
    cyc = get('gpc__cycles_elapsed.avg')
    ms = None
    if cyc and clock:
        ms = [c / (g * 1e9) * 1e3 for c, g in zip(cyc, clock)]
        out['frame ms (indicative)'] = mean(ms)
    rd = get('dramc__read_throughput.avg.pct_of_peak_sustained_elapsed')
    wr = get('dramc__write_throughput.avg.pct_of_peak_sustained_elapsed')
    if ms and rd and wr:
        n = min(len(ms), len(rd), len(wr))
        r_mb = [rd[i] / 100 * DRAM_PEAK_GBS * ms[i] for i in range(n)]  # GB/s x ms = MB
        w_mb = [wr[i] / 100 * DRAM_PEAK_GBS * ms[i] for i in range(n)]
        out['  of it read (estimated)'] = mean(r_mb)
        out['  of it write (estimated)'] = mean(w_mb)
        out['DRAM MB/frame (estimated)'] = mean(r_mb) + mean(w_mb)
    for label, key in (('PCIe read MB/frame', 'pcie__read_bytes.sum'), ('PCIe write MB/frame', 'pcie__write_bytes.sum')):
        v = get(key)
        if v:
            out[label] = mean(v) / 1e6
    for label, key in (('L2 throughput %', 'lts__throughput.avg.pct_of_peak_sustained_elapsed'),
                       ('L2 sectors, device aperture %',
                        'lts__t_sector_throughput_aperture_device.avg.pct_of_peak_sustained_elapsed'),
                       ('wait-for-idle stall %', 'fe__cycles_stalled_cmd_wfi_queue_sync.avg.pct_of_peak_sustained_elapsed'),
                       ('go-idle commands (drains)', 'fe__output_ops_cmd_go_idle_queue_sync.sum'),
                       ('pixel shader barriers', 'fe__pixel_shader_barriers.sum'),
                       ('draws (Nsight)', 'fe__draw_count.sum'), ('dispatches (Nsight)', 'gr__dispatch_count.sum')):
        v = get(key)
        if v:
            out[label] = mean(v)
    return out


WIN = re.compile(r'frames (\d+)-(\d+)')


def census_rows(run, lo, hi):
    """Overlap-weighted mean of the per-frame figures of every log window touching [lo, hi]."""
    acc = {}  # label -> [sum(value*w), sum(w)]

    def add(label, value, a, b):
        w = min(b, hi) - max(a, lo) + 1
        if w > 0:
            s = acc.setdefault(label, [0.0, 0.0])
            s[0] += value * w
            s[1] += w

    try:
        f = open(LOG.format(run), encoding='utf-8', errors='replace')
    except OSError:
        return {}

    def num(pat, s):
        m = re.search(pat, s)
        return float(m.group(1)) if m else None

    for line in f:
        if '[native][frame]' in line:
            m = WIN.search(line)
            if m:
                a, b = int(m[1]), int(m[2])
                for label, pat in (('resolves / frame', r'; resolves ([\d.]+)'),
                                   ('write-backs / frame', r'; write-backs ([\d.]+)')):
                    v = num(pat, line)
                    if v is not None:
                        add(label, v, a, b)
        elif '[native][truth] frames' in line:
            m = WIN.search(line)
            v = num(r'per frame: passes ([\d.]+)', line)
            if m and v is not None:
                add('truth passes / frame', v, int(m[1]), int(m[2]))
        elif '[s20][RECSTATS] frames' in line:
            m = WIN.search(line)
            if m:
                a, b = int(m[1]), int(m[2])
                for label, pat in (('render pass begins / frame', r'rendering begins ([\d.]+)'),
                                   ('barrier batches / frame', r'barrier batches ([\d.]+)'),
                                   ('draws / frame (log)', r', draws ([\d.]+)'),
                                   ('dispatches / frame (log)', r'dispatches ([\d.]+)')):
                    v = num(pat, line)
                    if v is not None:
                        add(label, v, a, b)
    return {k: s[0] / s[1] for k, s in acc.items() if s[1] > 0}


def build(carddir):
    meta = {}
    p = os.path.join(carddir, 'meta.json')
    if os.path.exists(p):
        meta = json.load(open(p, encoding='utf-8-sig'))
    rows = nsight_rows(carddir)
    if meta.get('run'):
        s, n = int(meta.get('start', 0)), int(meta.get('frames', 3))
        rows.update(census_rows(meta['run'], s - 30, s + n + 30))
    return meta, rows


def fmt(v, d):
    return '-' if v is None else f'{v:.{d}f}'


def groups_from(args):
    """-> list of groups (one per card column), each a list of card dirs (one per scene)."""
    if '--scenes' in args:
        sc = args[args.index('--scenes') + 1].split(',')
        tags = args[args.index('--tags') + 1].split(',')
        return [[os.path.join(CARDS, f'{t}_{s}') for s in sc] for t in tags]
    skip = set()
    for i, a in enumerate(args):
        if a in ('--scenes', '--tags'):
            skip.update((i, i + 1))
    return [[a] for i, a in enumerate(args) if i not in skip and not a.startswith('--')]


def show(groups, as_json):
    cards = [[build(d) for d in g] for g in groups]
    if as_json:
        print(json.dumps([[{'dir': d, 'meta': m, 'rows': r} for d, (m, r) in zip(g, cg)]
                          for g, cg in zip(groups, cards)], indent=1))
        return
    ncol = len(groups)
    for si in range(len(groups[0])):
        names = [os.path.basename(os.path.normpath(g[si])) for g in groups]
        meta0 = cards[0][si][0]
        print(f'\n== {meta0.get("scene", names[0])}   ({", ".join(names)}) ==')
        if meta0:
            print('   traced frames %s..+%s, companion run %s' % (meta0.get('start'), meta0.get('frames'), meta0.get('run')))
        head = f'{"row":32}' + ''.join(f' | {n[:22]:>22}' for n in names)
        if ncol == 2:
            head += f' | {"delta":>10} | {"delta %":>8}'
        print(head)
        print('-' * len(head))
        for label, kind, dec in ROWS:
            vs = [cards[c][si][1].get(label) for c in range(ncol)]
            if all(v is None for v in vs):
                continue
            line = f'{label:32}' + ''.join(f' | {fmt(v, dec):>22}' for v in vs)
            if ncol == 2:
                if vs[0] is not None and vs[1] is not None:
                    d = vs[1] - vs[0]
                    pct = '%+.1f%%' % (100.0 * d / vs[0]) if vs[0] else ('0.0%' if d == 0 else 'n/a')
                    line += f' | {d:>+10.{dec}f} | {pct:>8}'
                else:
                    line += f' | {"-":>10} | {"-":>8}'
            print(line)
    print('\n(estimated) = Nsight % x 1008 GB/s x frame time; (indicative) / % rows are clock-dependent: do not rank by them.')


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    groups = groups_from(args)
    if not groups or len(groups) > 2:
        print('give one card (print) or two (before, after)')
        return 1
    show(groups, '--json' in args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
