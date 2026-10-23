"""The native renderer's comparison loop (Session 38): parity and cost per scene, from one run's log.

The native renderer runs beside the emulated one (gpu_native=1) and writes a [native] report line every
gpu_native_report_frames frames: the parity of its depth and colour against the emulated frame's (the same frame,
read from the EDRAM) and the GPU time of the emulated scene draws vs the native ones. This tool groups those lines by
the scene markers the route logs ("scene=NAME", e.g. short\\s21_replay_time.txt: tron_343, tron_324, match) and prints
one row per scene. Run it through tools\\native\\compare.ps1 (plays the s21_lb replay), or on any run:

    py tools\\native\\compare.py RUN [--out FILE]

Numbers only; nothing from the game.
"""
import argparse
import os
import re
import sys

LOGS = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
TIME = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')
SCENE = re.compile(r'\[input\] line \d+: scene=(\w+)')
REPORT = re.compile(
    r'\[native\] frames (\d+)-(\d+) \((\d+) read(?:, \d+ replaced)?\): scene draws (\d+)/frame, native (\d+)/frame; '
    r'GPU emulated scene ([\d.]+) ms/frame, native ([\d.]+) ms/frame \(frames with timestamps (\d+)\); CPU (\d+) '
    r'us/frame; .*?targets (\d+)x(\d+); shading (\w+), draws ([\w-]+); (.*)$')
# Session 39 (gpu_native 2): replaced frames, the passes / loads / write-backs, their GPU time, and why draws stayed
# emulated.
REPLACED = re.compile(r'\((\d+) read, (\d+) replaced\)')
PASSES = re.compile(r'passes ([\d.]+), loads ([\d.]+), write-backs ([\d.]+) per frame; replaced frames: GPU native '
                    r'([\d.]+) ms/frame \(frames with timestamps (\d+)\)')
LEFT_OUT = re.compile(r'left out per frame: (.*?); passes')
DEPTH = re.compile(r'parity \((\d+) frames\): depth agrees on ([\d.]+)% of the pixels both drew \(([\d.]+)% of the '
                   r'frame\); \|dz\| <1e-6 ([\d.]+)%')
COLOR = re.compile(r'colour \((\d+) frames, RGB 8-bit levels\): exact ([\d.]+)%, within 2 ([\d.]+)%, within 8 '
                   r'([\d.]+)%, within 32 ([\d.]+)%, more ([\d.]+)%; mean \|difference\| R ([\d.]+) G ([\d.]+) B '
                   r'([\d.]+)')


def parse(path):
    scene = 'before'
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SCENE.search(line)
        if m and '[input]' in line:
            scene = m.group(1)
            continue
        m = REPORT.search(line)
        if not m:
            continue
        r = {
            'scene': scene, 'frames': int(m.group(3)), 'scene_draws': int(m.group(4)), 'native_draws': int(m.group(5)),
            'emu_ms': float(m.group(6)), 'nat_ms': float(m.group(7)), 'timed': int(m.group(8)),
            'cpu_us': int(m.group(9)), 'size': f'{m.group(10)}x{m.group(11)}', 'shading': m.group(12),
            'draws': m.group(13),
        }
        p = REPLACED.search(line)
        r['replaced'] = int(p.group(2)) if p else 0
        p = PASSES.search(line)
        if p:
            r['passes'], r['loads'], r['writebacks'] = float(p.group(1)), float(p.group(2)), float(p.group(3))
            r['repl_ms'], r['repl_timed'] = float(p.group(4)), int(p.group(5))
        p = LEFT_OUT.search(line)
        if p:
            for item in p.group(1).split(', '):
                name, _, value = item.rpartition(' ')
                try:
                    r['left_' + name] = float(value)
                except ValueError:
                    pass
        d = DEPTH.search(m.group(14))
        if d:
            r['depth_frames'] = int(d.group(1))
            r['depth_agree'] = float(d.group(2))
            r['depth_cover'] = float(d.group(3))
        c = COLOR.search(m.group(14))
        if c:
            r['color_frames'] = int(c.group(1))
            r['color_exact'] = float(c.group(2))
            r['color_within2'] = float(c.group(2)) + float(c.group(3))
            r['color_within8'] = r['color_within2'] + float(c.group(4))
            r['color_mean'] = (float(c.group(7)) + float(c.group(8)) + float(c.group(9))) / 3
        rows.append(r)
    return rows


