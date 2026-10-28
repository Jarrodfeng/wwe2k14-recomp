r"""Per-pass GPU times of NATIVE truth frames (Session 72): the [native][pass] lines that gpu_native_truth_pass_log
writes, one table per logged frame (in the order recorded) and a summary by class, split at the route's
"passlog: LABEL" marks.

    py -I tools\native\pass_log.py RUN [--dir LOGDIR] [--frames]

Classes (by the pass's own numbers, not by shader): "quad" = at most 2 draws (the post chain's full-surface quads and
the screen copies), split by size into full (>= 1280 wide), half / quarter / small; "scene" = the frame's passes of
100+ draws; "other" = the rest (shadow and reflection maps, sprites, the HUD).
"""
import argparse
import os
import re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) depth (\d) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
TOTAL = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) timed segments, ([0-9.]+) ms in them, ([0-9.]+) ms between '
                   r'them, ([0-9.]+) ms first to last')
MARK = re.compile(r'passlog: (\S+)')


def klass(w, h, draws):
    if draws <= 2:
        if w >= 1280:
            return 'quad full'
        if w >= 640:
            return 'quad half'
        if w >= 320:
            return 'quad quarter'
        return 'quad small'
    if draws >= 100:
        return 'scene'
    return 'other'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--frames', action='store_true', help='print every pass of every logged frame')
    a = ap.parse_args()
    label = 'start'
    frames = defaultdict(list)   # (label, frame) -> passes
    totals = {}
    order = []
    with open(os.path.join(a.dir, f'wwe2k14_{a.run}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARK.search(line)
            if m and '[input]' in line:
                label = m.group(1)
                continue
            m = PASS.search(line)
            if m:
                key = (label, int(m.group(1)))
                if key not in frames:
                    order.append(key)
                frames[key].append(dict(i=int(m.group(2)), kind=int(m.group(3)), w=int(m.group(4)), h=int(m.group(5)),
                                        pitch=int(m.group(6)), ck=int(m.group(7)), depth=int(m.group(8)),
                                        draws=int(m.group(9)), vs=m.group(10), ps=m.group(11),
                                        ms=float(m.group(12)), before=float(m.group(13))))
                continue
            m = TOTAL.search(line)
            if m:
                totals[(label, int(m.group(1)))] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
    if not order:
        print('no [native][pass] lines')
        return
    by_label = defaultdict(list)
    for key in order:
        by_label[key[0]].append(key)
    for lab, keys in by_label.items():
        print(f'== {lab}: {len(keys)} frame(s)')
        cls_ms = defaultdict(float)
        cls_n = defaultdict(int)
        before_ms = 0.0
        span = 0.0
        for key in keys:
            ps = frames[key]
            for p in ps:
                c = klass(p['w'], p['h'], p['draws'])
                cls_ms[c] += p['ms']
                cls_n[c] += 1
                before_ms += p['before']
            if key in totals:
                span += totals[key][2]
            if a.frames:
                print(f'   frame {key[1]}:')
                for p in ps:
                    print(f"     #{p['i']:3d} {p['w']:5d}x{p['h']:<4d} pitch {p['pitch']:3d} ck {p['ck']} d {p['depth']} "
                          f"draws {p['draws']:5d} {klass(p['w'], p['h'], p['draws']):12s} {p['ms']:7.3f} ms "
                          f"(before {p['before']:.3f}) ps {p['ps']}")
        n = len(keys)
        total_pass = sum(cls_ms.values())
        print(f'   per frame: passes {sum(cls_n.values()) / n:.1f}, in passes {total_pass / n:.3f} ms, between passes '
              f'{before_ms / n:.3f} ms, first to last {span / n:.3f} ms')
        for c in ('scene', 'other', 'quad full', 'quad half', 'quad quarter', 'quad small'):
            if cls_n[c]:
                print(f'     {c:12s} {cls_n[c] / n:5.1f} passes  {cls_ms[c] / n:7.3f} ms  '
                      f'({100.0 * cls_ms[c] / max(1e-9, total_pass):4.1f}% of pass time)')


if __name__ == '__main__':
    main()
