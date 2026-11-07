"""Session 95: whole-frame GPU ms (submission spans, so work hoisted to the head of a submission is in it) and frame ms per scene window, two runs of short\\s95_gpu_profile.txt side by side.
Answers: "did this switch lower the whole frame, not only the passes gpu_profile.py times?" Use it beside gpu_profile.py A B (that one sees the passes only, over 6 logged frames).
    py -I tools\\native\\scene_gpu_ab.py LOGDIR A B"""
import csv, os, re, statistics, sys

d, a, b = sys.argv[1:4]


def marks(run):
    t = open(os.path.join(d, 'wwe2k14_%s.log' % run), encoding='utf-8', errors='replace').read()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r'scene=(\w+) \(frame (\d+)\)', t)}


def window(run, name, lo, hi):
    m = marks(run)[name]
    v = []
    for r in csv.DictReader(open(os.path.join(d, 'frames_%s.csv' % run), newline='')):
        try:
            f = int(r['frame'])
            if m + lo <= f < m + hi:
                v.append((float(r['gpu_ms']), float(r['frame_ms'])))
        except (ValueError, KeyError):
            pass
    return v


print('window            frames  gpu_ms median %s -> %s          mean %s -> %s        frame_ms median' % (a, b, a, b))
for label, name, lo, hi in (('brock', 'tron_343', 100, 1900), ('bryan early', 'tron_324', 300, 1500), ('bryan late', 'tron_324', 1500, 4000), ('match', 'match', 50, 600)):
    A, B = window(a, name, lo, hi), window(b, name, lo, hi)
    med = lambda v, i: statistics.median(x[i] for x in v)
    avg = lambda v, i: statistics.mean(x[i] for x in v)
    print('%-16s %4d/%4d  %6.3f -> %6.3f (%+5.1f%%)   %6.3f -> %6.3f (%+5.1f%%)   %5.2f -> %5.2f' % (label, len(A), len(B), med(A, 0), med(B, 0), 100 * (med(B, 0) / med(A, 0) - 1), avg(A, 0), avg(B, 0), 100 * (avg(B, 0) / avg(A, 0) - 1), med(A, 1), med(B, 1)))
