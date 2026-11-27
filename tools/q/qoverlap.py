r"""q overlap: can two draw families overlap on screen (so a `q draworder` DIFF between them can change a pixel)? (S111 QOVERLAP, 0 game runs)

    py tools/q/q.py overlap ARENA FAMILY_A FAMILY_B [--cams N] [--cam chamber|match] [--json]

FAMILY_A / FAMILY_B are the names of `q draworder --families`. A family has static geometry here only where the player's own files hold it:
  stage_a          the arena slots flagged `a` (additive), twins by their r / y flags                (bgNN.pac, entry 50001 + the JBOY meshes)
  stage_s          the slots flagged `s` without `a` (translucent)
  arena            the other match-drawn stage slots (no `a`, no `s`; motion slots m(N) left out)
  ring, screens    the ring keys 951..982, the screen keys 800..809 of the arena pac
  chamber_glass    the pod glass (bgEtc.pac chamber CHAI key 5 mesh 1) at the four pods x four quarter turns   (arena 21 only)
  chamber_fences   (none: the CDTB fence mesh is not located) -> unknown
Any other family (bodies, particles, 2D rows, props ...) moves or has no file geometry: the answer is "unknown", never a guess.

THE TEST. Cameras are sampled across the game camera's rule (docs/systems/camera.md, "The match game camera (CAMGAME)"): the eye on the
sphere of the framing centre at the rule's distance range and pitch, yaw all round (the four compass sides plus the easing between), the
rule's FOV (vertical, 16:9). Rays go from each camera through sample points of family B's triangles; a ray COUNTS when it is inside the
frustum, hits family A too, and nothing opaque (the match-drawn non-`a` / non-`s` slots, ring, screens, the chamber's own CHAI pieces,
minus the two families) lies nearer than BOTH hits: then both are visible there and the order of the two draws can change that pixel.
Output: rays in frustum, rays with an A hit, rays that count, per camera (the cameras with a count, the best few); --json has every camera.
Limits: static geometry at the file pose (no motion slots, no wrestlers); an unknown family is unknown; the match camera's framing centre
height is assumed (marked [U] in the output); the rays are samples, so "no" means no sampled ray, not a proof.
Selftest: py -I tools/q/qoverlap_selftest.py
"""
import argparse
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'filecensus'))

# Camera rules (docs/systems/camera.md CAMGAME). dist = eye distance range (framing radius 300..800 x the rule's factor; radius >= 300 held by
# sub_825796F8), pitch = degrees looking down, fovv = vertical field of view in degrees, cy = framing centre height (file space, up = -y).
CAMERAS = {
    'chamber': dict(dist=(1200.0, 3200.0), pitch=13.0, fovv=17.0, cy=-200.0, note='chamber: distance radius x 4, pitch 13, FOV 17 fixed'),
    'match': dict(dist=(1500.0, 4000.0), pitch=19.5, fovv=12.5, cy=-100.0, note='match: distance radius x 5, pitch 13 + 6.5, FOV 12.5; centre height [U] assumed'),
}
CHAMBER_ARENAS = (21,)
ASPECT = 16.0 / 9.0
# The four pods of the chamber (table 0x83623F40, x y z, file space) and the four quarter turns the glass is drawn at.
PODS = [(-450.0, -120.0, -450.0), (-450.0, -120.0, 450.0), (450.0, -120.0, 450.0), (450.0, -120.0, -450.0)]


class OverlapError(Exception):
    pass


# ------------------------------------------------------------------ geometry
def tris_of(m):
    """a FileMesh -> (N, 3, 3) triangles (the strips with 0xFFFF restarts, degenerates dropped)"""
    if m.positions is None:
        return np.zeros((0, 3, 3), np.float64)
    out, strip = [], []
    for v in list(m.indices) + [0xFFFF]:
        if v == 0xFFFF:
            for i in range(len(strip) - 2):
                a, b, c = strip[i], strip[i + 1], strip[i + 2]
                if a != b and b != c and a != c and max(a, b, c) < len(m.positions):
                    out.append((a, b, c))
            strip = []
        else:
            strip.append(int(v))
    if not out:
        return np.zeros((0, 3, 3), np.float64)
    return m.positions[np.array(out)].astype(np.float64)


