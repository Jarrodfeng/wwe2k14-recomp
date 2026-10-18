"""60-fps entrance check (Session 7b). Usage: py tools/bench/fps60.py RUN [WINDOW=600]
Pass bar used in Session 7b: every 600-frame window >= 59.5 fps, < 0.5% of frames > 20 ms, wall time = real time.
Per scene (Brock / second entrance / match): frames, wall s, fps, frames > 20 / 25 / 35 ms, and per window: fps and
host GPU ms per frame (sum of gpu_ts submission durations between the window's first and last vdswap / frames)."""
import csv, re, sys, bisect
L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'
n = sys.argv[1].zfill(3)
W = int(sys.argv[2]) if len(sys.argv) > 2 else 600
anch = {}
for line in open(f'{L}/wwe2k14_{n}.log', encoding='utf-8', errors='replace'):
    m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
    if m:
        p = m.group(1).lower()
        for k in ('343.bik2', '103.bik2', '324.bik2', 'm3.pac'):
            if k in p and k not in anch:
                anch[k] = int(m.group(2))
frames = {}
for r in csv.DictReader(open(f'{L}/frames_{n}.csv')):
    frames[int(r['frame'])] = float(r['frame_ms'])
vd, ts = [], []
try:
    for r in csv.DictReader(open(f'{L}/timeline_{n}.csv', newline='')):
        try:
            e = r['event']
            if e == 'vdswap':
                vd.append((int(r['t_ns']), int(r['a'], 16)))
            elif e == 'gpu_ts':
                ts.append((int(r['t_ns']), int(r['b'], 16)))
        except Exception:
            break
except FileNotFoundError:
    pass
vd.sort(); ts.sort()
ts_t = [t for t, _ in ts]
ts_cum = [0]
for _, d in ts:
    ts_cum.append(ts_cum[-1] + d)


def t_of(frame):
    for t, f in vd:
        if f >= frame:
            return t
    return None


def gpu_ms_per_frame(a, b):
    if not vd or not ts:
        return float('nan')
    t0, t1 = t_of(a), t_of(b)
    if t0 is None or t1 is None:
        return float('nan')
    i, j = bisect.bisect_left(ts_t, t0), bisect.bisect_left(ts_t, t1)
    return (ts_cum[j] - ts_cum[i]) / 1000 / max(1, b - a)


def scene(a, b, label):
    sel = [(f, frames[f]) for f in range(a, b) if f in frames]
    ms = [m for _, m in sel]
    if not ms:
        print(f'run {n} {label}: no frames'); return
    print(f'run {n} {label} (frames {a}-{b}): {len(ms)} frames, wall {sum(ms) / 1000:.1f} s, {1000 * len(ms) / sum(ms):.2f} fps, '
          f'>20 ms {sum(m > 20 for m in ms)} ({100 * sum(m > 20 for m in ms) / len(ms):.2f}%), >25 {sum(m > 25 for m in ms)}, '
          f'>35 {sum(m > 35 for m in ms)}, GPU {gpu_ms_per_frame(a, b):.2f} ms/frame')
    out = []
    for k in range(0, len(sel), W):
        c = sel[k:k + W]
        cm = [m for _, m in c]
        out.append(f'{1000 * len(cm) / sum(cm):.1f}/{gpu_ms_per_frame(c[0][0], c[-1][0] + 1):.1f}')
    print(f'    per {W} frames fps/GPUms:', ' '.join(out))


second = '103.bik2' if '103.bik2' in anch else '324.bik2'
print('anchors', anch)
if '343.bik2' in anch and second in anch:
    scene(anch['343.bik2'], anch[second], 'Brock')
if second in anch and 'm3.pac' in anch:
    scene(anch[second], anch['m3.pac'] - 80, 'second entrance')
if 'm3.pac' in anch:
    last = max(frames)
    scene(anch['m3.pac'] + 120, last - 30, 'match')
