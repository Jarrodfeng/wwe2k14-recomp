r"""The per-pass GPU profile (Session 95, tool Q): one table per scene of what the GPU's frame is spent on. Reads a run of the route
short\s95_gpu_profile.txt (PC: run_list -Exclusive with --vsync=false; Surface: bench.ps1 -Only gpuprof; phone: the same route).

    py -I tools\native\gpu_profile.py RUN [--dir LOGS] [--top 14] [--pyro-only]     tables per scene (brock, pyro, bryan, match)
    py -I tools\native\gpu_profile.py RUN --by-frame pyro                           one line per frame of a scene: its GPU totals
    py -I tools\native\gpu_profile.py A B [--dir LOGS] [--dir2 LOGS]                two runs side by side per scene (totals only)

Per scene: GPU first-to-last, time inside timed segments, time between them, the part of it the GPU sat idle waiting for a submission
("busy" = first-to-last minus that idle), submissions a frame, and the top pieces (a piece = its kind / owned label, size, opening
pixel shader) by GPU ms a frame. RUN = number or a log path. Scene frames come from the route's "passlog: NAME" marks.
"""
import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import owned_pieces as op  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOGS = Path(os.environ.get('LOGS') or ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs')
SUBS = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) submissions; the GPU waited for one in (\d+) gaps, ([0-9.]+) ms')


def log_path(run, d):
    p = Path(run)
    if p.suffix == '.log' and p.exists():
        return p
    q = Path(d) / f'wwe2k14_{int(run):03d}.log'
    if not q.exists():
        sys.exit(f'gpu_profile: no log {q} (give --dir)')
    return q


def read(path):
    """scenes: {name: {frame: {'pieces': [...], 'total': (seg, gap, span), 'idle': ms, 'subs': n}}} in route order."""
    scenes, label = {}, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = op.MARK.search(line)
            if m and '[input]' in line:
                label = m.group(1)
                scenes.setdefault(label, defaultdict(lambda: {'pieces': [], 'total': None, 'idle': 0.0, 'subs': 0}))
                continue
            if label is None or '[native][pass]' not in line:
                continue
            m = op.PASS.search(line)
            if m:
                kind, owned = int(m.group(3)), m.group(14)
                name = f'owned {owned}' if owned else op.KINDS.get(kind, 'truth')
                scenes[label][int(m.group(1))]['pieces'].append(
                    (name, f'{m.group(4)}x{m.group(5)}', m.group(11)[:8], int(m.group(9)), float(m.group(12)), float(m.group(13))))
                continue
            m = op.TOTAL.search(line)
            if m:
                scenes[label][int(m.group(1))]['total'] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
                continue
            m = SUBS.search(line)
            if m:
                fr = scenes[label][int(m.group(1))]
                fr['subs'], fr['idle'] = int(m.group(2)), float(m.group(4))
    if not any(scenes.values()):
        sys.exit(f'gpu_profile: no [native][pass] lines under a passlog mark in {path}: wrong route, or the timers are off '
                 '(--vulkan_gpu_timestamps=true --gpu_native_gpu_timers=true)')
    return scenes


def totals(frames):
    fs = [v for v in frames.values() if v['total']]
    n = max(1, len(fs))
    seg = sum(v['total'][0] for v in fs) / n
    gap = sum(v['total'][1] for v in fs) / n
    span = sum(v['total'][2] for v in fs) / n
    idle = sum(v['idle'] for v in fs) / n
    subs = sum(v['subs'] for v in fs) / n
    return len(fs), seg, gap, span, idle, subs


def table(name, frames, top):
    n, seg, gap, span, idle, subs = totals(frames)
    print(f'== {name}: {n} frames; first to last {span:.3f} ms = segments {seg:.3f} + between {gap:.3f} (GPU idle {idle:.3f}); '
          f'busy {span - idle:.3f} ms; {subs:.1f} submissions')
    agg = defaultdict(lambda: [0.0, 0.0, 0, 0])
    nf = max(1, len(frames))
    for v in frames.values():
        for label, box, ps, draws, ms, before in v['pieces']:
            k = (label.split()[0] + ' ' + label.split()[1] if label.startswith('owned ') else label, box, ps)
            x = agg[k]; x[0] += ms; x[1] += before; x[2] += draws; x[3] += 1
    for k, (ms, before, draws, c) in sorted(agg.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:top]:
        print(f'  {ms / nf:7.3f} ms  before {before / nf:7.3f}  draws {draws / nf:7.1f}  x{c / nf:4.1f}  {k[0]:22s} {k[1]:10s} ps {k[2]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--dir', default=str(DEFAULT_LOGS))
    ap.add_argument('--dir2', default=None)
    ap.add_argument('--top', type=int, default=14)
    ap.add_argument('--by-frame', default=None, help='scene name: one line per frame')
    ap.add_argument('--scene', default=None, help='only this scene')
    a = ap.parse_args()
    if len(a.runs) == 2:
        s1 = read(log_path(a.runs[0], a.dir)); s2 = read(log_path(a.runs[1], a.dir2 or a.dir))
        for name in s1:
            if name not in s2:
                continue
            for tag, s in ((a.runs[0], s1), (a.runs[1], s2)):
                n, seg, gap, span, idle, subs = totals(s[name])
                print(f'{name:8s} {tag:>8s}: span {span:8.3f} seg {seg:8.3f} gap {gap:8.3f} idle {idle:8.3f} busy {span - idle:8.3f} subs {subs:5.1f} ({n} fr)')
        return
    scenes = read(log_path(a.runs[0], a.dir))
    if a.by_frame:
        for fr, v in sorted(scenes.get(a.by_frame, {}).items()):
            t = v['total'] or (0, 0, 0)
            top = max(v['pieces'], key=lambda p: p[4] + p[5], default=None)
            d = sum(p[3] for p in v['pieces'])
            print(f'{fr}: span {t[2]:7.3f} seg {t[0]:7.3f} gap {t[1]:7.3f} idle {v["idle"]:7.3f} subs {v["subs"]:3d} pieces {len(v["pieces"]):3d} draws {d:5d} '
                  f'top {top[0] if top else "-"} {top[1] if top else ""} {top[4] if top else 0:.3f} (+{top[5] if top else 0:.3f})')
        return
    for name, frames in scenes.items():
        if a.scene and name != a.scene:
            continue
        table(name, frames, a.top)


if __name__ == '__main__':
    main()
