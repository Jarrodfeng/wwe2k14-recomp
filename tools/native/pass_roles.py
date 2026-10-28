r"""Session 72 part 4: one pass-logged frame's GPU time by the role of each pass (by guest pixel shader hash and
size, docs\systems\post_effects.md and the s72 gap logs): own time + the gap before it, summed per role, for the
frames under each "passlog: LABEL" mark (as pass_log.py).

    py -I tools\native\pass_roles.py RUN [--dir LOGDIR] [--label LABEL]
"""
import argparse
import os
import re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) depth (\d) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
MARK = re.compile(r'passlog: (\S+)')

POST = {'9C85DA5F': 'post downsample', '156FA9A1': 'post bright pass', '901118D1': 'post downsample',
        '632189BA': 'post blur', '755AC86C': 'post blur', 'A7AD68BE': 'post downsample', 'A5C9AC54': 'post blur',
        '75641266': 'post glare', '9C84E33D': 'post glare', '4E2CD803': 'post combine', '1233F68C': 'post combine',
        '0DC910FA': 'post other'}
COPY = {'B5853824', '9A836B68', '7ABDFEFB'}


def role(kind, w, h, draws, ps):
    if kind == 5:
        return 'store (alias tiles)'
    if kind in (6, 7):
        return 'resolve / fill timer'
    if ps in POST:
        return POST[ps]
    if ps == 'F235532B':
        return 'mip chain copies'
    if ps == '8479D8DA':
        return 'reflection cube faces'
    if ps in COPY:
        return 'copies (scene restore etc.)'
    if draws >= 100:
        return 'scene'
    if w <= 160 and h <= 128 and draws <= 2:
        return 'tiny maps'
    if ps == '00000000':
        return 'depth only'
    return 'other'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--label')
    a = ap.parse_args()
    label = 'start'
    frames = defaultdict(lambda: defaultdict(list))  # label -> frame -> passes
    with open(os.path.join(a.dir, f'wwe2k14_{a.run}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARK.search(line)
            if m:
                label = m.group(1)
                continue
            m = PASS.search(line)
            if m:
                frames[label][int(m.group(1))].append(
                    (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(9)), m.group(11)[:8],
                     float(m.group(12)), float(m.group(13))))
    for lab, fr in frames.items():
        if a.label and lab != a.label:
            continue
        acc = defaultdict(lambda: [0, 0.0, 0.0])
        for passes in fr.values():
            for kind, w, h, draws, ps, own, before in passes:
                r = acc[role(kind, w, h, draws, ps)]
                r[0] += 1
                r[1] += own
                r[2] += before
        n = len(fr)
        tot = sum(v[1] + v[2] for v in acc.values()) / n
        print(f'== {lab}: {n} frame(s), {tot:.3f} ms per frame (own + gaps)')
        print(f'   {"role":30s} {"passes":>6s} {"own ms":>8s} {"gaps ms":>8s} {"total":>8s}  share')
        for name, (c, own, gap) in sorted(acc.items(), key=lambda kv: -(kv[1][1] + kv[1][2])):
            print(f'   {name:30s} {c / n:6.1f} {own / n:8.3f} {gap / n:8.3f} {(own + gap) / n:8.3f}  '
                  f'{100 * (own + gap) / n / tot:4.1f}%')


if __name__ == '__main__':
    main()