def twins(T, toks):
    res = [T]
    if 'r' in toks:
        R = T.copy()
        R[..., 0] *= -1
        res.append(R)
    if 'y' in toks:
        Y = T.copy()
        Y[..., 0] *= -1
        Y[..., 2] *= -1
        res.append(Y)
    return res


def rot(T, i):
    c, s = [(1, 0), (0, 1), (-1, 0), (0, -1)][i]
    R = T.copy()
    R[..., 0] = c * T[..., 0] + s * T[..., 2]
    R[..., 2] = -s * T[..., 0] + c * T[..., 2]
    return R


def _cat(lst):
    return np.concatenate(lst) if lst else np.zeros((0, 3, 3), np.float64)


def arena_sets(arena):
    """-> {'stage_a': tris, 'stage_s': tris, 'arena': tris, 'ring': tris, 'screens': tris} from the arena's pac (file space)"""
    import arena_files as af
    a = af.load_arena(arena, extra_keys=True)
    sets = {k: [] for k in ('stage_a', 'stage_s', 'arena', 'ring', 'screens')}
    for slot, meshes in a.slots.items():
        toks = a.flags.get(slot, set())
        if any(t.startswith('m(') for t in toks):
            continue
        if slot < 400:
            if not af.filecensus_flags.drawn_in_match(slot, toks, a.ents, 'match'):
                continue
            key = 'stage_a' if 'a' in toks else ('stage_s' if 's' in toks else 'arena')
        else:
            key = 'screens' if slot in af.SCREEN_KEYS else ('ring' if slot in af.RING_KEYS else None)
            if key is None:
                continue
        for m in meshes:
            T = tris_of(m)
            if len(T):
                sets[key] += twins(T, toks)
    return {k: _cat(v) for k, v in sets.items()}


def chamber_sets():
    """-> (glass tris, [CHAI k 0 / k 2 / k 4 occluder tris]) from bgEtc.pac PACH 6 (the chamber CHAI keys), at the four pods"""
    import arena_files as af
    pac = af.pac
    data = pac.load(os.path.join(pac.GAME, 'pac', 'bg', 'bgEtc.pac'))
    p0 = 0x4000 if data[:4] == b'EPAC' else 0
    heads = [q for q in range(p0, len(data) - 8, 0x800) if data[q:q + 4] == b'PACH']
    chai = {}
    for eid, raw in pac.pach(data, heads[6]):
        dec = pac.decode(raw)
        if bytes(dec[:4]) == b'PACH':
            for ceid, craw in pac.pach(dec):
                cd = pac.decode(craw)
                if bytes(cd[:4]) == b'JBOY':
                    chai[ceid] = af.parse_jboy(ceid, cd)
    glass, occ = [], []
    for m in chai.get(5, []):
        if m.index == 1:
            T = tris_of(m)
            for pp in PODS:
                for i in range(4):
                    glass.append(rot(T, i) + np.array(pp))
    occ += [tris_of(chai[0][0]), tris_of(chai[0][1]), tris_of(chai[2][0])]
    for m in chai.get(4, []):
        T = tris_of(m)
        for pp in PODS:
            for i in range(4):
                occ.append(rot(T, i) + np.array(pp))
    return _cat(glass), _cat(occ)


class Scene:
    """the geometry of one arena by family name; `fam(name)` -> tris or None (unknown); `occluders(skip)` -> every opaque triangle bar those families"""

    def __init__(self, arena, sets, glass=None, chai_occ=None):
        self.arena, self.sets, self.glass, self.chai_occ = arena, sets, glass, chai_occ

    def fam(self, name):
        s = self.sets
        if name in ('stage_a', 'stage_s', 'arena', 'ring', 'screens'):
            return s[name]
        if name == 'chamber_glass':
            return self.glass
        return None

    def occluders(self, skip):
        parts = [self.sets[k] for k in ('arena', 'ring', 'screens') if k not in skip]
        if self.chai_occ is not None:
            parts.append(self.chai_occ)
        return _cat(parts)


