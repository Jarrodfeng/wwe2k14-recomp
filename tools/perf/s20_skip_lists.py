"""Session 20 item 2: skip lists (vertex shader hashes) by category, for --gpu_debug_skip_vertex_shader.

    py tools\\perf\\s20_skip_lists.py [PAN_CSV MATCH_CSV]   -> tools\\perf\\s20_skip_lists.txt (NAME=hash,hash,...)

Built from Session 19's anatomy frames (run 1434: Bryan +440 = the ramp pan, match +600). Categories:
- crowd:  the triangle-strip shaders with hundreds of constants-only draws (pan: C1028A64 304, D5E2FCC9 1,396;
          match: 183CC64A 281). Checked by screenshot in item 2 (crowd gone, nothing else).
- points: B6C9863F, 960 single-point draws per frame in both frames, no colour mask, no depth: no visible output
          (Session 19 counted it with the crowd; purpose unconfirmed).
- chars:  tools\\matte\\character_vs.txt (the matte mask's 11 shaders, Session 16).
- post:   every shader drawing rectangle / quad lists (prim 8 / 13: screen-space passes; includes the HUD if any).
- arena:  everything else in either frame.
Shaders not in these two frames (other camera shots) are in no list.
"""
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LOGS = REPO / 'out' / 'build' / 'win-amd64-release' / 'logs'
pan_csv, match_csv = (sys.argv[1:3] if len(sys.argv) > 2 else
                      (LOGS / 'anatomy_1434_0.csv', LOGS / 'anatomy_1434_1.csv'))
CROWD = ['C1028A64EFCF8229', 'D5E2FCC9F76BC40C', '183CC64A4C4E96AC']
POINTS = ['B6C9863F710683EC']
chars = (REPO / 'tools' / 'matte' / 'character_vs.txt').read_text().strip().split(',')
prims = {}
for p in (pan_csv, match_csv):
    with open(p, newline='') as f:
        for r in csv.DictReader(f):
            if r['frame'] == '0' and r['kind'] == 'draw':
                prims.setdefault(r['vs_hash'], set()).add(r['prim'])
post = sorted(h for h, s in prims.items() if s & {'8', '13'} and h not in CROWD + POINTS + chars)
arena = sorted(h for h in prims if h not in CROWD + POINTS + chars + post)
lists = {'crowd': CROWD, 'points': POINTS, 'chars': chars, 'post': post, 'arena': arena}
out = Path(__file__).with_suffix('.txt')
out.write_text(''.join(f'{k}={",".join(v)}\n' for k, v in lists.items()))
for k, v in lists.items():
    print(f'{k}: {len(v)} shaders')
print(f'-> {out}')
