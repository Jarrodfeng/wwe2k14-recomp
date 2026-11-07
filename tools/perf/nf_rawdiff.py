"""Which words of a hashed region differ between two plays, in every raw dump (Session 95: a lean native check).
Prints offsets and values for the first dumps that differ, and how often each offset differs. Values are numbers from
the local dumps (out\\, never committed).

    py tools\\perf\\nf_rawdiff.py det_A.bin det_A.csv det_B.bin REGION [--show 6]
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, r'C:\Users\Jarrod\wwe2k14-recomp\tools\determinism')
import det_compare as dc  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('abin')
ap.add_argument('acsv')
ap.add_argument('bbin')
ap.add_argument('region')
ap.add_argument('--show', type=int, default=6)
a = ap.parse_args()
names = dc.read_csv_path(a.acsv)[0]
ra, rb = dc.read_bin_path(a.abin), dc.read_bin_path(a.bbin)
k = None
for i, n in enumerate(names):
    if n == a.region:
        k = i
sample = next(iter(ra.values()))
print('regions', len(names), 'raw slots', len(sample))
if k is None or k >= len(sample):
    # the raw slots may follow another order: look for one by position in a name list inside the dump, else all slots
    print('region index not usable, scanning all slots')
    ks = range(len(sample))
else:
    ks = [k]
count = collections.Counter()
shown = 0
for f in sorted(set(ra) & set(rb)):
    for s in ks:
        x, y = ra[f][s], rb[f][s]
        if x is None or y is None:
            continue
        x, y = bytes(x), bytes(y)
        d = [o for o in range(0, min(len(x), len(y)) & ~3, 4) if x[o:o + 4] != y[o:o + 4]]
        for o in d:
            count[(s, o)] += 1
        if d and shown < a.show:
            shown += 1
            print(f'frame {f} slot {s} ({len(x)} bytes):', ' '.join(f'+{o}: {x[o:o + 4].hex()} / {y[o:o + 4].hex()}' for o in d[:6]))
print('dumps compared', len(set(ra) & set(rb)), 'offsets differing:', count.most_common(12))
