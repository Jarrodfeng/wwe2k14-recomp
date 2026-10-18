"""Session 21: the barrier map. Reads a run's [s21][BARMAP] windows (SDK gpu_barrier_map=N) and symbolizes each
call site's 3 return addresses against the staged rexgpu-xenos.dll / .pdb with llvm-symbolizer.

    py tools/perf/barmap.py RUN [--exe-dir DIR] [--window K | --frames A-B] [--top 30]

Without --window / --frames: the windows are averaged (sites matched by their stack). Numbers only; our own logs.
"""
import argparse
import collections
import os
import re
import subprocess

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'

HEAD = re.compile(r'\[s21\]\[BARMAP\] frames (\d+)-(\d+): ([\d.]+) SubmitBarriers batches per frame \(([\d.]+) ending '
                  r'a render pass\), (\d+) call sites; module base ([0-9A-F]+)')
SITE = re.compile(r'\[s21\]\[BARMAP\] site \d+: ([\d.]+)/frame, cmds ([\d.]+), buffers shared ([\d.]+) edram ([\d.]+) '
                  r'other ([\d.]+), images ([\d.]+), render pass ends ([\d.]+), src ([0-9A-F]+) dst ([0-9A-F]+), '
                  r'stack ([0-9A-F]+) ([0-9A-F]+) ([0-9A-F]+)')
FIELDS = ['per_frame', 'cmds', 'shared', 'edram', 'other', 'images', 'rp_ends']

STAGES = [(0x1, 'top'), (0x2, 'drawind'), (0x4, 'vin'), (0x8, 'vs'), (0x10, 'tcs'), (0x20, 'tes'), (0x40, 'gs'),
          (0x80, 'fs'), (0x100, 'early_z'), (0x200, 'late_z'), (0x400, 'color_out'), (0x800, 'cs'),
          (0x1000, 'transfer'), (0x2000, 'bottom'), (0x4000, 'host'), (0x8000, 'all_gfx'), (0x10000, 'all')]


def stages(mask):
    m = int(mask, 16)
    return '+'.join(n for b, n in STAGES if m & b) or '0'


def load(log):
    windows = []
    for line in open(log, encoding='utf-8', errors='replace'):
        m = HEAD.search(line)
        if m:
            windows.append(dict(first=int(m[1]), last=int(m[2]), batches=float(m[3]), ends=float(m[4]),
                                base=int(m[6], 16), sites=[]))
            continue
        m = SITE.search(line)
        if m and windows:
            windows[-1]['sites'].append(dict(zip(FIELDS, map(float, m.groups()[:7])), src=m[8], dst=m[9],
                                             stack=tuple(int(x, 16) for x in m.groups()[9:12])))
    return windows


def symbolize(addrs, base, dll):
    """{addr: 'function file:line'} for addresses inside the module; llvm-symbolizer takes the PE's virtual address
    (preferred image base + RVA)."""
    out = {}
    inside = [a for a in addrs if base <= a < base + (64 << 20)]
    if not inside or not os.path.exists(SYMBOLIZER):
        return out
    pe_base = 0x180000000  # clang / MSVC default DLL image base; checked against the symbolizer result below
    for try_base in (pe_base, 0):
        q = '\n'.join('0x%X' % (a - base + try_base) for a in inside)
        r = subprocess.run([SYMBOLIZER, '--obj=' + dll, '--functions=short', '--relative-address'] if try_base == 0
                           else [SYMBOLIZER, '--obj=' + dll, '--functions=short'],
                           input=q, capture_output=True, text=True)
        blocks = [b.strip().split('\n') for b in r.stdout.strip().split('\n\n')]
        if len(blocks) == len(inside) and any(b[0] != '??' for b in blocks):
            for a, b in zip(inside, blocks):
                fn = b[0]
                loc = os.path.basename(b[1]) if len(b) > 1 else ''
                out[a] = '%s %s' % (fn, loc)
            return out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--exe-dir', default=EXE_DIR)
    ap.add_argument('--window', type=int)
    ap.add_argument('--frames')
    ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    log = os.path.join(a.exe_dir, 'logs', 'wwe2k14_%s.log' % str(a.run).zfill(3))
    windows = load(log)
    if not windows:
        raise SystemExit('no [s21][BARMAP] windows in ' + log)
    if a.window is not None:
        windows = [windows[a.window]]
    elif a.frames:
        lo, hi = map(int, a.frames.split('-'))
        windows = [w for w in windows if w['first'] >= lo and w['last'] <= hi]
    agg = collections.OrderedDict()
    for w in windows:
        for s in w['sites']:
            e = agg.setdefault(s['stack'], dict({k: 0.0 for k in FIELDS}, src=0, dst=0))
            for k in FIELDS:
                e[k] += s[k] / len(windows)
            e['src'] |= int(s['src'], 16)
            e['dst'] |= int(s['dst'], 16)
    base = windows[0]['base']
    dll = os.path.join(a.exe_dir, 'rexgpu-xenos.dll')
    names = symbolize(sorted({x for st in agg for x in st}), base, dll)
    total = sum(w['batches'] for w in windows) / len(windows)
    ends = sum(w['ends'] for w in windows) / len(windows)
    print('run %s: %d window(s), frames %d-%d: %.1f batches/frame (%.1f ending a render pass), %d sites' % (
        a.run, len(windows), windows[0]['first'], windows[-1]['last'], total, ends, len(agg)))
    print('(sites listed only from the top 40 of each window: a site missing from a window counts 0 there)')
    rows = sorted(agg.items(), key=lambda kv: -kv[1]['per_frame'])[:a.top]
    for i, (st, e) in enumerate(rows):
        print('%2d. %6.1f/frame  cmds %.1f  buf shared %.1f edram %.1f other %.1f  img %.1f  rp-ends %.1f  src %s -> dst %s'
              % (i, e['per_frame'], e['cmds'], e['shared'], e['edram'], e['other'], e['images'], e['rp_ends'],
                 stages('%X' % e['src']), stages('%X' % e['dst'])))
        for x in st:
            print('      %s' % names.get(x, '0x%X (outside the module)' % x))


if __name__ == '__main__':
    main()
