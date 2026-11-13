"""Lane 242 (H3): does the wrestlers' VS SphereOccP / SphereOccV (c168-c171) follow the game's formula, from the scene record?

    py -I tools\\skip\\sphere_check.py CAPTURE_DIR [--frames A:B[:STEP]] [--game DIR] [--lag N] [--plant NAME] [--list]

CAPTURE_DIR is a capture folder (the one holding frames\\ and scene\\; its parent holds manifest.json with the wrestler ids,
slot N = actor:slot:N). Reads game files for the wrestlers' skeleton BIND matrices only (player's own, read-only,
tools\\assets\\skel_check.py), the capture, nothing else; writes nothing. 0 game runs.

The formula (CARRY_DARKENING.md 2, SKIP_GAME_WORK_MAP.md 18.8; guest code sub_826DF8E0, sub_82C32380 lerp, sub_82C32A88 vec x mat):
  W(n)  = the skeleton's WORLD node matrix (obj+192), row-vector form. The record holds the SKINNING matrices (obj+200 = inverse
          bind x world), so W(n) = bind(n) x skin(n), bind from the skeleton file. The CPU computed the spheres BEFORE this frame's
          skeleton update: use the record of frame - 1 (--lag 1, the default).
  for sphere i, bone A = node of role a_i, bone B = node of role b_i (defaults (6,5) and (10,9): L upper arm / L clavicle and
  R upper arm / R clavicle):
    P.xyz = (off_i, 1) x W(A)          off_i = the character table's local offset (cols 5-7), fitted once per object here
    P.w   = radius (12)                                            [then, with a move-contact record: P.xyz = contact*blend + P.xyz*(1-blend)]
    V.xyz = normalize(0.5 * W(A).row0 + 0.5 * W(B).row0)
    V.w   = max(0, dot(W(A).row0, (Rz(angle_i) x W(B)).row0) * scale)   angle +90 / -90, scale 1.4
  c168 = P0, c169 = P1, c170 = V0, c171 = V1.
Plants (the checker must fail them): skin (use the skin rows as if they were world: lane 227's 3.4-4.8 miss), lag0 (this frame's record),
  strength_a (dot with the wrong row of B), swap (spheres swapped).
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import ledger as L  # noqa: E402

# role -> bone names (default table, variant 0; the roles are 6/5 and 10/9, found by the fit, names from the skeleton file)
SPHERES = (
    dict(a='Bip L UpperArm', b='Bip L Clavicle', angle=90.0, scale=1.4, radius=12.0),
    dict(a='Bip R UpperArm', b='Bip R Clavicle', angle=-90.0, scale=1.4, radius=12.0),
)


def default_game():
    """game_data\\extracted of this checkout, or of the main checkout when this is an agent worktree (.claude\\worktrees\\X)."""
    p = REPO
    for _ in range(4):
        c = os.path.join(p, 'game_data', 'extracted')
        if os.path.isdir(c):
            return c
        p = os.path.dirname(p)
    return os.path.join(REPO, 'game_data', 'extracted')


def load_bind(game, char_id):
    import pac
    import skel_check
    d = pac.load(os.path.join(game, 'pac', 'ch/ch%d.pac' % char_id))
    nodes, worldc, _ = skel_check.parse(dict(pac.entries(d, 8))['p0/0'])
    return [n['name'] for n in nodes], np.array([w.T for w in worldc])   # row-vector form


def rz_row(deg):
    t = np.radians(deg)
    c, s = np.cos(t), np.sin(t)
    return np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])


def predict(W, off, sp, plant=None):
    """W = world matrices (n, 4, 4). Returns [(P xyzw, V xyzw)] for the two spheres."""
    out = []
    for i, s in enumerate(sp):
        A, B = W[s['ai']], W[s['bi']]
        P = np.append(off[i] @ A[:3, :3] + A[3, :3], s['radius'])
        ax = 0.5 * A[0, :3] + 0.5 * B[0, :3]
        ax = ax / np.linalg.norm(ax)
        rot = rz_row(s['angle']) @ B[:3, :3]
        row = 1 if plant != 'strength_a' else 2
        strength = max(0.0, float(A[0, :3] @ rot[0 if plant != 'strength_a' else 2]) * s['scale'])
        out.append((P, np.append(ax, strength)))
    if plant == 'swap':
        out = out[::-1]
    return out


def recorded_check(cap, reg, fr, offsets=(0, 1, 2)):
    """--recorded: the lane 242 recorder's rows (kScSphereOcc, hook frame stamped) against the stream's c168-c171, bit for bit.
    A row is found by (engine object, hook frame = record guest frame - offset); the offset with the most equal draws is the hook-to-record offset (caster_contrib.h kHookToRecord is 1)."""
    rows = {}
    for fi in fr:
        rec = cap.record(fi)
        for r in rec.sphere_occ():
            rows[(r['object'], r['frame'])] = r
    if not rows:
        print('RECORDED no kScSphereOcc rows in these records (a capture made before lane 242, or the recorder was not registered)')
        return None
    res = {o: [0, 0, 0] for o in offsets}   # equal, differ, missing
    skipped = {o: 0 for o in offsets}       # spheres the game skipped (not compared)
    for fi in fr:
        f = cap.frame(fi)
        g = cap.record(fi).guest_frame
        seen = set()
        for i in range(f.ndraws):
            o = f.origin(i)
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not (o['pass_site'] == L.SITE_A and vn and 'sphere_occ' in vn) or o['object'] in seen:
                continue
            seen.add(o['object'])
            b = f.bank(d['vs_bank'])
            v = [struct.unpack('<4f', struct.pack('<4I', *b[4 * r:4 * r + 4])) for r in (168, 169, 170, 171)]
            for off in offsets:
                r = rows.get((o['object'], g - off))
                if r is None:
                    res[off][2] += 1
                    continue
                # Only spheres the game computed in that call (ran bit) are compared; a skipped sphere's output is stale in the component and the stream carries the reset
                # constant of sub_82BCC7F8 instead (0x8206F2D0: (0,0,0,1)), which a consumer substitutes. Those are counted apart.
                good = True
                for i2 in (0, 1):
                    if i2 >= r['count'] or not r['ran'][i2]:
                        skipped[off] += 1
                        continue
                    for a, b2 in ((tuple(r['p'][i2]), v[i2]), (tuple(r['v'][i2]), v[2 + i2])):
                        good = good and struct.pack('<4f', *a) == struct.pack('<4f', *b2)
                res[off][0 if good else 1] += 1
    for off in offsets:
        print('RECORDED hook frame = record frame - %d: equal %d, differ %d, no row %d, spheres skipped by the game (not compared) %d' % (off, *res[off], skipped[off]))
    best = max(offsets, key=lambda o: res[o][0])
    return res[best][0] > 0 and res[best][1] == 0 and res[best][2] == 0


def ulps(a, b):
    a32 = np.float32(a)
    return abs(float(a) - float(b)) / float(np.spacing(np.abs(a32)) if a32 != 0 else 1e-45)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default=None, help='A:B[:STEP] (default all)')
    ap.add_argument('--game', default=default_game())
    ap.add_argument('--lag', type=int, default=1)
    ap.add_argument('--plant', default=None)
    ap.add_argument('--tol-p', type=float, default=2e-4, help='centre tolerance (units; float32 ulp at 700 is 6e-5)')
    ap.add_argument('--tol-v', type=float, default=2e-6)
    ap.add_argument('--fix-off', default=None, help='JSON [[x,y,z],[x,y,z]]: hold the character-table offsets instead of fitting them')
    ap.add_argument('--recorded', action='store_true', help='compare the lane 242 recorder rows in the records with the stream (bit for bit) instead of the formula')
    a = ap.parse_args()
    cap = L.Capture(a.capture)
    reg = L.Registry(os.path.join(REPO, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    if a.recorded:
        nfr = len(os.listdir(os.path.join(a.capture, 'frames')))
        if a.frames:
            pp = [int(x) for x in a.frames.split(':')]
            frr = range(pp[0], pp[1], pp[2] if len(pp) > 2 else 1)
        else:
            frr = range(1, nfr)
        ok = recorded_check(cap, reg, frr)
        print('VERDICT %s' % ('MATCH' if ok else 'MISMATCH' if ok is False else 'NO ROWS'))
        return 0 if ok else 1
    man = json.load(open(os.path.join(a.capture, '..', 'manifest.json')))
    wrestlers = [int(x) for x in man['where']['match']['wrestlers']]
    nf = len(os.listdir(os.path.join(a.capture, 'frames')))
    if a.frames:
        p = [int(x) for x in a.frames.split(':')]
        fr = range(p[0], p[1], p[2] if len(p) > 2 else 1)
    else:
        fr = range(a.lag, nf)
    lag = 0 if a.plant == 'lag0' else a.lag
    skel = {}
    state = {}      # object -> dict(sp, off)
    tot = dict(draws=0, objects=0, p_ok=0, p_bad=0, v_ok=0, v_bad=0, w_ok=0, w_bad=0, rad_ok=0, rad_bad=0, norec=0, bank_differs=0)
    maxe = dict(p=0.0, v=0.0, w=0.0)
    bad_frames = {}
    rec_cache = {}

    def record(fi):
        if fi not in rec_cache:
            rec_cache.clear()
            rec_cache[fi] = cap.record(fi)
        return rec_cache[fi]

    for fi in fr:
        f = cap.frame(fi)
        seen = {}
        for i in range(f.ndraws):
            o = f.origin(i)
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not (o['pass_site'] == L.SITE_A and vn and 'sphere_occ' in vn):
                continue
            b = f.bank(d['vs_bank'])
            v = [struct.unpack('<4f', struct.pack('<4I', *b[4 * r:4 * r + 4])) for r in (168, 169, 170, 171)]
            tot['draws'] += 1
            if o['object'] in seen:
                if seen[o['object']] != v:
                    tot['bank_differs'] += 1
                continue
            seen[o['object']] = v
            rec = record(fi - lag)
            eng = rec.engine_objects()
            objs = rec.objects()
            bones = rec.bones()
            k = next((k for k, e in enumerate(eng) if e == o['object'] and objs[k]['bones_count']), None)
            if k is None:
                tot['norec'] += 1
                continue
            slot = int(objs[k]['id'].split(':')[-1]) if objs[k]['id'].startswith('actor:slot:') else None
            if slot is None or slot >= len(wrestlers):
                tot['norec'] += 1
                continue
            if wrestlers[slot] not in skel:
                skel[wrestlers[slot]] = load_bind(a.game, wrestlers[slot])
            names, Br = skel[wrestlers[slot]]
            skin = np.array(bones[objs[k]['bones_first']:objs[k]['bones_first'] + objs[k]['bones_count']], np.float64).reshape(-1, 4, 4)
            if len(skin) != len(Br):
                tot['norec'] += 1
                continue
            W = skin if a.plant == 'skin' else np.einsum('nij,njk->nik', Br, skin)
            st = state.get(o['object'])
            if st is None:
                sp = [dict(s, ai=names.index(s['a']), bi=names.index(s['b'])) for s in SPHERES]
                if a.fix_off:
                    off = [np.array(x) for x in json.loads(a.fix_off)]
                else:
                    off = []
                    for i2, s in enumerate(sp):
                        M = W[s['ai']]
                        off.append(np.linalg.solve(M[:3, :3].T, np.array(v[i2][:3]) - M[3, :3]))
                st = state[o['object']] = dict(sp=sp, off=off, slot=slot, char=wrestlers[slot])
                tot['objects'] += 1
            pr = predict(W, st['off'], st['sp'], a.plant)
            ok = True
            for i2 in (0, 1):
                P, V = pr[i2]
                sp_ = np.array(v[i2]); sv = np.array(v[2 + i2])
                ep = float(np.abs(P[:3] - sp_[:3]).max())
                ev = float(np.abs(V[:3] - sv[:3]).max())
                ew = abs(V[3] - sv[3])
                maxe['p'] = max(maxe['p'], ep); maxe['v'] = max(maxe['v'], ev); maxe['w'] = max(maxe['w'], ew)
                for key, good in (('p', ep <= a.tol_p), ('v', ev <= a.tol_v), ('w', ew <= a.tol_v * 4)):
                    tot[key + ('_ok' if good else '_bad')] += 1
                    ok &= good
                tot['rad_ok' if abs(P[3] - sp_[3]) < 1e-6 else 'rad_bad'] += 1
            if not ok:
                bad_frames.setdefault(o['object'], []).append(fi)
    print('capture %s' % a.capture)
    print('lag %d  plant %s' % (lag, a.plant))
    for ob, st in state.items():
        print('object %08X slot %d char %d  bones A/B: %s  offsets %s' % (
            ob, st['slot'], st['char'], [(s['a'], s['b']) for s in st['sp']], [np.round(x, 3).tolist() for x in st['off']]))
    print('sphere_occ draws %d, distinct objects/frame checked %d, bank differs between draws of one object-frame: %d, no record: %d' % (
        tot['draws'], sum(tot[k] for k in ('v_ok', 'v_bad')) // 2, tot['bank_differs'], tot['norec']))
    print('centre P (c168/c169) within %g: %d ok / %d bad (max err %.3g)' % (a.tol_p, tot['p_ok'], tot['p_bad'], maxe['p']))
    print('axis V.xyz (c170/c171) within %g: %d ok / %d bad (max err %.3g)' % (a.tol_v, tot['v_ok'], tot['v_bad'], maxe['v']))
    print('strength V.w within %g: %d ok / %d bad (max err %.3g)' % (a.tol_v * 4, tot['w_ok'], tot['w_bad'], maxe['w']))
    print('radius P.w == 12: %d ok / %d bad' % (tot['rad_ok'], tot['rad_bad']))
    for ob, fl in bad_frames.items():
        print('object %08X: %d frames with any mismatch: %s' % (ob, len(fl), fl[:20]))
    bad = tot['p_bad'] + tot['v_bad'] + tot['w_bad'] + tot['rad_bad']
    print('VERDICT %s' % ('MATCH' if bad == 0 and tot['p_ok'] > 0 else 'MISMATCH (%d)' % bad))
    return 0 if bad == 0 and tot['p_ok'] > 0 else 1


if __name__ == '__main__':
    sys.exit(main())
