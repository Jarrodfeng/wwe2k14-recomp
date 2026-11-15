#!/usr/bin/env python3
"""S102 M7: blank the `scene` column (free text from the game's own sequence names) of the fixture frames csvs under tools\\ab_flip\\fixtures: they then hold only our counters.
  py -I tools\\ab_flip\\scrub_fixture.py [DIR]"""
import csv
import glob
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fixtures')
for p in glob.glob(os.path.join(root, '**', 'frames_*.csv'), recursive=True):
    with open(p, newline='') as f:
        rows = list(csv.reader(f))
    i = rows[0].index('scene')
    for r in rows[1:]:
        if len(r) > i:
            r[i] = ''
    with open(p, 'w', newline='') as f:
        csv.writer(f, lineterminator='\n').writerows(rows)
    print('scrubbed', p)
