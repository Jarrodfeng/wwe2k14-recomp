"""The segment log of one frame between its Nth and Mth 'truth pass' lines (1-based pass numbers as the copy graph
counts them: pass 0 = the first), shortened: pass / draw / restore / resolve / dead-pass / copy lines only.
Session 80 (frame structure).

    py -I tools\\native\\frame_window.py LOG FRAME FIRST_PASS LAST_PASS
"""
import re
import sys

log, frame, first, last = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
keep = re.compile(r'truth (pass|draw|restore|dead pass|resolve)|resolve dest|draw textures|deferred')
n = -1
for line in open(log, encoding='utf-8', errors='replace'):
    if f'[native][seg] frame {frame} ' not in line:
        continue
    body = line.split(f'[native][seg] frame {frame} ', 1)[1].rstrip()
    if body.startswith('truth pass:'):
        n += 1
    if first <= n <= last and keep.search(body) and not body.startswith('truth pass end'):
        print(f'[{n}] {body[:230]}')
