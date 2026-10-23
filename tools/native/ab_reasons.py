"""Session 43: an in-run A/B route's [native] report lines grouped by the mode ("ab: mode M") in force over each
report's frames: per scene and mode, the mean of every "left out per frame" reason, segments and loads. Use with
--gpu_native_report_frames=30 (a report inside one 120-frame window) and --wwe2k14_file_trace=true (scenes).

    py tools\\native\\ab_reasons.py RUN
"""
import os
import re
import sys
from collections import defaultdict

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
MODE = re.compile(r'\[input\] line \d+: ab: mode (\d) \(frame (\d+)\)')
OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.* frame=(\d+)")
REPORT = re.compile(r'\[native\] frames (\d+)-(\d+) .*left out per frame: (.*?); segments native ([\d.]+), emulated '
                    r'([\d.]+); passes ([\d.]+), loads ([\d.]+), write-backs ([\d.]+)')
SCENES = (('343.bik2', 'Brock'), ('324.bik2', 'Bryan'), ('m3.pac', 'match'))


def main():
    run = int(sys.argv[1])
    switches, scenes, reports, seen = [], [], [], set()
    for line in open(os.path.join(LOGS, f'wwe2k14_{run:03d}.log'), encoding='utf-8', errors='replace'):
        m = MODE.search(line)
        if m:
            switches.append((int(m.group(2)), m.group(1)))
            continue
        m = OPEN.search(line)
        if m:
            for key, name in SCENES:
                if m.group(1).lower().endswith(key) and name not in seen:
                    seen.add(name)
                    scenes.append((int(m.group(2)), name))
            continue
        m = REPORT.search(line)
        if m:
            reports.append(m)

    def at(table, frame):
        cur = None
        for f0, v in table:
            if f0 <= frame:
                cur = v
        return cur

    sums = defaultdict(lambda: defaultdict(float))
    counts = defaultdict(int)
    for m in reports:
        f0, f1 = int(m.group(1)), int(m.group(2))
        # The native renderer counts frames itself (its frame numbers run a little behind the guest's).
        a, b = at(switches, f0), at(switches, f1)
        scene = at(scenes, f1)
        if a is None or a != b or scene is None:
            continue
        key = (scene, a)
        counts[key] += 1
        for part in m.group(3).split(', '):
            name, value = part.rsplit(' ', 1)
            sums[key][name] += float(value)
        for i, name in ((4, 'segments native'), (5, 'segments emulated'), (6, 'passes'), (7, 'loads'),
                        (8, 'write-backs')):
            sums[key][name] += float(m.group(i))
    for key in sorted(counts):
        n = counts[key]
        vals = ', '.join(f'{k} {v / n:.1f}' for k, v in sums[key].items() if v)
        print(f'{key[0]:6s} mode {key[1]} ({n} reports): {vals}')


if __name__ == '__main__':
    main()
