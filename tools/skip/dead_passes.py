r"""Lane 175 (S101): the dead-work census. Offline, 0 game runs. For every frame of a capture, which of the stream's ops produce an image that NOTHING reads afterwards?

An op (draw, clear, copy) is DEAD when the image it writes is not read by any later op before being fully rewritten (backward liveness over the whole capture in stream order: frame N's
writes can be read by frame N+1 and later), and is not the frame's presented surface (the last pass with draws, an assumption printed in the header). A pass is dead when all its ops are.
This is the candidate list for an invisible cut of the GAME's work: a pass whose output nobody reads can be left unrecorded without a pixel changing. It says nothing about OUR readers: the
capture's slots are the stream's own reads, which is what ours reads too in place, so a map read only by draws ours replaces counts as read (conservative).

  py -I tools\skip\dead_passes.py CAPTURE.zip [--frames A:B:STEP] [--min-frames N] [--json OUT] [-q]
Output: one row per pass class {size, target image, pass site, vs effects}: frames seen, draws, prims, % of those ops dead, and the whole-pass-dead count. Counts only, nothing from the game."""
import argparse
import collections
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
import cards_census as cc  # noqa: E402

OP_TARGETS, OP_CLEAR, OP_DRAW, OP_COPY, OP_PRESENT = 0, 1, 2, 3, 4
CLEAR_DT = np.dtype([('flags', 'u1'), ('mask', 'u1'), ('stencil', 'u1'), ('full', 'u1'), ('color', '<f4', (4,)), ('depth', '<f4'), ('rect', '<i4', (4,))])
PRESENT_DT = np.dtype([('image', '<u4'), ('mip', 'u1'), ('layer', 'u1'), ('pad', '<u2'), ('rect', '<i4', (4,))])


