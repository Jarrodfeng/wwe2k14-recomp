r"""Session 72 part 4: what fills the GPU time between NATIVE truth passes. Joins one frame's pass log
([native][pass], GPU times in recording order) with its segment log ([native][seg], what was recorded: passes,
resolves, texture fills, stores) - route tools\input_scripts\short\s72_gap_log.txt arms both for the same frame.
The n-th "truth pass:" line of the segment log is the n-th kind-4 segment of the pass log; the non-pass events
between two passes are printed above the pass whose "before it" gap they fall into, then a summary of the gaps by
what preceded them.

    py -I tools\native\gap_log.py RUN [--dir LOGDIR] [--min MS]
"""
import argparse
import os
import re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) depth (\d) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
TOTAL = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) timed segments, ([0-9.]+) ms in them, ([0-9.]+) ms between')
SEG = re.compile(r'\[native\]\[seg\] (?:frame (\d+) )?(.*)')


def event_class(text):
    if text.startswith('resolve copy:'):
        m = re.search(r'shader (\d+) dest page \S+ pitch \d+ (\d+)x(\d+) dest format (\d+)', text)
        return f'resolve sh{m.group(1)} {m.group(2)}x{m.group(3)} f{m.group(4)}' if m else 'resolve copy'
    if text.startswith('truth texture fill:'):
        m = re.search(r'format (\d+) level (\d+) (\d+)x(\d+) .*pack mode (\d+)', text)
        return f'fill f{m.group(1)} {m.group(3)}x{m.group(4)} pack{m.group(5)}' if m else 'fill'
    if text.startswith('truth texture fill refused'):
        return 'fill refused'
    if text.startswith('truth texture-only resolve'):
        return 'texture-only resolve'
    return text.split(':')[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--min', type=float, default=0.0, help='print only passes with a gap of at least MS')
    a = ap.parse_args()
    path = os.path.join(a.dir, f'wwe2k14_{a.run}.log')
    passes = defaultdict(list)   # frame -> [(kind, w, h, draws, ps, own, before)]
    totals = {}
    seg = defaultdict(list)      # frame -> [event text]
    last_frame = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = PASS.search(line)
            if m:
                passes[int(m.group(1))].append((int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(9)),
                                                m.group(11)[:8], float(m.group(12)), float(m.group(13))))
                continue
            m = TOTAL.search(line)
            if m:
                totals[int(m.group(1))] = (float(m.group(3)), float(m.group(4)))
                continue
            m = SEG.search(line)
            if m:
                if m.group(1):
                    last_frame = int(m.group(1))
                text = m.group(2)
                if last_frame is not None and not text.startswith('truth draw:'):
                    seg[last_frame].append(text)
    for frame in sorted(passes):
        events = seg.get(frame) or seg.get(frame + 1) or seg.get(frame - 1) or []
        # Split the segment log at its "truth pass:" lines: groups[i] = events before pass i.
        groups, cur = [], []
        for e in events:
            if e.startswith('truth pass:'):
                groups.append(cur)
                cur = []
            else:
                cur.append(e)
        tail = cur
        own, between = totals.get(frame, (0.0, 0.0))
        print(f'== frame {frame}: {len(passes[frame])} segments, {own:.3f} ms in them, {between:.3f} ms between; '
              f'segment log: {len(groups)} passes')
        by_cause = defaultdict(lambda: [0, 0.0])
        pi = 0
        for k, (kind, w, h, draws, ps, t, before) in enumerate(passes[frame]):
            if kind == 4:
                pre = groups[pi] if pi < len(groups) else ['(segment log ran out)']
                pi += 1
            else:
                pre = []
            classes = [event_class(e) for e in pre if not e.startswith(('truth pass end', 'lazy pass barrier'))]
            cause = ' + '.join(sorted(set(c.split(' ')[0] for c in classes))) or ('store' if kind == 5 else '-')
            by_cause[cause][0] += 1
            by_cause[cause][1] += before
            if before >= a.min:
                label = {4: 'pass', 5: 'store', 6: 'staged resolve', 7: 'fill'}.get(kind, f'kind {kind}')
                print(f'  #{k:<3d} gap {before:.3f}  {label} {w}x{h} draws {draws} ps {ps}: {t:.3f} ms')
                for c in classes:
                    print(f'         before: {c}')
        print('  gaps by what was recorded before them:')
        for cause, (n, ms) in sorted(by_cause.items(), key=lambda kv: -kv[1][1]):
            print(f'    {ms:7.3f} ms  {n:3d}x  {cause}')
        if tail:
            print(f'  after the last pass: {", ".join(sorted(set(event_class(e) for e in tail)))}')


if __name__ == '__main__':
    main()