def load_scene(arena):
    sets = arena_sets(arena)
    if arena in CHAMBER_ARENAS:
        g, o = chamber_sets()
        return Scene(arena, sets, g, o)
    return Scene(arena, sets)


# ------------------------------------------------------------------ rays
def hits(o, d, T):
    """nearest-hit parameters of the ray o + t d against each triangle of T (inf = miss), Moller-Trumbore"""
    e1 = T[:, 1] - T[:, 0]
    e2 = T[:, 2] - T[:, 0]
    pv = np.cross(d, e2)
    det = (e1 * pv).sum(1)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
    tv = o - T[:, 0]
    u = (tv * pv).sum(1) * inv
    qv = np.cross(tv, e1)
    v = (qv * d).sum(1) * inv
    t = (e2 * qv).sum(1) * inv
    m = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-3)
    return np.where(m, t, np.inf)


def make_cameras(rule, n):
    """n cameras across the rule: distance over its range (4 steps), pitch fixed, yaw spread all round; -> list of dict(eye, fwd, right, up, tanv, tanh, D, yaw)"""
    r = CAMERAS[rule]
    nd = 4 if n >= 4 else max(n, 1)
    ds = np.linspace(r['dist'][0], r['dist'][1], nd)
    pitch = math.radians(r['pitch'])
    centre = np.array([0.0, r['cy'], 0.0])
    cams = []
    for i in range(n):
        D = float(ds[i % nd])
        yaw = 2 * math.pi * i / n
        fwd = np.array([math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)])   # +y = down
        right = np.array([math.cos(yaw), 0.0, -math.sin(yaw)])
        up = np.cross(fwd, right)
        tv = math.tan(math.radians(r['fovv'] / 2))
        cams.append(dict(eye=centre - fwd * D, fwd=fwd, right=right, up=up, tanv=tv, tanh=tv * ASPECT, D=D, yaw=round(math.degrees(yaw), 1)))
    return cams


def sample_points(T, max_tris=300):
    """four points per triangle (centroid + three off-centre), at most max_tris triangles spread evenly"""
    if not len(T):
        return np.zeros((0, 3))
    if len(T) > max_tris:
        T = T[np.linspace(0, len(T) - 1, max_tris).astype(int)]
    ws = np.array([(1 / 3, 1 / 3, 1 / 3), (0.6, 0.2, 0.2), (0.2, 0.6, 0.2), (0.2, 0.2, 0.6)])
    return np.einsum('wk,nkc->nwc', ws, T).reshape(-1, 3)


def overlap_rays(A, B, OCC, cams, max_tris=300, plant=None):
    """-> dict(cams=[{D, yaw, in_frustum, a_hit, both}], in_frustum, a_hit, both): the module's test (see the file docstring).
    plant (selftest twins only): 'no_occluders' = nothing blocks; 'any_a' = every ray in the frustum counts as an A hit."""
    P = sample_points(B, max_tris)
    if plant == 'no_occluders':
        OCC = OCC[:0]
    per, tot = [], dict(in_frustum=0, a_hit=0, both=0)
    if len(A):
        lo, hi = A.reshape(-1, 3).min(0) - 1e-3, A.reshape(-1, 3).max(0) + 1e-3
    for c in cams:
        row = dict(D=c['D'], yaw=c['yaw'], in_frustum=0, a_hit=0, both=0)
        d = P - c['eye']
        dist = np.linalg.norm(d, axis=1)
        d = d / np.maximum(dist, 1e-9)[:, None]
        zf = d @ c['fwd']
        zs = np.where(zf > 0, zf, 1.0)
        inside = (zf > 0) & (np.abs(d @ c['right']) / zs <= c['tanh']) & (np.abs(d @ c['up']) / zs <= c['tanv'])
        for k in np.nonzero(inside)[0]:
            row['in_frustum'] += 1
            if not len(A):
                continue
            # slab test against A's box first: most rays miss it
            with np.errstate(divide='ignore', invalid='ignore'):
                t1, t2 = (lo - c['eye']) / d[k], (hi - c['eye']) / d[k]
            tn, tf = np.nanmax(np.minimum(t1, t2)), np.nanmin(np.maximum(t1, t2))
            if not (tf >= max(tn, 0)) and plant != 'any_a':
                continue
            ta = 0.5 if plant == 'any_a' else hits(c['eye'], d[k], A).min()
            if not np.isfinite(ta):
                continue
            row['a_hit'] += 1
            to = hits(c['eye'], d[k], OCC).min() if len(OCC) else np.inf
            if ta < to and dist[k] < to:
                row['both'] += 1
        for key in tot:
            tot[key] += row[key]
        per.append(row)
    return dict(cams=per, **tot)


