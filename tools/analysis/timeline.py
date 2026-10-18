"""Per-frame breakdown of a frame-pacing timeline (logs/timeline_NNN.csv from --wwe2k14_timeline=true).

    py tools/analysis/timeline.py <timeline.csv> [--frames A-B] [--slow-ms 45] [--dump N] [--list 20]

Threads are identified by their events: the game's main thread logs `vdswap`, ReXGlue's GPU command
processor logs `cp_swap`, the vblank thread logs `vblank`.

For each guest frame (VdSwap to VdSwap on the main thread):
  main: time the game spent in its GPU waits (gwait, by reason code), and how often it kicked the GPU (wptr).
For each GPU-side frame (cp_swap to cp_swap on the command processor thread):
  idle  - out of commands (waiting for the game to submit more)
  wrm   - WAIT_REG_MEM (waiting for a value in memory/register, usually written by the game's CPU)
  hwait - waiting for the host GPU to finish a submission (frames in flight)
  intr  - running the game's interrupt callback on the CP thread
  busy  - everything else (command translation, Vulkan recording, submits)
`--dump N` prints every event in guest frame N with times relative to its start.
"""
import argparse
import bisect
import csv
import statistics
from collections import defaultdict


def load(path):
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r['t_ns']), int(r['thread']), r['event'],
                             int(r['a'], 16), int(r['b'], 16), int(r['c'], 16)))
            except (ValueError, KeyError):
                break  # truncated last line of a crashed run
    rows.sort(key=lambda x: x[0])
    return rows


def pair_intervals(rows):
    """X_b / X_e pairs on the same thread -> {X: [(thread, t0, t1, begin_args, end_args)]}."""
    open_ = defaultdict(list)
    out = defaultdict(list)
    for t, th, ev, a, b, c in rows:
        if ev.endswith('_b'):
            open_[(th, ev[:-2])].append((t, (a, b, c)))
        elif ev.endswith('_e'):
            stack = open_.get((th, ev[:-2]))
            if stack:
                t0, args0 = stack.pop()
                out[ev[:-2]].append((th, t0, t, args0, (a, b, c)))
    for k in out:
        out[k].sort(key=lambda x: x[1])
    return out


def clipped(intervals, thread, lo, hi, key=None):
    """Total ns of intervals on `thread` inside [lo, hi), optionally grouped by key(interval)."""
    total = defaultdict(int)
    for iv in intervals:
        th, t0, t1 = iv[0], iv[1], iv[2]
        if th != thread or t1 <= lo or t0 >= hi:
            continue
        total[key(iv) if key else None] += min(t1, hi) - max(t0, lo)
    return total


