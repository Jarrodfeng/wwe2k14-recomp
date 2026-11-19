r"""S103 lane V1: the arena's stage visibility, from the files (offline, 0 game runs).

The game decides which stage-slot models it draws through the arena's visibility tree (pac entry 50020, "CStageVisibleTestNode"; docs\formats\stage_visibility.md).
This tool is the independent reader and the proof:

  py -I tools\arena\stage_vis.py tree ARENA                       # parse 50020, join it to the slots, print the numbers
  py -I tools\arena\stage_vis.py groups                           # the time-cycled slot groups (entries 60010-60025) of every arena
  py -I tools\arena\stage_vis.py check CAPTURE:ARENA [--frames A:B:STEP] [--plant NAME] [--json OUT]
        per sampled frame: the slots the stream's stage-site draws joined (tools\arena\join_census.py) against the slots OUR rule keeps
        (box vs the frame's ViewProj frustum), site A and the mirrored site B apart. Exit 1 on a false reject (a slot the game drew that we drop).
Plants (check only, must FAIL): no_frustum (keeps everything: the cost is not saved, the check demands a saving), narrow (frustum shrunk to 60 % in x: false rejects), display (the plain display frustum without the stage's x term: false rejects).
Numbers only. Nothing from the game is written."""
import argparse
import collections
import json
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import pac  # noqa: E402
import arena_files as af  # noqa: E402

NODE_RE = re.compile(r'NODE MY_NAME = (\S+)(?: PARENT_NAME = (\S+))?\s*\{\s*([^}]*)\}\s*(\d+)')


def read_entries(arena_id, want):
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    out = {}
    for eid, b in pac.pach(data, p):
        if want(eid) if callable(want) else eid in want:
            out.setdefault(eid, pac.decode(b))
    return out


def parse_tree(text):
    """-> {name: {'parent', 'lo', 'hi', 'kids'}}; lo / hi = the file's 3ds-max-space box (x, y, z up). Order kept."""
    nodes = collections.OrderedDict()
    for m in NODE_RE.finditer(text):
        v = [float(x) for x in m.group(3).split()]
        if len(v) != 6:
            raise ValueError('node %s: %d numbers' % (m.group(1), len(v)))
        nodes[m.group(1)] = {'parent': m.group(2), 'lo': v[:3], 'hi': v[3:], 'kids': int(m.group(4))}
    return nodes


def max_to_model(lo, hi):
    """The tree's box (max space: x, y, z up) in the models' file space (x, -z, y): see docs\\formats\\stage_visibility.md."""
    return (np.array([lo[0], -hi[2], lo[1]], np.float32), np.array([hi[0], -lo[2], hi[1]], np.float32))


def slot_node_names(arena_id):
    """-> {slot: [node names of the slot's JBOY]} (the model's own node table, 80-byte records, name first)."""
    raw = read_entries(arena_id, lambda e: 0 <= e < 400)
    out = {}
    for e, d in raw.items():
        if d[:4] != b'JBOY':
            continue
        body = d[8:8 + struct.unpack_from('>I', d, 4)[0]]
        nn, no = struct.unpack_from('>I', body, 0x18)[0], struct.unpack_from('>I', body, 0x20)[0]
        out[e] = [bytes(body[no + 80 * i:no + 80 * i + 16]).split(b'\0')[0].decode('latin1') for i in range(nn)]
    return out


class StageVis:
    """The tree joined to the slots: box[slot] = (lo, hi) in file space, for the slots that have a tree node (the others are never tested: the game draws them)."""

    def __init__(self, arena_id):
        self.arena_id = arena_id
        ents = read_entries(arena_id, (50020,))
        if 50020 not in ents:
            raise SystemExit('bg%02d: no entry 50020' % arena_id)
        self.nodes = parse_tree(ents[50020].decode('cp932', 'replace'))
        self.names = slot_node_names(arena_id)
        self.box = {}
        self.node_of = {}
        for slot, names in self.names.items():
            for nm in names:
                if nm in self.nodes:
                    n = self.nodes[nm]
                    self.box[slot] = max_to_model(n['lo'], n['hi'])
                    self.node_of[slot] = nm
                    break


def planes_from_vp(vp):
    """Six planes (unit normals, inside when n.p + d >= 0) from a column-vector clip matrix (clip = VP * [p; 1], the stream's c56..c59 rows), D3D depth 0..w."""
    r = [np.asarray(vp[i], np.float64) for i in range(4)]
    raw = [r[3] + r[0], r[3] - r[0], r[3] + r[1], r[3] - r[1], r[2], r[3] - r[2]]
    return np.array([p / np.linalg.norm(p[:3]) for p in raw])


