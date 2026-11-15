r"""S102 lane P3 (offline step): the plane shadow chain's four passes in a frame's ops, found the way the binder's PlanePlan finds them (src\renderer_adapter\binder.h). Shared by plane_skip_cut.py, plane_leftovers.py and own_plane_check.py.

A frame's chain = four passes in a row, each a SetTargets (op kind 0) whose first colour target has the role shadow_map, an optional Clear (1), its draws (2) and ONE Copy (3):
  0 the mask      1+ draws of pass site 0x82BF4D38 (T_PlaneShadowMap; the ring proxy 956 and the wrestler proxy)
  1 the downscale ONE draw of call site 0x828159FC (yDownScale)
  2, 3 the blurs  ONE draw each of call site 0x827EEC84 (yBlur plane_x, plane_z)
Numbers only. Nothing from the game is written."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

SITE_MASK, CALL_DOWN, CALL_BLUR = sc.SITE_PLANE, sc.CALL_DOWN, sc.CALL_PBLUR
ROLE_SHADOW = sc.ROLE_NAMES.index('shadow_map')
KINDS = (0, 1, 2, 2)


class Chain:
    """passes: list of dict(kind, set_op, clear_op or -1, draws [op indices], copy_op, target (capture image id), dest (capture image id))."""
    __slots__ = ('passes', 'first_op', 'end_op', 'ops', 'outside')

    def ops_of(self):
        out = []
        for p in self.passes:
            out.append(p['set_op'])
            if p['clear_op'] >= 0:
                out.append(p['clear_op'])
            out += p['draws']
            out.append(p['copy_op'])
        return out


def find(cap, f):
    """-> (Chain, '') or (None, why): why '' = no mask draw in the frame (no chain at all), else the reason the chain is irregular."""
    if not f.has_origins or not len(f.ops):
        return None, ''
    n = len(f.ops)
    o = f.origins
    ps = o['pass_site'].astype(np.int64)
    cs = sc.pcap.queue_site(o).astype(np.int64)
    kinds = np.array([int(k) for k, _ in f.ops]) if n else np.zeros(0, int)
    idx = np.array([int(i) for _, i in f.ops]) if n else np.zeros(0, int)
    first_mask = next((i for i in range(n) if kinds[i] == 2 and int(ps[idx[i]]) == SITE_MASK), None)
    if first_mask is None:
        return None, ''
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    i = first_mask
    while i > 0 and kinds[i] != 0:
        i -= 1
    if kinds[i] != 0:
        return None, 'no SetTargets before the first mask draw'
    ch = Chain()
    ch.passes = []
    ch.first_op = i
    for k in range(4):
        if i >= n or kinds[i] != 0:
            return None, 'pass %d does not start with a SetTargets' % k
        t = f.targets[int(idx[i])]
        img = int(t['color'][0]['image']) if int(t['count']) else int(t['depth']['image'])
        rec = cap.imgs.get(img)
        if rec is None or int(rec['role']) != ROLE_SHADOW:
            return None, 'pass %d target %d has not the role shadow_map' % (k, img)
        p = dict(kind=KINDS[k], set_op=i, clear_op=-1, draws=[], copy_op=-1, target=img, dest=-1)
        i += 1
        if i < n and kinds[i] == 1:
            p['clear_op'] = i
            i += 1
        while i < n and kinds[i] == 2:
            d = int(idx[i])
            if p['kind'] == 0:
                ok = int(ps[d]) == SITE_MASK
            elif p['kind'] == 1:
                ok = int(cs[d]) == CALL_DOWN
            else:
                ok = int(cs[d]) == CALL_BLUR and int(ps[d]) != SITE_MASK
            if not ok:
                return None, 'pass %d holds a draw that is not its own' % k
            p['draws'].append(i)
            i += 1
        if not p['draws'] or (p['kind'] != 0 and len(p['draws']) != 1) or i >= n or kinds[i] != 3:
            return None, 'pass %d has no draws / not one draw / no copy' % k
        p['copy_op'] = i
        p['dest'] = int(copies[int(idx[i])]['d']['image'])
        i += 1
        ch.passes.append(p)
    ch.end_op = i
    ch.ops = n
    # review F10c: mask / downscale / blur site draws of the frame that are NOT in the chain found (a second chain, a stray draw)
    inside = set(ch.ops_of())
    ch.outside = sum(1 for o in range(n) if kinds[o] == 2 and o not in inside and (int(ps[idx[o]]) == SITE_MASK or int(cs[idx[o]]) in (CALL_DOWN, CALL_BLUR)))
    return ch, ''


def ids_ok(row, ids):
    """review F10b: the chain's copy destinations are the images the checks read: the mask's picture ids[0] and the blur result ids[1] (three copies into it)."""
    return row.get('dests') == [ids[0], ids[1], ids[1], ids[1]]


def facts(cap, frames=None):
    """Per frame: dict(chain bool, why, mask, blur, clears, ops, draws(total of the frame), copies, targets). One pass over the capture."""
    out = []
    for fi in (frames if frames is not None else range(len(cap.frames))):
        f = cap.c.frame(fi)
        ch, why = find(cap, f)
        kinds = np.array([int(k) for k, _ in f.ops]) if len(f.ops) else np.zeros(0, int)
        row = dict(frame=fi, chain=ch is not None, why=why, ops=int(len(kinds)), targets=int((kinds == 0).sum()), clears=int((kinds == 1).sum()), draws=int((kinds == 2).sum()), copies=int((kinds == 3).sum()))
        if ch is not None:
            row.update(outside=ch.outside, mask=len(ch.passes[0]['draws']), chain_clears=sum(1 for p in ch.passes if p['clear_op'] >= 0), chain_ops=len(ch.ops_of()), dests=[p['dest'] for p in ch.passes], targets_ids=[p['target'] for p in ch.passes])
        out.append(row)
    return out
