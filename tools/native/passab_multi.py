r"""Session 80: an in-run A/B/C... route measured by the per-pass GPU log, for the Surface (its sequential runs drift
with its heat: 2026-11-01, a lever that removes GPU work came out 47% slower as the 4th run of 4). As
passab_route.py, but any number of arms, each a set of cvar values (every arm sets every cvar named in any arm; a cvar
an arm does not name gets its first value in the list, the base). Per scene (Brock's entrance +300, Bryan's +440, the
match +900 of the s21_lb replay): one warm-up cycle of WARMUP frames per arm (not logged: the arms' pipelines are made
before the measured cycles), then CYCLES x (FRAMES pass-logged frames per arm, in rotation). Labels <scene>_<arm>; read
with py -I tools\native\pass_roles.py RUN (per label) or py -I tools\native\passab_multi.py --read RUN.

    py -I tools\native\passab_multi.py NAME "WHAT" ARM:CVAR=V[;CVAR=V...] [ARM:...] [--cycles 8 --frames 4 --warmup 60]
    py -I tools\native\passab_multi.py --read RUN [--dir LOGDIR]

Writes tools\input_scripts\short\s80_NAME_multi.txt. Run it with --wwe2k14_replay_play=s21_lb
--gpu_native_gpu_timers=true and the scene's flags (a NATIVE test settings file).
"""
import argparse
import os
import re
import statistics
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEAD = """set 0x0010 0 0 0 0 -1636 -1137
wait_frames 10
set 0x0000 0 0 0 0 -1069 -1234
wait_frames 66
set 0x1000 0 0 0 0 -770 -1136
wait_frames 10
set 0x0000 0 0 0 0 -770 -1136"""
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #\d+ kind \d+ \d+x\d+ .*: ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
MARK = re.compile(r'passlog: (\S+)')


def write(a):
    arms = []  # (name, {cvar: value})
    for spec in a.arms:
        name, _, body = spec.partition(':')
        values = {}
        for item in filter(None, body.split(';')):
            k, _, v = item.partition('=')
            values[k.strip()] = v.strip()
        arms.append((name, values))
    cvars = []
    for _, values in arms:
        cvars += [k for k in values if k not in cvars]
    base = {k: next(v[k] for _, v in arms if k in v) for k in cvars}  # first value named = the base value

    def settings(values):
        return [f'cvar {k} {values.get(k, base[k])}' for k in cvars]

    lines = ['# harness: fastforward',
             f'# Session 80, written by tools\\native\\passab_multi.py: {a.what}.']
    for name, values in arms:
        lines.append(f'#   arm {name}: ' + (', '.join(f'{k}={values.get(k, base[k])}' for k in cvars) or 'base'))
    lines += [f'# Per scene: warm-up {a.warmup} frames per arm, then {a.cycles} cycles x {a.frames} pass-logged frames '
              'per arm; labels <scene>_<arm>.',
              'on_fail quit', f'log bench: start s80_{a.name}_multi (launch)', 'wait_file SaveData.Dat 3600',
              'wait_frames 160', HEAD, 'wait_file MNUIHD.PAC 3600', 'wait_file INGAMELUA.PAC 1800',
              'log bench: match launched', 'wait_file BG04.PAC 900']
    gap = a.frames + 4
    for scene, marker, offset in (('brock', '343.bik2', 300), ('bryan', '324.bik2', 440), ('match', 'M3.PAC', 900)):
        lines += [f'wait_file {marker} 30000', f'log scene={scene}',
                  f'wait_frames {max(0, offset - a.warmup * len(arms))}']
        for name, values in arms:
            lines += settings(values) + [f'wait_frames {a.warmup}']
        for _ in range(a.cycles):
            for name, values in arms:
                lines += settings(values) + ['wait_frames 2', f'log passlog: {scene}_{name}',
                                             f'cvar gpu_native_truth_pass_log {a.frames}', f'wait_frames {gap}']
        lines += settings({})  # the base between scenes
    lines += ['wait_frames 60', 'log bench: done', 'quit']
    out = os.path.join(ROOT, 'tools', 'input_scripts', 'short', f's80_{a.name}_multi.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(out)


def read(a):
    # GPU ms per logged frame (own + gaps of its passes) per label; median and mean, then each arm against the base.
    label = 'start'
    frames = defaultdict(lambda: defaultdict(float))
    with open(os.path.join(a.dir, f'wwe2k14_{a.read}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARK.search(line)
            if m:
                label = m.group(1)
                continue
            m = PASS.search(line)
            if m:
                frames[label][int(m.group(1))] += float(m.group(2)) + float(m.group(3))
    by_scene = defaultdict(dict)
    for lab, fr in frames.items():
        if '_' not in lab:
            continue
        scene, arm = lab.split('_', 1)
        by_scene[scene][arm] = list(fr.values())
    for scene, arms in by_scene.items():
        base_name = next(iter(arms))
        base = statistics.median(arms[base_name])
        print(f'== {scene} (base = {base_name}: median {base:.2f} ms)')
        for arm, v in arms.items():
            med = statistics.median(v)
            print(f'   {arm:24s} n {len(v):3d}  median {med:7.2f}  mean {statistics.mean(v):7.2f}  '
                  f'vs base {100 * (med - base) / base:+6.1f}%')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name', nargs='?')
    ap.add_argument('what', nargs='?')
    ap.add_argument('arms', nargs='*')
    ap.add_argument('--cycles', type=int, default=8)
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--warmup', type=int, default=60)
    ap.add_argument('--read')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    a = ap.parse_args()
    if a.read:
        read(a)
    elif a.name and a.arms:
        write(a)
    else:
        ap.error('give NAME WHAT ARM... or --read RUN')


if __name__ == '__main__':
    main()