ASPECT_16_9 = 16.0 / 9.0   # the stage frustum's x term at the 16:9 screen mode: the constant at 0x8202B710 [C: image read]; 4:3 mode 0x82004D8C = 1.3333; any other mode 1.0


SLACK = 1.05   # OURS, not the game's: the volume is rebuilt from the display ViewProj, not from the game's camera words; measured: misses of 0.4 % (arena 4 slot 229, frames 140-150, 2 % was 0.1 % short)


def stage_planes(vp, term=ASPECT_16_9):
    """The game's stage frustum as OUR camera sees it (docs\\formats\\stage_visibility.md): y = the display's (tan theta = 1 / P11), x = tan(theta x term) where the game's sub_82769B98
    scales the half ANGLE by the screen-mode constant, widened to the display's own x when the display is wider (no 16:9 limit: a wider screen sees more)."""
    vp = np.asarray(vp, np.float64).copy()
    p00, p11 = np.linalg.norm(vp[0][:3]), np.linalg.norm(vp[1][:3])
    kx = max(1.0, np.tan(np.arctan(1.0 / p11) * term) * p00)   # stage tan_x / display tan_x
    vp[0] /= kx * SLACK   # (row 0 carries the x clip coordinate: dividing widens the clip volume in x only)
    vp[1] /= SLACK
    return planes_from_vp(vp)


def box_visible(pl, lo, hi, flip_x=False):
    """The game's reject (sub_82769820): d = n.(c - P) <= -half rejects, half = 0.5 sum |n_i e_i|. Same test with a box centre and a plane distance."""
    if flip_x:
        lo, hi = np.array([-hi[0], lo[1], lo[2]], np.float64), np.array([-lo[0], hi[1], hi[2]], np.float64)
    c, h = (np.asarray(lo, np.float64) + hi) / 2, (np.asarray(hi, np.float64) - lo) / 2
    for p in pl:
        if p[:3] @ c + p[3] <= -(np.abs(p[:3]) @ h):
            return False
    return True


def cmd_tree(a):
    sv = StageVis(a.arena)
    print('arena %d: %d tree nodes, %d slot models, %d slots joined by model name' % (a.arena, len(sv.nodes), len(sv.names), len(sv.box)))
    unjoined = sorted(set(sv.names) - set(sv.box))
    print('slots without a node (never tested): %s' % unjoined)
    parents = collections.Counter(n['parent'] for n in sv.nodes.values())
    print('kids count equals the number of children: %s' % all(parents.get(k, 0) == v['kids'] for k, v in sv.nodes.items()))
    # the tree box equals the model's mesh box (the node is the model's own AABB)
    arena = af.load_arena(a.arena)
    worst = 0.0
    for s, (lo, hi) in sv.box.items():
        ps = [m.positions for m in arena.slots.get(s, []) if m.positions is not None]
        if not ps:
            continue
        p = np.concatenate(ps)
        dd = max(float(np.abs(p.min(0) - lo).max()), float(np.abs(p.max(0) - hi).max()))
        if dd > 2.0:
            print('  slot %d node %s differs from the mesh box by %.1f (flags %s)' % (s, sv.node_of[s], dd, sorted(arena.flags.get(s, ()))))
        worst = max(worst, dd)
    print('largest difference between a node box and its model\'s mesh box: %.3f' % worst)


def cmd_joins(a):
    """Every arena: how many slots the name join gives a box, and how many of those differ from the slot's own mesh box (name collisions, placed entrance pieces)."""
    tot = collections.Counter()
    for n in af.arena_ids():
        try:
            sv = StageVis(n)
        except SystemExit:
            continue
        arena = af.load_arena(n)
        for s, (lo, hi) in sv.box.items():
            ps = [m.positions for m in arena.slots.get(s, []) if m.positions is not None]
            if not ps:
                continue
            p = np.concatenate(ps)
            dd = max(float(np.abs(p.min(0) - lo).max()), float(np.abs(p.max(0) - hi).max()))
            node = sv.node_of[s]
            ent = False
            while node:
                ent = ent or node.startswith('ENTRANCE')
                node = sv.nodes[node]['parent']
            tot['joined'] += 1
            if dd > 2.0:
                tot['differ'] += 1
                tot['differ_under_ENTRANCE'] += ent
    print(dict(tot))


