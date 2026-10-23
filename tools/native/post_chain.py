"""The post chain of one frame (Session 42): every render pass after the scene, its target, its draws, the textures
they sample and which earlier resolve wrote them, and the resolves that end it. Numbers only.

Reads a frame-anatomy CSV with the POSTMAP columns (SDK 36fd3bb+: copy_control, copy_pitch, tex), e.g. from route
short\\s42_postmap.txt.

    py tools\\native\\post_chain.py logs\\anatomy_NNN_K.csv [--frame 0] [--all] [--from-scene]

--from-scene: start at the scene's colour resolve (the pass with the most depth-writing draws).
"""
import argparse
import csv
from collections import Counter

COLOR_FMT = {0: '8888', 1: '8888G', 2: '2_10_10_10', 3: '2_10_10_10F', 4: '16_16', 5: '16x4', 6: '16_16F',
             7: '16x4F', 10: '2_10x3_AS_10x4', 12: '2_10F_AS_16x4', 14: '32F', 15: '32_32F'}
TEX_FMT = {2: 'k_8', 6: 'k_8888', 7: 'k_2_10_10_10', 10: 'k_8_8', 16: 'k_10_11_11', 17: 'k_11_11_10', 18: 'DXT1',
           19: 'DXT23', 20: 'DXT45', 22: 'k_24_8', 23: 'k_24_8F', 24: 'k_16', 25: 'k_16_16', 26: 'k_16x4',
           31: 'k_16_16F', 32: 'k_16x4F', 36: 'k_32F', 37: 'k_32_32F'}
COPY_FMT = TEX_FMT  # RB_COPY_DEST_INFO copy_dest_format uses the texture (ColorFormat) numbering
PRIM = {1: 'pt', 2: 'line', 4: 'tri', 5: 'fan', 6: 'strip', 8: 'rect', 13: 'quad'}


def u(v):
    return int(v, 16) if v else 0


def surf(r):
    s = u(r['rb_surface_info'])
    c = u(r['rb_color_info0'])
    d = u(r['rb_depth_info'])
    return f"p{s & 0x3FFF} x{1 << ((s >> 16) & 3)} c@{c & 0xFFF}:{COLOR_FMT.get((c >> 16) & 0xF, (c >> 16) & 0xF)} z@{d & 0xFFF}"


def rect(r):
    """The viewport rectangle in guest pixels (x0,y0)-(x1,y1), or the screen scissor for screen-space draws."""
    import struct
    f = lambda v: struct.unpack('<f', struct.pack('<I', u(v)))[0]
    xs, xo, ys, yo = (f(r['pa_cl_vport_xscale']), f(r['pa_cl_vport_xoffset']), f(r['pa_cl_vport_yscale']),
                      f(r['pa_cl_vport_yoffset']))
    tl, br = u(r['pa_sc_window_scissor_tl']), u(r['pa_sc_window_scissor_br'])
    sc = f"sc({tl & 0x7FFF},{(tl >> 16) & 0x7FFF})-({br & 0x7FFF},{(br >> 16) & 0x7FFF})"
    return f"vp({xo - abs(xs):.0f},{yo - abs(ys):.0f})-({xo + abs(xs):.0f},{yo + abs(ys):.0f}) {sc}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--all', action='store_true', help='print every pass (default: from the scene on)')
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv, newline='')) if int(r['frame']) == a.frame]
    if not rows or 'tex' not in rows[0]:
        raise SystemExit('no rows, or no POSTMAP columns (needs SDK 36fd3bb or later)')
    passes, cur = [], None
    for r in rows:
        if r['kind'] == 'copy':
            if cur is None:
                cur = {'key': None, 'draws': [], 'copies': []}
                passes.append(cur)
            cur['copies'].append(r)
            continue
        key = (r['rb_surface_info'], r['rb_color_info0'], r['rb_color_info1'], r['rb_depth_info'])
        if cur is None or cur['key'] != key or cur['copies']:
            cur = {'key': key, 'draws': [], 'copies': []}
            passes.append(cur)
        cur['draws'].append(r)
    # The scene: the pass with the most depth-writing draws (the frame's scene target).
    zw = lambda p: sum(1 for r in p['draws'] if u(r['rb_depthcontrol']) & 4)
    scene_index = max(range(len(passes)), key=lambda i: zw(passes[i]))
    scene_key = passes[scene_index]['key']
    first = 0 if a.all else scene_index
    written = {}  # base address -> (pass index, copy index in frame, description)
    copy_no = 0
    print(f"frame {a.frame}: {len(rows)} rows, {len(passes)} passes; scene target {scene_key}; from pass {first}")
    for i, p in enumerate(passes):
        for c in p['copies']:
            written[u(c['copy_dest'])] = (i, copy_no, f"copy#{copy_no}")
            c['_no'] = copy_no
            copy_no += 1
        if i < first:
            continue
        d = p['draws']
        if d:
            prims = Counter(PRIM.get(int(r['prim']), r['prim']) for r in d)
            shaders = Counter((r['vs_hash'][:6], r['ps_hash'][:6]) for r in d)
            blend = Counter(r['rb_colorcontrol'] for r in d)
            print(f"\npass {i}: {surf(d[0])}  draws {len(d)} ({', '.join(f'{k} {v}' for k, v in prims.items())}),"
                  f" zwrite {zw(p)}, shader pairs {len(shaders)}, colormask {d[0]['rb_color_mask']}")
            seen = set()
            for r in d[:12]:
                texs = []
                for t in r['tex'].split():
                    slot, base, fmt, size, dim, tiled = t.split(':')
                    b = int(base, 16)
                    src = written.get(b)
                    texs.append(f"t{slot}={TEX_FMT.get(int(fmt), fmt)} {size}"
                                + (f" <- {src[2]} (pass {src[0]})" if src else f" @{base}"))
                line = (f"  {r['idx']:>5} {PRIM.get(int(r['prim']), r['prim'])} vs {r['vs_hash'][:8]} ps "
                        f"{r['ps_hash'][:8]} {rect(r)} blend {r['rb_colorcontrol'][-4:]}/{r.get('rb_depthcontrol','')}"
                        f" | {'; '.join(texs)}")
                if line not in seen:
                    print(line)
                    seen.add(line)
            if len(d) > 12:
                print(f"  ... {len(d) - 12} more draws")
        for c in p['copies']:
            ctl, info, pitch = u(c.get('copy_control', '')), u(c['copy_info']), u(c.get('copy_pitch', ''))
            src = ctl & 7
            print(f"  copy#{c['_no']} {surf(c)} src {'depth' if src == 4 else f'rt{src}'} -> @{c['copy_dest']} "
                  f"fmt {COPY_FMT.get((info >> 7) & 0x3F, (info >> 7) & 0x3F)} {pitch & 0x3FFF}x{(pitch >> 16) & 0x3FFF}"
                  f"{' clear-colour' if ctl & 0x100 else ''}{' clear-depth' if ctl & 0x200 else ''}"
                  f" cmd {(ctl >> 20) & 3}")


if __name__ == '__main__':
    main()
