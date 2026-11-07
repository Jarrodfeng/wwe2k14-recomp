"""How many drawn frames reached a present, from a run's timeline (Session 95). Works on old runs too.
Answers: swaps a second against presents a second per stretch; how the skipped frames fall (long runs of every second frame or
scattered); how old a frame was when presented; where a slow paint spent its time.
  py tools\\native\\present_share.py TIMELINE_CSV [FROM_S TO_S]    (default: the run's marks, else the whole run)
Timeline events: vdswap (the game's swap), gorefresh (a = the frame's refresh_seq), hpresent (a = refresh_seq of the frame
presented; after vkQueuePresentKHR), pacq_*, pfence_*, and from Session 95 pcons / prec / pfa / psub / ppres between taking a
frame and presenting it, pgate_b / pgate_e (present_every_frame)."""
import bisect
import collections
import csv
import statistics as st
import sys

KEEP = {'vdswap', 'gorefresh', 'hpresent', 'mark', 'pacq_end', 'pcons', 'prec', 'pfa', 'psub', 'ppres', 'pgate_b', 'pgate_e'}
ev = collections.defaultdict(list)
with open(sys.argv[1], newline='') as f:
    for r in csv.DictReader(f):
        if r['event'] in KEEP:
            ev[r['event']].append((int(r['t_ns']) / 1e9, int(r['a'], 16)))
if len(sys.argv) >= 4:
    lo, hi = float(sys.argv[2]), float(sys.argv[3])
elif len(ev['mark']) >= 2:
    lo, hi = ev['mark'][0][0], ev['mark'][-1][0]
else:
    lo, hi = ev['vdswap'][0][0], ev['vdswap'][-1][0]


def within(name):
    return [x for x in ev[name] if lo <= x[0] <= hi]


swaps, refresh, pres = within('vdswap'), within('gorefresh'), within('hpresent')
secs = hi - lo
print(f'{lo:.1f}-{hi:.1f} s: {len(swaps) / secs:.1f} swaps a second, {len(pres) / secs:.1f} presents a second; '
      f'{100 * len(pres) / max(len(refresh), 1):.0f}% of the frames drawn reached a present')
gaps = [pres[i + 1][1] - pres[i][1] for i in range(len(pres) - 1)]
hist = collections.Counter(min(g, 5) for g in gaps)
print('frames between presents (1 = none skipped):', ', '.join(f'{k}: {hist[k]}' for k in sorted(hist)))
runs, cur = [], 0
for g in gaps:
    if g >= 2:
        cur += 1
    elif cur:
        runs.append(cur)
        cur = 0
if cur:
    runs.append(cur)
if runs:
    print(f'skipping stretches: {len(runs)}, longest {max(runs)} presents in a row')
at = {seq: t for t, seq in refresh}
age = sorted((t - at[seq]) * 1000 for t, seq in pres if seq in at)
if age:
    print(f'frame age at its present: median {st.median(age):.1f} ms, 90% {age[len(age) * 9 // 10]:.1f} ms, worst {age[-1]:.1f} ms')
step = sorted((pres[i + 1][0] - pres[i][0]) * 1000 for i in range(len(pres) - 1))
if step:
    print(f'time between presents: median {st.median(step):.1f} ms, 99% {step[len(step) * 99 // 100]:.1f} ms (1% low {1000 / step[len(step) * 99 // 100]:.1f} fps)')
# Where the paint's time went: pacq_end -> pcons -> prec -> pfa -> psub -> ppres -> hpresent.
order = ['pacq_end', 'pcons', 'prec', 'pfa', 'psub', 'ppres', 'hpresent']
times = {n: [t for t, _ in within(n)] for n in order}
if times['pcons']:
    spans = collections.defaultdict(list)
    for t0 in times['pacq_end']:
        last, name = t0, 'pacq_end'
        for n in order[1:]:
            i = bisect.bisect_left(times[n], last)
            if i >= len(times[n]):
                break
            spans[name + '>' + n].append((times[n][i] - last) * 1000)
            last, name = times[n][i], n
    for k, v in spans.items():
        v.sort()
        print(f'  paint step {k:18s} median {st.median(v):7.2f} ms  90% {v[len(v) * 9 // 10]:7.2f}  worst {v[-1]:7.2f}')
else:
    v = []
    for t0 in times['pacq_end']:
        i = bisect.bisect_left(times['hpresent'], t0)
        if i < len(times['hpresent']):
            v.append((times['hpresent'][i] - t0) * 1000)
    if v:
        v.sort()
        print(f'  paint from acquire to present: median {st.median(v):.2f} ms, 90% {v[len(v) * 9 // 10]:.2f}, worst {v[-1]:.2f}; '
              f'{sum(1 for x in v if x > 5)} of {len(v)} over 5 ms')
gb, ge = within('pgate_b'), [t for t, _ in within('pgate_e')]
w = []
for t0, _ in gb:
    i = bisect.bisect_left(ge, t0)
    if i < len(ge):
        w.append((ge[i] - t0) * 1000)
if w:
    print(f'present_every_frame: {len(w)} waits, {sum(w) / secs / 10:.1f}% of the command thread, median {st.median(w):.2f} ms, worst {max(w):.2f} ms')