def cmd_groups(a):
    for n in af.arena_ids():
        e = read_entries(n, lambda x: 60010 <= x <= 60025)
        for k, d in sorted(e.items()):
            nums = [int(x) for x in re.findall(r'-?\d+', d.decode('cp932', 'replace'))]
            pairs = list(zip(nums[0::2], nums[1::2]))
            print('arena %d entry %d: (slot, ticks) %s total %d' % (n, k, pairs, sum(t for _, t in pairs)))


def cmd_check(a):
    import join_census as jc
    import variant_table as vt
    path, arena_id = a.capture.rsplit(':', 1)
    arena_id = int(arena_id)
    if not os.path.isabs(path) and not os.path.exists(path):
        path = os.path.join(jc.MAIN, path)   # (captures live in the main checkout's out\, also when this runs from a worktree)
    sv = StageVis(arena_id)
    arena = af.load_arena(arena_id)
    cap = jc.Capture(path)
    fi = jc.FileIndex(arena, None)
    for s in list(sv.box):   # as stage_tree.h Fill: a box is used only when the slot's own meshes confirm it (2 units), and is then the UNION of the two
        ps = [m.positions for m in arena.slots.get(s, []) if m.positions is not None]
        lo, hi = sv.box[s]
        if not ps:
            del sv.box[s]
            continue
        p = np.concatenate(ps)
        if max(float(np.abs(p.min(0) - lo).max()), float(np.abs(p.max(0) - hi).max())) > 2.0:
            del sv.box[s]
        else:
            sv.box[s] = (np.minimum(lo, p.min(0)), np.maximum(hi, p.max(0)))
    lo_f, hi_f, step = (a.frames.split(':') + ['1'])[:3] if a.frames else ('0', str(len(cap.frames)), '10')
    hi_f = str(min(int(hi_f), len(cap.frames)))
    cache = {}
    prev_rej = None
    tot = collections.Counter()
    rows = []
    r_slots = {s for s, fl in arena.flags.items() if 'r' in fl}
    for fn in range(int(lo_f), int(hi_f), int(step)):
        j = vt.joined_draws(cap, arena, fi, fn, cache)
        f = cap.frame(fn, None)
        cap.advance_contents(f, fn) if hasattr(cap, 'advance_contents') else None
        o, d = f.origins, f.draws
        # the camera's pass = the ViewProj most site-A draws use; a draw under another ViewProj is another pass (shadow-map views of the `p` slots) and says nothing about the camera's volume
        by_vp = collections.Counter()
        rows = []
        for i, cands in j.items():
            site = int(o[i]['pass_site'])
            key = f.reg(int(d[i]['vs_bank']), 56, 4).tobytes()
            rows.append((site, key, cands))
            if site == jc.SITE_A:
                by_vp[key] += 1
        if not by_vp:
            continue
        main_key = by_vp.most_common(1)[0][0]
        vp = np.frombuffer(main_key, np.float32).reshape(4, 4).copy()
        mirror_key = vp.copy()
        mirror_key[:, 0] *= -1
        mirror_key = mirror_key.tobytes()   # the mirrored site's ViewProj is the camera's with x negated (join_census: mirror)
        drawn = {jc.SITE_A: set(), jc.SITE_B: set()}
        for site, key, cands in rows:
            if key != (main_key if site == jc.SITE_A else mirror_key):
                tot['other_pass_draws'] += 1
                continue
            for s, _m in cands:
                drawn[site].add(s)
        pl = planes_from_vp(vp) if a.plant == 'display' else stage_planes(vp)   # (plant 'display': the plain display frustum, no stage term)
        if a.plant == 'narrow':
            vp2 = vp.copy()
            vp2[0] *= 1.6
            pl = planes_from_vp(vp2)
        tested = {s for s in sv.box if not any(t.startswith('m(') for t in arena.flags.get(s, ()))}   # (a slot with an m(N) motion is placed by the stage key: its node box is local, so it is not tested here, it stays drawn)
        if a.margin:   # the smallest widening k (x and y of the clip volume x k) that keeps every drawn slot: how much looser than the display frustum the game's is
            need = {}
            for site, flip in ((jc.SITE_A, False), (jc.SITE_B, True)):
                for s in drawn[site] & tested:
                    for k in (1.0, 1.05, 1.1, 1.2, 1.3, 1.5, 1.75, 2.0, 3.0):
                        v2 = vp.copy()
                        v2[0] /= k
                        v2[1] /= k
                        if box_visible(planes_from_vp(v2), *sv.box[s], flip_x=flip):
                            need[(site == jc.SITE_B, s)] = k
                            break
                    else:
                        need[(site == jc.SITE_B, s)] = 99
            print('frame %4d margin k needed: %s' % (fn, {('B' if b else 'A') + str(s): k for (b, s), k in sorted(need.items()) if k > 1.0}))
        keepA = {s for s in tested if a.plant == 'no_frustum' or box_visible(pl, *sv.box[s])}
        keepB = {s for s in tested & r_slots if a.plant == 'no_frustum' or box_visible(pl, *sv.box[s], flip_x=True)}
        dA, dB = drawn[jc.SITE_A] & tested, drawn[jc.SITE_B] & tested
        rej_now = frozenset([('A', x) for x in tested - keepA] + [('B', x) for x in (tested & r_slots) - keepB])   # the set the applier folded into its state hash before review 3
        if prev_rej is not None:
            tot['consecutive_frames'] += 1
            tot['reapplies_if_hashed'] += rej_now != prev_rej
        prev_rej = rej_now
        frA, frB = dA - keepA, dB - keepB
        # the slots our rule keeps that the stream did not draw: the look flags (e = events only, g = game only: one of the two looks hides them), `w`, or unexplained (the join
        # misses a draw, a slot the game also hides by another rule): counted apart so the box rule is judged on what it decides
        for s in keepA - dA:
            fl = arena.flags.get(s, set())
            if fl & {'e', 'g'} and not fl >= {'e', 'g'}:
                tot['kept_not_drawn_look'] += 1
            elif 'w' in fl:
                tot['kept_not_drawn_w'] += 1
            else:
                tot['kept_not_drawn_other'] += 1
                if a.list_other:
                    tot['other_slot_%d' % s] += 1
        nm = {s: len(arena.slots[s]) for s in tested if s in arena.slots}   # mesh draws a copy costs (one stream draw per mesh part)
        tot['mesh_draws_tested'] += sum(nm.values()) + sum(nm[s] for s in tested & r_slots if s in nm)
        tot['mesh_draws_dropped'] += sum(nm[s] for s in tested - keepA if s in nm) + sum(nm[s] for s in (tested & r_slots) - keepB if s in nm)
        tot['frames'] += 1
        tot['drawnA'] += len(dA)
        tot['drawnB'] += len(dB)
        tot['false_reject'] += len(frA) + len(frB)
        tot['tested_slots'] += len(tested) + len(tested & r_slots)
        tot['kept'] += len(keepA) + len(keepB)
        tot['dropped'] += len(tested) - len(keepA) + len(tested & r_slots) - len(keepB)
        rows.append({'frame': fn, 'drawnA': sorted(dA), 'drawnB': sorted(dB), 'keepA': len(keepA), 'keepB': len(keepB), 'frA': sorted(frA), 'frB': sorted(frB)})
        print('frame %4d  A: drawn %2d kept %3d false-reject %s   B: drawn %2d kept %3d false-reject %s' % (fn, len(dA), len(keepA), sorted(frA), len(dB), len(keepB), sorted(frB)))
    print('TOTAL', dict(tot))
    if a.json:
        json.dump(rows, open(a.json, 'w'))
    ok = tot['false_reject'] == 0 and tot['frames'] > 0
    if a.plant == 'no_frustum':
        ok = tot['dropped'] > 0   # the plant keeps everything: the demand "something is dropped" fails
    print('VERDICT', 'PASS' if ok else 'FAIL')
    if a.expect_fail:   # a planted twin: the check must have failed
        print('EXPECTED FAIL: %s' % ('caught' if not ok else 'NOT CAUGHT'))
        return 0 if not ok else 1
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('tree')
    p.add_argument('arena', type=int)
    sub.add_parser('groups')
    sub.add_parser('joins')
    p = sub.add_parser('check')
    p.add_argument('capture')
    p.add_argument('--frames', default='')
    p.add_argument('--plant', default='')
    p.add_argument('--json', default='')
    p.add_argument('--margin', action='store_true')
    p.add_argument('--expect-fail', dest='expect_fail', action='store_true')
    p.add_argument('--list-other', dest='list_other', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'tree':
        return cmd_tree(a)
    if a.cmd == 'groups':
        return cmd_groups(a)
    if a.cmd == 'joins':
        return cmd_joins(a)
    return cmd_check(a)


if __name__ == '__main__':
    sys.exit(main() or 0)