def weighted(rows, key, weight):
    total = sum(r[weight] for r in rows if key in r)
    if not total:
        return None
    return sum(r[key] * r[weight] for r in rows if key in r) / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--out')
    ap.add_argument('--hitch-us', type=int, default=5000,
                    help='leave out report windows whose CPU time per frame is above this (pipeline creation)')
    a = ap.parse_args()
    path = os.path.join(LOGS, f'wwe2k14_{a.run}.log')
    rows = [r for r in parse(path) if r['native_draws'] > 0]
    if not rows:
        print(f'run {a.run}: no [native] report lines with native draws')
        return 1
    out = []
    out.append(f'run {a.run}: native renderer parity and cost per scene ({len(rows)} report windows; windows with '
               f'more than {a.hitch_us} us of CPU per frame left out of the cost)')
    out.append(f'{"scene":<12} {"frames":>6} {"draws":>11} {"depth ok %":>10} {"cover %":>8} {"colour exact %":>14} '
               f'{"<=2 %":>7} {"<=8 %":>7} {"mean":>6} {"GPU emu ms":>10} {"GPU nat ms":>10} {"CPU us":>7}')
    scenes = []
    for r in rows:
        if r['scene'] not in scenes:
            scenes.append(r['scene'])
    for s in scenes + ['all']:
        rs = [r for r in rows if s == 'all' or r['scene'] == s]
        steady = [r for r in rs if r['cpu_us'] <= a.hitch_us and r['timed']]
        frames = sum(r['frames'] for r in rs)

        def f(v, fmt):
            return format(v, fmt) if v is not None else '-'
        draws = f'{weighted(rs, "native_draws", "frames"):.0f}/{weighted(rs, "scene_draws", "frames"):.0f}'
        out.append(
            f'{s:<12} {frames:>6} {draws:>11} {f(weighted(rs, "depth_agree", "depth_frames"), ".3f"):>10} '
            f'{f(weighted(rs, "depth_cover", "depth_frames"), ".1f"):>8} '
            f'{f(weighted(rs, "color_exact", "color_frames"), ".2f"):>14} '
            f'{f(weighted(rs, "color_within2", "color_frames"), ".3f"):>7} '
            f'{f(weighted(rs, "color_within8", "color_frames"), ".3f"):>7} '
            f'{f(weighted(rs, "color_mean", "color_frames"), ".3f"):>6} '
            f'{f(weighted(steady, "emu_ms", "timed"), ".3f"):>10} {f(weighted(steady, "nat_ms", "timed"), ".3f"):>10} '
            f'{f(weighted(steady, "cpu_us", "frames"), ".0f"):>7}')
    if any(r.get('replaced') for r in rows):
        # Replace mode (gpu_native 2): how much of each scene was replaced, what stayed emulated and why, and the GPU
        # time of the replaced frames' native work (passes + loads + write-backs).
        out.append('')
        out.append(f'{"scene":<12} {"replaced %":>10} {"passes":>6} {"loads":>6} {"w-backs":>7} {"GPU nat ms":>10}  '
                   f'left emulated per frame (by reason)')
        for s in scenes + ['all']:
            rs = [r for r in rows if s == 'all' or r['scene'] == s]
            frames = sum(r['frames'] for r in rs)
            replaced = sum(r.get('replaced', 0) for r in rs)
            steady = [r for r in rs if r['cpu_us'] <= a.hitch_us and r.get('repl_timed')]
            reasons = {}
            for r in rs:
                for k, v in r.items():
                    if k.startswith('left_'):
                        reasons[k[5:]] = reasons.get(k[5:], 0.0) + v * r['frames']
            left = ', '.join(f'{k} {v / frames:.1f}' for k, v in reasons.items() if v / frames >= 0.05) or 'none'

            def g(key):
                v = weighted(rs, key, 'frames')
                return format(v, '.1f') if v is not None else '-'
            nat = weighted(steady, 'repl_ms', 'repl_timed')
            out.append(f'{s:<12} {100.0 * replaced / max(1, frames):>10.1f} {g("passes"):>6} {g("loads"):>6} '
                       f'{g("writebacks"):>7} {format(nat, ".3f") if nat is not None else "-":>10}  {left}')
    shading = sorted({r['shading'] for r in rows})
    draws = sorted({r['draws'] for r in rows})
    sizes = sorted({r['size'] for r in rows})
    out.append(f'shading {",".join(shading)}; draws {",".join(draws)}; targets {",".join(sizes)}. "draws" = native / '
               f'emulated draws into the scene target per frame; "cover" = share of the frame where both drew; '
               f'colour in 8-bit levels (largest RGB channel difference); GPU ms = the emulated scene segments vs '
               f'our passes over the same draws.')
    text = '\n'.join(out)
    print(text)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, 'w', encoding='utf-8') as fh:
            fh.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
