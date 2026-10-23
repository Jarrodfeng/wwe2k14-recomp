"""GPU time per render pass of one captured frame (Session 42): the frame-anatomy rows grouped into passes as
post_chain.py groups them, with the sparse GPU marks' row times (anatomy_NNN_K_gpu.csv) summed per pass.

    py tools\\native\\pass_gpu.py logs\\anatomy_NNN_K.csv [--frame 0]
"""
import argparse
import csv
import os
from collections import defaultdict


def u(v):
    return int(v, 16) if v else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--all', action='store_true', help='every pass, not only from the scene on (Session 43)')
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv, newline='')) if int(r['frame']) == a.frame]
    gpu_path = a.csv[:-4] + '_gpu.csv'
    gpu = defaultdict(float)
    tail = 0.0
    if os.path.exists(gpu_path):
        for g in csv.DictReader(open(gpu_path, newline='')):
            if int(g['frame']) != a.frame or not g['gpu_row_us']:
                continue
            if g['kind'] in ('draw', 'copy', 'prep_begin'):
                gpu[int(g['idx'])] += float(g['gpu_row_us'])
            else:
                tail += float(g['gpu_row_us'])
    passes, cur = [], None
    for r in rows:
        idx = int(r['idx'])
        if r['kind'] == 'copy':
            if cur is None:
                cur = {'key': None, 'draws': [], 'copies': [], 'us': 0.0}
                passes.append(cur)
            cur['copies'].append(r)
            cur['us'] += gpu.get(idx, 0.0)
            continue
        key = (r['rb_surface_info'], r['rb_color_info0'], r['rb_depth_info'], r['rb_modecontrol'])
        if cur is None or cur['key'] != key or cur['copies']:
            cur = {'key': key, 'draws': [], 'copies': [], 'us': 0.0}
            passes.append(cur)
        cur['draws'].append(r)
        cur['us'] += gpu.get(idx, 0.0)
    zw = lambda p: sum(1 for r in p['draws'] if u(r['rb_depthcontrol']) & 4)
    scene = max(range(len(passes)), key=lambda i: zw(passes[i]))
    total = sum(p['us'] for p in passes)
    print(f'frame {a.frame}: {len(passes)} passes, GPU {total / 1000:.3f} ms in rows (+{tail / 1000:.3f} other); '
          f'scene pass {scene}')
    before = sum(p['us'] for p in passes[:scene])
    print(f'before the scene {before / 1000:.3f} ms, scene {passes[scene]["us"] / 1000:.3f} ms, after '
          f'{sum(p["us"] for p in passes[scene + 1:]) / 1000:.3f} ms')
    first = 0 if a.all else scene
    for i, p in enumerate(passes[first:], first):
        s = u(p['key'][0]) if p['key'] else 0
        c = u(p['key'][1]) if p['key'] else 0
        d = u(p['key'][2]) if p['key'] else 0
        zw_n = zw(p)
        print(f'  pass {i:3d}: p{s & 0x3FFF:<5d} x{1 << ((s >> 16) & 3)} mode {u(p["key"][3]) if p["key"] else 0} '
              f'cfmt {(c >> 16) & 0xF:2d} c@{c & 0xFFF:<4d} z@{d & 0xFFF:<4d} zfmt {(d >> 16) & 1} '
              f'draws {len(p["draws"]):5d} (zwrite {zw_n:4d}) copies {len(p["copies"])}  {p["us"] / 1000:.3f} ms')


if __name__ == '__main__':
    main()
