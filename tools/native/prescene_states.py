"""The render state of every pass of one captured frame (Session 43, low-spec step 5.1): per pass (grouped as
pass_gpu.py groups them) its surface, colour / depth formats, and for its draws the primitives, depth test / write /
function, stencil, colour masks and RB_COLORCONTROL values, with the pass's GPU time when the capture has sparse GPU
marks (anatomy_NNN_K_gpu.csv). Numbers only.

    py tools\\native\\prescene_states.py logs\\anatomy_NNN_K.csv [--frame 0] [--before-scene]
"""
import argparse
import csv
import os
from collections import Counter, defaultdict

COLOR_FMT = {0: '8888', 1: '8888G', 2: '2_10_10_10', 3: '2_10_10_10F', 4: '16_16', 5: '16x4', 6: '16_16F',
             7: '16x4F', 10: '2_10x3_AS_10x4', 12: '2_10F_AS_16x4', 14: '32F', 15: '32_32F'}
PRIM = {1: 'pt', 2: 'line', 4: 'tri', 5: 'fan', 6: 'strip', 8: 'rect', 13: 'quad'}
ZFUNC = ['never', 'less', 'equal', 'lequal', 'greater', 'notequal', 'gequal', 'always']


def u(v):
    return int(v, 16) if v else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--before-scene', action='store_true')
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv, newline='')) if int(r['frame']) == a.frame]
    gpu = defaultdict(float)
    gpu_path = a.csv[:-4] + '_gpu.csv'
    if os.path.exists(gpu_path):
        for g in csv.DictReader(open(gpu_path, newline='')):
            if int(g['frame']) == a.frame and g['gpu_row_us'] and g['kind'] in ('draw', 'copy', 'prep_begin'):
                gpu[int(g['idx'])] += float(g['gpu_row_us'])
    passes, cur = [], None
    for r in rows:
        if r['kind'] == 'copy':
            if cur is None:
                cur = {'key': None, 'draws': [], 'copies': [], 'us': 0.0}
                passes.append(cur)
            cur['copies'].append(r)
            cur['us'] += gpu.get(int(r['idx']), 0.0)
            continue
        key = (r['rb_surface_info'], r['rb_color_info0'], r['rb_depth_info'], r['rb_modecontrol'])
        if cur is None or cur['key'] != key or cur['copies']:
            cur = {'key': key, 'draws': [], 'copies': [], 'us': 0.0}
            passes.append(cur)
        cur['draws'].append(r)
        cur['us'] += gpu.get(int(r['idx']), 0.0)
    zw = lambda p: sum(1 for r in p['draws'] if u(r['rb_depthcontrol']) & 4)
    scene = max(range(len(passes)), key=lambda i: zw(passes[i]))
    last = scene if a.before_scene else len(passes)
    for i, p in enumerate(passes[:last]):
        if not p['key']:
            continue
        s, c, d, m = (u(x) for x in p['key'])
        prims, states = Counter(), Counter()
        for r in p['draws']:
            dc = u(r['rb_depthcontrol'])
            z = 'z-' if not dc & 2 else f"z:{ZFUNC[(dc >> 4) & 7]}{'+w' if dc & 4 else ''}"
            st = ' st' if dc & 1 else ''
            prims[PRIM.get(int(r['prim'] or 0), r['prim'])] += 1
            states[f"{z}{st} mask {u(r['rb_color_mask']):04X} cc {u(r['rb_colorcontrol']):08X}"] += 1
        print(f"pass {i:3d} p{s & 0x3FFF:<5d} x{1 << ((s >> 16) & 3)} mode {m} c@{c & 0xFFF}:{COLOR_FMT.get((c >> 16) & 0xF, (c >> 16) & 0xF)} "
              f"z@{d & 0xFFF}:{'20e4' if (d >> 16) & 1 else 'u24'} draws {len(p['draws'])} "
              f"({' '.join(f'{k} {v}' for k, v in prims.items())}) copies {len(p['copies'])} {p['us'] / 1000:.3f} ms")
        for k, v in states.most_common():
            print(f"        {v:4d}  {k}")


if __name__ == '__main__':
    main()