def ms(ns):
    return ns / 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--frames', default='', help='guest frame range A-B')
    ap.add_argument('--slow-ms', type=float, default=45.0)
    ap.add_argument('--dump', type=int, default=0)
    ap.add_argument('--list', type=int, default=20, help='per-frame rows to print')
    args = ap.parse_args()

    rows = load(args.csv)
    by_ev = defaultdict(list)
    for r in rows:
        by_ev[r[2]].append(r)
    main_th = by_ev['vdswap'][0][1] if by_ev['vdswap'] else None
    cp_th = by_ev['cp_swap'][0][1] if by_ev['cp_swap'] else None
    vs_th = by_ev['vblank'][0][1] if by_ev['vblank'] else None
    print(f'{len(rows)} events, {ms(rows[-1][0] - rows[0][0]) / 1000:.1f} s; threads: main {main_th}, '
          f'command processor {cp_th}, vblank {vs_th}')
    print('event counts: ' + ', '.join(f'{k} {len(v)}' for k, v in sorted(by_ev.items())))
    for r in by_ev.get('fence_ptr', []):
        print(f'fence counter at guest 0x{r[3]:08X} (device 0x{r[4]:08X})')

    iv = pair_intervals(rows)
    vd = [(r[0], r[3]) for r in by_ev['vdswap']]  # (t, frame)
    cps = [r[0] for r in by_ev['cp_swap']]
    vbl = [r[0] for r in by_ev['vblank']]
    wptr_t = [r[0] for r in by_ev['wptr']]

    lo_f, hi_f = 0, 1 << 60
    if args.frames:
        lo_f, hi_f = (int(x) for x in args.frames.split('-'))

    # Pair the k-th VdSwap with the k-th cp_swap (one XE_SWAP packet per VdSwap): only valid if counts line up.
    offset = None
    if vd and cps:
        # cp_swap must come after its VdSwap; find the smallest shift that makes that true for the first frames.
        for shift in range(0, 5):
            ok = all(cps[i + shift] >= vd[i][0] for i in range(min(50, len(vd) - 1, len(cps) - shift - 1)))
            if ok:
                offset = shift
                break

    frames = []
    for i in range(1, len(vd)):
        t0, t1, f = vd[i - 1][0], vd[i][0], vd[i][1]
        if not (lo_f <= f <= hi_f):
            continue
        g = clipped(iv.get('gwait', []), main_th, t0, t1, key=lambda x: x[3][0])
        kicks = bisect.bisect_left(wptr_t, t1) - bisect.bisect_left(wptr_t, t0)
        cp = None
        if offset is not None and i + offset < len(cps) and i - 1 + offset >= 0:
            c0, c1 = cps[i - 1 + offset], cps[i + offset]
            parts = {k: sum(clipped(iv.get(k, []), cp_th, c0, c1).values())
                     for k in ('cp_idle', 'wrm', 'hwait', 'intr', 'endsub', 'vswap')}
            busy = (c1 - c0) - parts['cp_idle'] - parts['wrm'] - parts['hwait'] - parts['intr']
            cp = dict(span=c1 - c0, busy=busy, lag=c1 - t1, **parts)
        frames.append(dict(frame=f, t0=t0, t1=t1, ms=ms(t1 - t0), gwait=g, kicks=kicks, cp=cp))

    if not frames:
        print('no frames in range')
        return

    def summarize(label, fs):
        if not fs:
            return
        print(f'\n== {label}: {len(fs)} frames, guest frame {statistics.mean(f["ms"] for f in fs):.1f} ms avg ==')
        reasons = defaultdict(list)
        for f in fs:
            for k, v in f['gwait'].items():
                reasons[k].append(ms(v))
        for k in sorted(reasons):
            print(f'  main thread gwait reason {k}: {sum(reasons[k]) / len(fs):6.2f} ms/frame '
                  f'(in {len(reasons[k])} of {len(fs)} frames)')
        print(f'  GPU kicks (wptr) per frame: {statistics.mean(f["kicks"] for f in fs):.1f}')
        cps_ = [f['cp'] for f in fs if f['cp']]
        if cps_:
            keys = ('span', 'busy', 'cp_idle', 'wrm', 'hwait', 'intr', 'endsub', 'vswap', 'lag')
            print('  command processor per frame (ms):  ' +
                  '  '.join(f'{k} {statistics.mean(ms(c[k]) for c in cps_):.2f}' for k in keys))

    summarize('all frames', frames)
    summarize(f'slow frames (>= {args.slow_ms} ms)', [f for f in frames if f['ms'] >= args.slow_ms])
    summarize('30 fps frames (25-40 ms)', [f for f in frames if 25 <= f['ms'] < 40])
    summarize('60 fps frames (< 25 ms)', [f for f in frames if f['ms'] < 25])

    # What releases the CP's WAIT_REG_MEM waits: time from the latest vblank to each wrm end.
    wrm = [x for x in iv.get('wrm', []) if x[0] == cp_th]
    if wrm and vbl:
        addr = defaultdict(int)
        deltas = []
        for th, t0, t1, a0, a1 in wrm:
            addr[a0[0]] += 1
            j = bisect.bisect_right(vbl, t1) - 1
            if j >= 0:
                deltas.append(ms(t1 - vbl[j]))
        print(f'\nWAIT_REG_MEM waits: {len(wrm)}; addresses: ' +
              ', '.join(f'0x{k:08X} x{v}' for k, v in sorted(addr.items(), key=lambda x: -x[1])[:4]))
        deltas.sort()
        print(f'  time from latest vblank to wait end: median {deltas[len(deltas) // 2]:.2f} ms, '
              f'p10 {deltas[len(deltas) // 10]:.2f}, p90 {deltas[9 * len(deltas) // 10]:.2f}')
        durs = sorted(ms(t1 - t0) for _, t0, t1, _, _ in wrm)
        print(f'  wait length: median {durs[len(durs) // 2]:.2f} ms, p90 {durs[9 * len(durs) // 10]:.2f}, '
              f'max {durs[-1]:.2f}')

    print(f'\nper frame (first {args.list} in range; times in ms; lag = cp_swap minus VdSwap):')
    for f in frames[:args.list]:
        g = ' '.join(f'r{k}:{ms(v):.1f}' for k, v in sorted(f['gwait'].items()))
        c = f['cp']
        cs = (f'cp span {ms(c["span"]):5.1f} busy {ms(c["busy"]):5.1f} idle {ms(c["cp_idle"]):5.1f} '
              f'wrm {ms(c["wrm"]):5.1f} hwait {ms(c["hwait"]):4.1f} lag {ms(c["lag"]):5.1f}') if c else ''
        print(f'  frame {f["frame"]:6d} {f["ms"]:5.1f} | gwait {g:28s} kicks {f["kicks"]:3d} | {cs}')

    if args.dump:
        fr = next((f for f in frames if f['frame'] == args.dump), None)
        if fr:
            print(f'\nevents in guest frame {args.dump} ({fr["ms"]:.1f} ms), relative to its start:')
            names = {main_th: 'main', cp_th: 'cp', vs_th: 'vsync'}
            for t, th, ev, a, b, c in rows:
                if fr['t0'] <= t < fr['t1'] and ev not in ('memw',):
                    print(f'  {ms(t - fr["t0"]):8.3f} {names.get(th, th):>6} {ev:12s} 0x{a:X} 0x{b:X} 0x{c:X}')


if __name__ == '__main__':
    main()
