# Session 46: per run, the truth census summed over the run: frames, replaced, truth frames, fallbacks by reason,
# EDRAM accesses per frame in windows that were all truth vs the others.  py truth_census.py RUN...
import re, sys, collections
LOG = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\wwe2k14_{}.log'
ed = re.compile(r'\[native\]\[edram\] frames (\d+)-(\d+) \((\d+) replaced, (\d+) truth\).*EDRAM accesses ([\d.]+)')
tr = re.compile(r'\[native\]\[truth\] frames \d+-\d+: truth frames (\d+), fallbacks (\d+) \(([^)]*)\)')
for run in sys.argv[1:]:
    frames = replaced = truth = fb = 0
    reasons = collections.Counter()
    acc_truth = []; acc_other = []
    try:
        lines = open(LOG.format(run), encoding='utf-8', errors='replace')
    except OSError:
        print(f'{run}: no log'); continue
    for l in lines:
        m = ed.search(l)
        if m:
            a, b, r, t, acc = int(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5])
            n = b - a + 1
            frames += n; replaced += r; truth += t
            (acc_truth if t == n else acc_other).append(acc)
            continue
        m = tr.search(l)
        if m:
            fb += int(m[2])
            if m[3] != 'none':
                for part in m[3].split(', '):
                    name, _, cnt = part.rpartition(' ')
                    reasons[name] += int(cnt)
    avg = lambda v: sum(v) / len(v) if v else 0.0
    print(f'{run}: frames {frames}, replaced {replaced}, truth {truth} ({100.0 * truth / max(1, frames):.1f}%); '
          f'fallbacks {fb}' + (f' ({", ".join(f"{k} {v}" for k, v in reasons.most_common())})' if reasons else '') +
          f'; EDRAM accesses/frame: all-truth windows {avg(acc_truth):.1f} ({len(acc_truth)}), others {avg(acc_other):.1f} ({len(acc_other)})')