def verdict(res):
    n = res['both']
    return ('yes (%d rays)' % n) if n else 'no'


def camera_rule(arena):
    return 'chamber' if arena in CHAMBER_ARENAS else 'match'


def run(arena, fa, fb, ncams=144, rule=None, scene=None, max_tris=300):
    """-> dict(answer 'yes'|'no'|'unknown', text, ...); scene may be passed to reuse one arena's geometry"""
    scene = scene or load_scene(arena)
    A, B = scene.fam(fa), scene.fam(fb)
    miss = [n for n, t in ((fa, A), (fb, B)) if t is None]
    if miss:
        return dict(answer='unknown', text='unknown (no static geometry for %s)' % ', '.join(miss), arena=arena, a=fa, b=fb)
    if not len(A) or not len(B):
        return dict(answer='no', text='no (%s has no triangles in arena %d)' % (fa if not len(A) else fb, arena), arena=arena, a=fa, b=fb, rays=0)
    rule = rule or camera_rule(arena)
    cams = make_cameras(rule, ncams)
    res = overlap_rays(A, B, scene.occluders({fa, fb}), cams, max_tris)
    res.update(arena=arena, a=fa, b=fb, rule=rule, answer='yes' if res['both'] else 'no', rays=res['both'], tris_a=len(A), tris_b=len(B))
    res['text'] = verdict(res)
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(prog='q overlap', description=__doc__.split('\n')[0])
    ap.add_argument('arena', type=int)
    ap.add_argument('fam_a')
    ap.add_argument('fam_b')
    ap.add_argument('--cams', type=int, default=144, help='cameras sampled across the rule (default 144)')
    ap.add_argument('--cam', choices=sorted(CAMERAS), default=None, help='camera rule (default: chamber for arena 21, else match)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    r = run(a.arena, a.fam_a, a.fam_b, a.cams, a.cam)
    if a.json:
        print(json.dumps(r, indent=1, default=str))
        return 0
    print('overlap arena %d %s | %s: %s' % (a.arena, a.fam_a, a.fam_b, r['text']))
    if 'cams' in r:
        rule = CAMERAS[r['rule']]
        print('  camera rule %s (%d cameras); %s' % (r['rule'], len(r['cams']), rule['note']))
        print('  triangles: %s %d, %s %d; rays in frustum %d, with a %s hit %d, both visible %d' % (
            a.fam_a, r['tris_a'], a.fam_b, r['tris_b'], r['in_frustum'], a.fam_a, r['a_hit'], r['both']))
        withc = sorted((c for c in r['cams'] if c['both']), key=lambda c: -c['both'])
        print('  cameras with a counting ray: %d of %d' % (len(withc), len(r['cams'])))
        for c in withc[:8]:
            print('    D %.0f yaw %5.1f: %d rays (%d in frustum, %d with a %s hit)' % (c['D'], c['yaw'], c['both'], c['in_frustum'], c['a_hit'], a.fam_a))
        if len(withc) > 8:
            print('    ... %d more cameras (all: --json)' % (len(withc) - 8))
    return 0


if __name__ == '__main__':
    sys.exit(main())
