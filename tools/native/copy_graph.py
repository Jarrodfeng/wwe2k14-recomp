"""Session 80 (frame structure): one frame's copy graph from the segment log (gpu_native_segment_log with the census
lines: "resolve dest: ..." and "draw textures: ..."). For every copy (a resolve into a texture page): when it ran (the
pass it followed), which draws sampled that page before the next copy into it or the frame's end (by pass and pixel
shader), the restores that read it, and the copies nothing sampled. Pages copied twice with no truth draw between are
flagged as back-to-back copies of the same source.

    py -I tools\\native\\copy_graph.py RUN FRAME [--min-size 65536]
"""
import argparse
import os
import re
from collections import defaultdict

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
PASS = re.compile(r'truth pass: colour (-?\d+) .*?depth (-?\d+) .*?rect (\S+) vs (\w+) clear (\w+) reads c(\w+) d(\w+)')
DRAW = re.compile(r'truth draw: vs (\w+) ps (\w+)')
TEX = re.compile(r'fc(\d+) 0x([0-9A-F]+) f(\d+) (\d+)x(\d+)')
DEST = re.compile(r'resolve dest: page 0x([0-9A-F]+) texture page 0x([0-9A-F]+) format (\d+) (\d+)x(\d+) at (\d+),(\d+) '
                  r'depth (\w+)')
RESTORE = re.compile(r'truth restore (?:\w+ )?into colour (-?\d+) depth (-?\d+) \(textures 0x([0-9A-F]+) / 0x([0-9A-F]+)\)'
                     r'.*: (.*)$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('frame')
    ap.add_argument('--min-size', type=int, default=65536, help='copies of fewer pixels are left out')
    a = ap.parse_args()
    prefix = f'[native][seg] frame {a.frame} '
    lines = []
    with open(os.path.join(LOGS, f'wwe2k14_{int(a.run):03d}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            i = line.find(prefix)
            if i >= 0:
                lines.append(line[i + len(prefix):].rstrip())
    copies = []          # dict(page, fmt, w, h, depth, pass_no, draws_before, readers, restores)
    current = {}         # page -> index into copies
    pass_no, passes = -1, []
    pending_tex = []
    draws = 0
    for text in lines:
        m = PASS.match(text)
        if m:
            pass_no += 1
            passes.append(dict(colour=int(m[1]), depth=int(m[2]), rect=m[3], vs=m[4][:8], clear=m[5], reads=m[6] + m[7]))
            continue
        m = DEST.match(text)
        if m:
            page = int(m[2], 16) or int(m[1], 16)
            prev = current.get(page)
            c = dict(page=page, fmt=int(m[3]), w=int(m[4]), h=int(m[5]), depth=m[8] == 'true', pass_no=pass_no,
                     draws=draws, readers=defaultdict(int), restores=[], after_draws=draws - (copies[prev]['draws']
                                                                                               if prev is not None else -1))
            current[page] = len(copies)
            copies.append(c)
            continue
        if text.startswith('draw textures:'):
            pending_tex = [(int(t[1], 16), int(t[2]), int(t[3]), int(t[4])) for t in TEX.findall(text)]
            continue
        m = DRAW.match(text)
        if m:
            draws += 1
            for page, fmt, w, h in pending_tex:
                k = current.get(page)
                if k is not None:
                    copies[k]['readers'][(pass_no, passes[pass_no]['vs'] if pass_no >= 0 else '-', m[2][:8])] += 1
            pending_tex = []
            continue
        m = RESTORE.match(text)
        if m:
            for page in (int(m[3], 16), int(m[4], 16)):
                k = current.get(page)
                if k is not None:
                    copies[k]['restores'].append((pass_no, m[5][:40]))
    print(f'frame {a.frame}: {len(passes)} passes, {draws} draws, {len(copies)} copies '
          f'({sum(1 for c in copies if c["w"] * c["h"] >= a.min_size)} of at least {a.min_size} pixels)')
    # Back-to-back copies: consecutive copies with no draw between them (the same moment of the source).
    for i, c in enumerate(copies):
        if c['w'] * c['h'] < a.min_size:
            continue
        same_moment = [d for d in copies[:i] if d['draws'] == c['draws'] and d['depth'] == c['depth'] and
                       d['w'] == c['w'] and d['h'] == c['h'] and d['page'] != c['page']]
        tag = f'  SAME MOMENT as 0x{same_moment[-1]["page"]:05X}' if same_moment else ''
        kind = 'depth' if c['depth'] else 'colour'
        print(f'copy #{i} after pass {c["pass_no"]} (draw {c["draws"]}): 0x{c["page"]:05X} {kind} f{c["fmt"]} '
              f'{c["w"]}x{c["h"]}{tag}')
        if not c['readers'] and not c['restores']:
            print('    NOT SAMPLED before the next copy into it / the frame end')
        for (p, vs, ps), n in sorted(c['readers'].items()):
            target = passes[p] if p >= 0 else {}
            print(f'    read by pass {p} (vs {vs}, colour {target.get("colour")}, {target.get("rect")}) ps {ps}: {n} draws')
        for p, what in c['restores']:
            print(f'    restore in pass {p}: {what}')


if __name__ == '__main__':
    main()