def frame_nodes(cap, f):
    """-> list of nodes in stream order: dict(kind, k, passi, w (images written), r (images read), prims) and the pass list."""
    copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
    clears_t = cc.table(f, cc.pcap.SEC['clears'], CLEAR_DT)
    pres_t = cc.table(f, cc.pcap.SEC['presents'], PRESENT_DT)
    D = f.draws
    sl, tf, tc = f.slots, D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
    nodes, passes, cur, curinfo = [], [], None, None
    for k, (kind, idx) in enumerate(f.ops):
        kind, idx = int(kind), int(idx)
        if kind == OP_TARGETS:
            t = f.targets[idx]
            cols = [int(t['color'][c]['image']) for c in range(int(t['count']))]
            dep = int(t['depth']['image']) if int(t['depth']['image']) else 0
            curinfo = {'colors': cols, 'depth': dep, 'size': cap.wh(cols[0] if cols else dep), 'pos': k, 'ops': []}
            passes.append(curinfo)
            continue
        if kind == OP_PRESENT:
            n = {'kind': 'present', 'k': k, 'w': [], 'r': [int(pres_t[idx]['image'])], 'prims': 0, 'full': False, 'pass': len(passes) - 1}
            if curinfo is not None:
                curinfo['ops'].append(n)
            nodes.append(n)
            continue
        if curinfo is None:
            continue
        if kind == OP_CLEAR:
            c = clears_t[idx]
            w = (curinfo['colors'] if c['flags'] & 1 else []) + ([curinfo['depth']] if (c['flags'] & 6 and curinfo['depth']) else [])
            n = {'kind': 'clear', 'k': k, 'w': w, 'r': [], 'prims': 0, 'full': bool(c['full'])}
        elif kind == OP_DRAW:
            r = [int(x['image']) for x in sl[int(tf[idx]):int(tf[idx]) + int(tc[idx])]]
            ic = int(D['index_count'][idx]) or int(D['vertex_count'][idx])
            n = {'kind': 'draw', 'k': k, 'w': curinfo['colors'] + ([curinfo['depth']] if curinfo['depth'] else []), 'r': r, 'prims': ic // 3, 'd': idx, 'full': False}
        elif kind == OP_COPY:
            c = copies_t[idx]
            n = {'kind': 'copy', 'k': k, 'w': [int(c['d']['image'])], 'r': [int(c['s']['image'])], 'prims': 0, 'full': False}
        else:
            continue
        n['pass'] = len(passes) - 1
        curinfo['ops'].append(n)
        nodes.append(n)
    return nodes, passes


def classify(cap, f, passes):
    """Label every pass: (size, first colour image, pass site of its first draw, up to 2 vs effects)."""
    out = []
    veff = cap.effs(f.draws['vs_hash']) if len(f.draws) else []
    for p in passes:
        ds = [n for n in p['ops'] if n['kind'] == 'draw']
        sites, effs = collections.Counter(), collections.Counter()
        for n in ds:
            if f.has_origins:
                sites[int(f.origins['pass_site'][n['d']])] += 1
            effs[str(veff[n['d']])] += 1
        site = sites.most_common(1)[0][0] if sites else 0
        top = ','.join(e for e, _ in effs.most_common(2))
        out.append((p['size'], p['colors'][0] if p['colors'] else p['depth'], '%08X' % site, top))
    return out


def run(cap, frames):
    per = []
    for fi in frames:
        f = cap.frame(fi)
        nodes, passes = frame_nodes(cap, f)
        per.append((fi, f, nodes, passes))
    # presented surface assumption: the colour target of the last pass that has a draw
    live = set()
    dead_by_node = {}
    for fi, f, nodes, passes in reversed(per):
        for n in reversed(nodes):
            alive = n['kind'] == 'clear' and False
            if n['kind'] == 'present':
                alive = True
            elif any(w in live for w in n['w']):
                alive = True
            n['alive'] = alive
            if n['kind'] == 'clear':
                if alive and n['full']:
                    for w in n['w']:
                        live.discard(w)
            elif alive:
                live.update(n['r'])
                if n['kind'] == 'copy':
                    pass
    return per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--min-frames', type=int, default=1)
    ap.add_argument('--json')
    ap.add_argument('-q', action='store_true')
    ap.add_argument('--all', action='store_true', help='list classes with no dead op too')
    a = ap.parse_args()
    cap = cc.Cap(a.capture)
    total = len(cap.frames)
    if a.frames:
        s, e, st = (a.frames.split(':') + ['', '', ''])[:3]
        idx = list(range(int(s or 0), min(total, int(e or total)), int(st or 1)))
    else:
        idx = list(range(total))
    per = run(cap, idx)
    cls = collections.OrderedDict()
    for fi, f, nodes, passes in per[:-1]:  # the last frame's liveness is unknown beyond the capture: not counted
        labels = classify(cap, f, passes)
        for pi, p in enumerate(passes):
            c = cls.setdefault(labels[pi], {'frames': set(), 'passes': 0, 'dead_passes': 0, 'draws': 0, 'dead_draws': 0, 'prims': 0, 'dead_prims': 0, 'ops': 0, 'dead_ops': 0})
            c['frames'].add(fi)
            c['passes'] += 1
            ops = p['ops']
            c['ops'] += len(ops)
            c['dead_ops'] += sum(1 for n in ops if not n['alive'])
            for n in ops:
                if n['kind'] == 'draw':
                    c['draws'] += 1
                    c['prims'] += n['prims']
                    if not n['alive']:
                        c['dead_draws'] += 1
                        c['dead_prims'] += n['prims']
            if ops and all(not n['alive'] for n in ops):
                c['dead_passes'] += 1
    rows = []
    for k, c in cls.items():
        rows.append((k, c))
    rows.sort(key=lambda kc: -kc[1]['dead_ops'])
    nfr = len(per) - 1
    print('dead-work census: %s, %d frames read (%d counted); presented = the Present ops' % (os.path.basename(a.capture), len(per), nfr))
    print('%-11s %-8s %-9s %-44s %6s %6s %7s %9s %8s %7s' % ('size', 'target', 'site', 'vs effects', 'frames', 'passes', 'deadpas', 'draws', 'deaddrw', 'dead%'))
    out = []
    for k, c in rows:
        if (c['dead_ops'] == 0 and not a.all) or len(c['frames']) < a.min_frames:
            continue
        pct = 100.0 * c['dead_ops'] / max(1, c['ops'])
        print('%-11s %-8s %-9s %-44s %6d %6d %7d %9d %8d %6.1f%%' % (str(k[0]), k[1], k[2], k[3][:44], len(c['frames']), c['passes'], c['dead_passes'], c['draws'], c['dead_draws'], pct))
        out.append({'size': k[0], 'target': k[1], 'site': k[2], 'effects': k[3], 'frames': len(c['frames']), 'passes': c['passes'], 'dead_passes': c['dead_passes'],
                    'draws': c['draws'], 'dead_draws': c['dead_draws'], 'prims': c['prims'], 'dead_prims': c['dead_prims'], 'ops': c['ops'], 'dead_ops': c['dead_ops']})
    tot_ops = sum(c['ops'] for c in cls.values())
    tot_dead = sum(c['dead_ops'] for c in cls.values())
    print('all: %d ops, %d dead (%.2f%%)' % (tot_ops, tot_dead, 100.0 * tot_dead / max(1, tot_ops)))
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
