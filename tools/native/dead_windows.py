"""Per 60-frame window of a run log: passes per frame, dead passes turned off / kept (gpu_native_truth_drop_dead_passes),
grouped into runs of equal values. Session 80.

    py -I tools\\native\\dead_windows.py LOG
"""
import re
import sys

text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
rows = []
for m in re.finditer(r'\[native\]\[truth\] frames (\d+)-(\d+): truth frames.*?per frame: passes ([0-9.]+)', text):
    rows.append([int(m.group(1)), int(m.group(2)), float(m.group(3)), 0.0, 0.0, m.end()])
dead = [(m.start(), float(m.group(1)), float(m.group(2))) for m in
        re.finditer(r'dead passes turned off per frame: ([0-9.]+); cleared by the next pass but kept ([0-9.]+)', text)]
for pos, off, kept in dead:  # the census line follows its window's truth line
    prev = [r for r in rows if r[5] < pos]
    if prev:
        prev[-1][3], prev[-1][4] = off, kept
groups = []
for r in rows:
    key = (round(r[3]), round(r[4]))
    if groups and groups[-1][2] == key:
        groups[-1][1] = r[1]
        groups[-1][3].append(r[2])
    else:
        groups.append([r[0], r[1], key, [r[2]]])
for a, b, (off, kept), passes in groups:
    print(f'frames {a:5d}-{b:5d}: passes/frame {min(passes):5.1f}-{max(passes):5.1f}  dead off ~{off}  kept ~{kept}')
