r"""Lane SC (S101, docs\renderer\SHADOW_PROMOTION_MAP.md section 7 lane SC): the shadow census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's sections 1-2 on case zips / capture folders, on every frame: which passes draw shadows and how many draws a
frame (the plane mask, the props map + circles, wrestler maps A / B, the coverage reductions and min blurs, the rim light map that is NOT a shadow), the targets and
copies of each chain, who reads the final images (slot, effect family), the casters by object and class (wrestler / piece / prop / ring proxy / wrestler proxy), the
chCommon.pac entry 0 join (the wrestlers' plane-shadow proxy) and the ring entry 956 join (bit-equal positions, part = mesh, index counts), the shadow vertex layouts against
the same (object, part)'s colour draw element by element, the light matrices and their registers (one per frame per chain, the circle = Trapezoidal x ViewProj, the readers'
projection bias), the plane pass constants, the palette rows against the colour pass (the collapsed bones), the provenance of the read images, the surface roles (the rim map
is tagged shadow_map), and the arena's `p` slots (they never cast).

  py -I tools\shadow\shadow_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--expect FILE] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--emit-expect OUT] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder (relative paths are read from the main checkout). LABEL names the capture's entry in expect.json (brock, bryan, wm30);
the entry also names the arena (the pac whose ring and stage slots are joined). Exit 1 when any check FAILs or an input cannot be read.
Checks: names, passes, order, targets, copies, readers, casters, chcommon_join, ring_join, proxy_scope, eyepos, plane_consts, layouts, xdword, lights, circle, bias, palettes,
provenance, roles, p_slots, lambert; plus "expect" (every measured group against expect.json).
--plant (tests only): proxy_part_shift (the chCommon join is made one mesh off), hash_only (the shadow passes picked by the surface role / shader alone: the rim map is swallowed),
no_names (origins stripped: a version-1 capture; must fail loudly).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
import json
import os
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
for sub in ('particles', 'solver', 'arena', 'wm30', 'ring'):
    sys.path.insert(0, os.path.join(ROOT, 'tools', sub))
import pcap  # noqa: E402
import capread  # noqa: E402
import arena_files as af  # noqa: E402
import ring_files as rf  # noqa: E402
import pac  # noqa: E402

PLANTS = ('proxy_part_shift', 'hash_only', 'no_names')
SITE_PLANE, SITE_MAP, SITE_RIM, SITE_COLOUR, SITE_MIRROR = 0x82BF4D38, 0x82C04634, 0x82BFBD58, 0x82C0A950, 0x82C0B94C
CALL_DOWN, CALL_PBLUR, CALL_CIRCLE, CALL_REDUCE, CALL_RIMRED, CALL_MINBLUR = 0x828159FC, 0x827EEC84, 0x828131E4, 0x82C1F514, 0x82C1F48C, 0x82814080
LAMBERT = (0x82BF5C58, 0x82BF8118)
ORDER = ['plane', 'plane_down', 'plane_blur', 'props', 'mapA', 'mapB', 'reduce', 'min_pair', 'min_rgba', 'min_r', 'min_rg', 'rim', 'rim_reduce', 'rim_t2']
MAP_KINDS = ('props', 'mapA', 'mapB')
FMT_RGBA8, FMT_RGBA16F = 1, 16
ROLE_NAMES = ['unnamed', 'scene_color', 'scene_depth', 'shadow_map', 'post_step', 'feedback_map', 'atlas_target', 'menu_picture', 'tron_picture', 'ui', 'present', 'other', 'rim_map']
ELEM_SIZE = {109: 16, 106: 12, 41: 4, 37: 4, 92: 8, 83: 4}
IMG_DT = np.dtype([('id', '<u4'), ('w', '<u2'), ('h', '<u2'), ('d', '<u2'), ('mips', 'u1'), ('layers', 'u1'), ('format', 'u1'), ('kind', 'u1'), ('usage', 'u1'), ('source', 'u1'),
                   ('flags', 'u1'), ('gf', 'u1'), ('role', 'u1'), ('role_flags', 'u1'), ('pad', 'u1'), ('pad2', 'u1', (3,))])
IREF = [('image', '<u4'), ('mip', 'u1'), ('layer', 'u1'), ('pad', '<u2')]
COPY_DT = np.dtype([('s', IREF), ('d', IREF), ('sr', '<i4', (4,)), ('dr', '<i4', (4,)), ('filt', 'u1'), ('p', 'u1', (3,))])
assert IMG_DT.itemsize == 24 and COPY_DT.itemsize == 52
SEC_COPIES, SEC_PROV = capread.SEC['copies'], capread.SEC['provenance']


class CensusError(Exception):
    """An input that cannot be read. Never a pass."""


# ---------------------------------------------------------------- the shader registers (from the committed effect descriptions) -----------------------
def fold_rim(key, value):
    # S101 lane 86: captures taken after the rim_map role carry rim_map where older ones carry shadow_map; expect.json accepts both
    if key == 'roles' and isinstance(value, dict):
        out = {}
        for k, v in value.items():
            v = dict(v)
            if 'rim_map' in v:
                v['shadow_map'] = v.get('shadow_map', 0) + v.pop('rim_map')
            out[k] = v
        return out
    return value


def fx_registers():
    """(effect, field) -> register, from src\\renderer\\shaders\\fx\\*.fx.json 'solve' blocks. Looked up, never typed."""
    out = {}
    d = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
    for fn in sorted(os.listdir(d)):
        if fn.endswith('.fx.json'):
            j = json.load(open(os.path.join(d, fn), encoding='utf-8'))
            for st in ('vertex', 'fragment'):
                for c in j.get('solve', {}).get(st, {}).get('constants', []):
                    out.setdefault((j['effect'], st, c['field']), c['register'])
    return out


FXR = fx_registers()


def reg(effect, stage, field):
    r = FXR.get((effect, stage, field))
    if r is None:
        raise CensusError('no register for %s %s %s in the effect descriptions' % (effect, stage, field))
    return r


R_VIEWPROJ = reg('yShadowMap', 'vertex', 'ViewProj')            # 56
R_TRAP = reg('yShadowMap', 'vertex', 'Trapezoidal')             # 64
R_VIEWDEPTH = reg('yShadowMap', 'vertex', 'ViewDepth')          # 54
R_WORLD = reg('yShadowMap', 'vertex', 'WorldMatrixArray')       # 76
R_SHADOWPRM = reg('yShadowMap', 'fragment', 'ShadowPrm')        # 168 (ps bank)
R_EYE = reg('yPlaneShadowMap', 'vertex', 'EyePos')              # 72
R_PLANEPOS = reg('yPlaneShadowMap', 'vertex', 'PlanePos')       # 7
R_FADE0 = reg('yPlaneShadowMap', 'vertex', 'ShadowDistance')    # 0
R_CAMVP = reg('yPlaneShadowMap', 'vertex', 'CameraViewProj')    # 13
R_PLANEINV = reg('yPlaneShadowMap', 'vertex', 'PlaneInv')       # 9
R_CIRCLE_VP = reg('yCircleShadow', 'vertex', 'ViewProj')        # 4
R_CIRCLE_PRM = reg('yCircleShadow', 'vertex', 'ShadowParam')    # 0
R_WTEXXY = reg('yCh_skin', 'vertex', 'TexProjLightXY')          # 68
R_WTEXW = reg('yCh_skin', 'vertex', 'TexProjLightW')            # 71
R_WLIGHTDEPTH = reg('yCh_skin', 'vertex', 'ViewLightDepth')     # 166
R_FLOOR_TEX = reg('yBG_DefaultLM', 'vertex', 'TexProjLight')    # 68
R_MAT_CAMVP = reg('yProp_PSMLM', 'vertex', 'CameraViewProj')    # 234
R_MAT_PLANEINV = reg('yProp_PSMLM', 'vertex', 'PlaneInv')       # 230


def regs(bank, r, n=1):
    return bank[4 * r:4 * (r + n)].view('<f4').reshape(n, 4)


# ---------------------------------------------------------------- the capture side --------------------------------------------------------------------
class Cap:
    def __init__(self, path):
        self.c = pcap.Capture(path)
        self.frames = self.c.frames
        self.geo = self.c.geo
        name = next((n for n in self.c.zf.namelist() if n.endswith('images.bin')), None) if self.c.zf is not None else os.path.join(path, 'images.bin')
        if not name:
            raise CensusError('%s: no images.bin' % path)
        raw = self.c.read(name)
        magic, ver, count, _ = struct.unpack_from('<IIII', raw, 0)
        if len(raw) < 16 + 24 * count:
            raise CensusError('%s: images.bin does not hold %d records of 24 bytes' % (path, count))
        self.imgs = {int(r['id']): r for r in np.frombuffer(raw, IMG_DT, count, 16)}
        self._eff = {}

    def frame(self, i):
        return self.c.frame(i)

    def eff(self, h):
        e = self._eff.get(h)
        if e is None:
            e = self._eff[h] = pcap.effect_of(h)
        return e

    def effs(self, hashes):
        u, inv = np.unique(hashes, return_inverse=True)
        names = np.array([self.eff(int(x)) for x in u], dtype=object)
        return names[inv]


def table(f, sid, dt):
    if sid not in f.secs:
        return np.zeros(0, dt)
    es, cnt, off = f.secs[sid]
    if es != dt.itemsize:
        raise CensusError('frame %d: section %d element size %d in the file, %d here' % (f.idx, sid, es, dt.itemsize))
    return np.frombuffer(f.buf, dt, cnt, off)


class Pass:
    __slots__ = ('target', 'depth', 'draws', 'copies', 'clear', 'kind', 'pos')

    def __init__(self, target, depth, pos):
        self.target, self.depth, self.pos = target, depth, pos
        self.draws, self.copies, self.clear, self.kind = [], [], False, None


def walk(f, copies):
    passes, cur = [], None
    for k, (kind, idx) in enumerate(f.ops):
        kind, idx = int(kind), int(idx)
        if kind == 0:
            t = f.targets[idx]
            cur = Pass(int(t['color'][0]['image']) if t['count'] else int(t['depth']['image']), int(t['depth']['image']), k)
            passes.append(cur)
        elif cur is None:
            raise CensusError('frame %d: an op before any SetTargets' % f.idx)
        elif kind == 1:
            cur.clear = True
        elif kind == 2:
            cur.draws.append(idx)
        elif kind == 3:
            cur.copies.append(idx)
    return passes


def mm_update(d, k, v):
    cur = d.get(k)
    d[k] = [v, v] if cur is None else [min(cur[0], v), max(cur[1], v)]


def class_of_effects(effs):
    """The class of a caster object from the effects of its colour draws (the wrestler hook's rule, MAP 1.2)."""
    fam = {e.split('/')[0] for e in effs}
    if any(x.startswith(('yCh_skin', 'yCh_gear', 'yCh_Aniso', 'yCh_Refl')) for x in fam):
        return 'wrestler'
    if fam and all(x.startswith('yCh_trans') for x in fam):
        return 'piece'
    if any(x.startswith(('yProp', 'yBG')) for x in fam):
        return 'prop'
    return 'other'


class FileSide:
    """What the player's files hold that the census joins to: chCommon.pac entry 0 (the wrestlers' plane proxy), the arena's ring entry 956, the arena's stage slots."""

    def __init__(self, arena):
        self.arena = arena
        path = os.path.join(pac.GAME, 'pac', 'ch', 'chCommon.pac')
        if not os.path.exists(path):
            raise CensusError('cannot read %s' % path)
        data = pac.load(path)
        p = 0x4000 if data[:4] == b'EPAC' else 0
        while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
            p += 0x800
        raw = dict(pac.pach(data, p))
        if 0 not in raw:
            raise CensusError('chCommon.pac has no entry 0')
        self.proxy = [m for m in af.parse_jboy(0, pac.decode(raw[0])) if m.positions is not None]
        ring = rf.load_ring(arena, textures=False)
        self.ring956 = [m for m in ring.entries.get(956, []) if m.ok]
        self.a = af.load_arena(arena, True)
        self.stage = collections.defaultdict(list)
        for slot, ms in self.a.slots.items():
            for m in ms:
                if m.positions is not None:
                    self.stage[(m.n, m.stream_index_count)].append((slot, m))
        self.p_slots = sorted(s for s, t in self.a.flags.items() if 'p' in t)
        self.proxy_by = collections.defaultdict(list)
        for m in self.proxy:
            self.proxy_by[(m.n, m.stream_index_count)].append(m)
        self.ring_by = collections.defaultdict(list)
        for m in self.ring956:
            self.ring_by[(m.n, m.stream_index_count)].append(m)
        self.ring_all = collections.defaultdict(list)
        for ms in ring.entries.values():
            for m in ms:
                if m.ok:
                    self.ring_all[(m.n, m.stream_index_count)].append(m)


def layout_of(f, d):
    L = f.layouts[int(d['layout'])]
    els = [(int(e['fmt']), int(e['off']), int(e['goff'])) for e in f.elements[int(L['first']):int(L['first']) + int(L['count'])]]
    return int(L['stride']), els


def vertex_rows(cap, d, stride):
    n = int(d['vertex_count'])
    o = int(d['vertex_offset'])
    return cap.geo[o:o + n * stride].reshape(n, stride)


def positions_of(rows, n):
    return np.ascontiguousarray(rows[:, 0:12]).view('<f4').reshape(n, 3)


def near_equal(a, b, tol=1e-4):
    """Two f32 arrays (as bytes) that differ by no more than tol in value: the same vertex computed twice with different rounding."""
    x = np.ascontiguousarray(a).view('<f4').astype(np.float64)
    y = np.ascontiguousarray(b).view('<f4').astype(np.float64)
    return bool(np.isfinite(x).all() and np.isfinite(y).all() and np.abs(x - y).max() <= tol)


def fmt_name(fmt, goff):
    return {109: 'pos', 106: 'weights', 41: 'ids', 92: 'normal', 83: 'uv'}.get(fmt, 'u8x4_g%d' % goff)


# ---------------------------------------------------------------- the measuring ------------------------------------------------------------------------
def measure(cap, files, frames, plant):
    S = dict(stray=0, frames=0, named=0, versions=set(), cnt={}, npass={}, frames_with=collections.Counter(), order_viol=0, order_modes=collections.Counter(), order_viol_first=None,
             targets=collections.defaultdict(set), copies={}, copy_style=collections.defaultdict(collections.Counter),
             readers=collections.defaultdict(lambda: collections.defaultdict(list)), casters=collections.defaultdict(lambda: collections.defaultdict(dict)),
             caster_objs=collections.defaultdict(lambda: collections.defaultdict(dict)),
             chc=collections.Counter(), ringj=collections.Counter(), scope=collections.Counter(), eye=collections.defaultdict(collections.Counter),
             inst=collections.defaultdict(lambda: collections.defaultdict(list)), planec=collections.Counter(), planeinv=[], planeinv_bad=0, fade=set(), planepos=set(),
             layout=collections.Counter(), layout_draws=collections.Counter(), xd=collections.Counter(),
             light=collections.Counter(), light_vals={}, light_dir=[], light_cam_deg=[], circ=collections.Counter(), circ_err=0.0, bias=collections.Counter(), bias_err={},
             bias_sign=collections.Counter(), shprm=collections.defaultdict(set), pal=collections.Counter(), pal_f=collections.defaultdict(collections.Counter),
             pal_pts=collections.defaultdict(set), ty_dev=[], ring_join=collections.defaultdict(set), ring_join_draws=collections.Counter(), prov=collections.Counter(), roles=collections.defaultdict(collections.Counter), pslot=collections.Counter(), pslot_hit_slots=set(), pslot_ctrl_slots=set(),
             lamb=collections.Counter(), final_same=collections.Counter(), mat=collections.Counter(), seconds=0.0)
    geo_cache = {}
    t0 = time.time()
    for fi in frames:
        f = cap.frame(fi)
        S['frames'] += 1
        S['versions'].add(f.version)
        has_org = f.has_origins and plant != 'no_names'
        S['named'] += bool(has_org)
        if not has_org:
            continue
        do_frame(cap, files, f, fi, S, plant, geo_cache)
    S['seconds'] = time.time() - t0
    return S


def do_frame(cap, files, f, fi, S, plant, geo_cache):
    d = f.draws
    nd = len(d)
    o = f.origins
    ps, kl, ob = (o[k].astype(np.int64) for k in ('pass_site', 'klass', 'object'))
    cs, pt = pcap.queue_site(o), pcap.queue_part(o)   # S101 lane PN: generic queue sites, part cleared for op-68 records
    vs_eff, ps_eff = cap.effs(d['vs_hash']), cap.effs(d['ps_hash'])
    copies = table(f, SEC_COPIES, COPY_DT)
    provs = table(f, SEC_PROV, capread.PROV_DT)
    passes = walk(f, copies)
    imgs = cap.imgs

    def img(i):
        r = imgs.get(i)
        if r is None:
            raise CensusError('frame %d: image %d is not in images.bin' % (fi, i))
        return r

    # ---- classify the passes by the draws' origins (never by a shader hash alone)
    cnt = collections.Counter()
    npass = collections.Counter()
    kinds_seen = []
    for p in passes:
        if not p.draws:
            continue
        ix = np.array(p.draws)
        sites, calls, effs = ps[ix], cs[ix], ps_eff[ix]
        tr = img(p.target)
        kind = None
        if (sites == SITE_PLANE).any():
            kind = 'plane'
        elif (sites == SITE_MAP).any():
            kind = 'props' if int(tr['format']) == FMT_RGBA8 else ('mapA' if int(tr['gf']) == 4 else 'mapB' if int(tr['gf']) == 5 else 'map?')
        elif (sites == SITE_RIM).any():
            kind = 'rim'
        elif (calls == CALL_PBLUR).any():
            kind = 'plane_blur'
        elif (calls == CALL_DOWN).any():
            kind = 'plane_down'
        elif (calls == CALL_REDUCE).any():
            kind = 'reduce'
        elif ((calls == CALL_RIMRED) & (effs == 'yBlur/flag_any')).any():
            kind = 'rim_reduce'
        elif (calls == CALL_MINBLUR).any():
            e = effs[calls == CALL_MINBLUR][0]
            kind = {'yBlur/min_pair': 'min_pair', 'yBlur/min_rgba': 'min_rgba', 'yBlur/min_r': 'min_r', 'yBlur/min_rg': 'min_rg', 'yBlur/t2': 'rim_t2'}.get(e, 'min?')
        elif (calls == CALL_CIRCLE).any():
            kind = 'props'
        if plant == 'hash_only' and kind in ('rim', 'rim_t2') and int(tr['role']) in (3, 12):
            # the old rule: a surface tagged shadow_map is a shadow map. The rim light map and its blur are swallowed as shadow maps.
            kind = 'props' if int(tr['format']) == FMT_RGBA8 else 'mapA'
        p.kind = kind
        if kind is None:
            continue
        npass[kind] += 1
        kinds_seen.append(kind)
        if kind in ('props', 'mapA', 'mapB', 'plane'):
            # every draw of a shadow pass must carry a shadow origin (its pass site, or the circle call); anything else swallowed by the pass is a stray
            S['stray'] += int((~((sites == (SITE_PLANE if kind == 'plane' else SITE_MAP)) | (calls == CALL_CIRCLE))).sum())
            cnt[kind] += int(((sites == (SITE_PLANE if kind == 'plane' else SITE_MAP)) | ((plant == 'hash_only') & (sites == SITE_RIM))).sum())
        elif kind == 'rim':
            cnt[kind] += int((sites == SITE_RIM).sum())
        else:
            cnt[kind] += len(ix)
        S['targets'][kind].add('%dx%d f%d gf%d role%d' % (int(tr['w']), int(tr['h']), int(tr['format']), int(tr['gf']), int(tr['role'])))
        S['roles'][kind][ROLE_NAMES[int(tr['role'])] if int(tr['role']) < len(ROLE_NAMES) else '?'] += 1
    cnt['circles'] = int((cs == CALL_CIRCLE).sum())
    for k in set(list(S['cnt']) + list(cnt)):
        mm_update(S['cnt'], k, cnt.get(k, 0))
    for k in set(list(S['npass']) + list(npass)):
        mm_update(S['npass'], k, npass.get(k, 0))
    for k in set(cnt):
        if cnt[k]:
            S['frames_with'][k] += 1
    # ---- order: passes in op order never go backwards in the chain's own order
    ranks = [ORDER.index(k) for k in kinds_seen if k in ORDER]
    viol = any(b < a for a, b in zip(ranks, ranks[1:]))
    scene_pos = None
    first_colour = np.nonzero((ps == SITE_COLOUR) | (ps == SITE_MIRROR))[0]
    didx, dpos = f.draw_ops()
    pos_of = np.full(nd, -1, np.int64)
    pos_of[didx] = dpos
    if len(first_colour):
        scene_pos = int(pos_of[first_colour].min())
        last_chain = max((p.pos for p in passes if p.kind in ORDER and p.kind != 'rim_t2'), default=-1)
        viol = viol or (scene_pos < max((p.pos for p in passes if p.kind in ORDER), default=-1))
    S['order_viol'] += bool(viol)
    sig = []
    for k in kinds_seen:
        if k in ORDER and (not sig or sig[-1] != k):
            sig.append(k)
    S['order_modes']['>'.join(sig)] += 1
    # ---- copies
    dest_of = collections.defaultdict(set)
    src_of_dest = collections.defaultdict(set)
    for p in passes:
        if p.kind is None:
            continue
        mm_update(S['copies'], p.kind, len(p.copies))
        for ci in p.copies:
            c = copies[ci]
            dst, src = int(c['d']['image']), int(c['s']['image'])
            dest_of[p.kind].add(dst)
            src_of_dest[dst].add(src)
            r, st = c['sr'], img(src)
            S['copy_style'][p.kind]['whole' if list(r) == [0, 0, int(st['w']), int(st['h'])] else 'rect'] += 1
    # the final image of each chain (the readers' view of the chain)
    label_of = {}
    for kind, name in (('plane', 'plane_copy'), ('plane_down', 'plane_down_copy'), ('plane_blur', 'plane_mask'), ('props', 'props_map'), ('mapA', 'mapA_copy'), ('mapB', 'mapB_final'),
                       ('reduce', 'reduce_copy'), ('min_pair', 'min_pair_copy'), ('rim', 'rim_map'), ('rim_t2', 'rim_t2_map')):
        for i in dest_of.get(kind, ()):
            label_of[i] = name
    mb = dest_of.get('mapB', set())
    for k in ('min_rgba', 'min_r', 'min_rg'):
        if dest_of.get(k):
            S['final_same'][k + ('==mapB' if dest_of[k] == mb else '!=mapB')] += 1
            for i in dest_of[k]:
                label_of[i] = 'mapB_final' if dest_of[k] == mb else k + '_copy'
    # ---- readers: every draw's slots against the chain's images
    cnt_fam = collections.Counter()
    tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
    sl_img = f.slots['image'].astype(np.int64)
    sl_slot = f.slots['slot'].astype(np.int64)
    in_chain = np.isin(sl_img, np.array(sorted(label_of), dtype=np.int64)) if label_of else np.zeros(len(sl_img), bool)
    if in_chain.any():
        owner = np.repeat(np.arange(nd), tc)
        base = np.concatenate([np.arange(a, a + b) for a, b in zip(tf, tc)]) if nd else np.zeros(0, np.int64)
        if len(owner) == len(base):
            sel = np.nonzero(in_chain[base])[0]
            for j in sel:
                di, sj = int(owner[j]), int(base[j])
                e = vs_eff[di].split('/')[0]
                fam = 'wrestler' if e.startswith('yCh') else 'floor' if e.startswith('yBG') else 'ring_mat' if e.startswith('yProp_PSM') else 'blur' if e.startswith('yBlur') else e
                cnt_fam['%s|slot%d|%s' % (label_of[int(sl_img[sj])], int(sl_slot[sj]), fam)] += 1
        else:
            raise CensusError('frame %d: texture slots not laid out per draw' % fi)
    for k in set(S.setdefault('reader_mm', {}).keys()) | set(cnt_fam):
        mm_update(S['reader_mm'], k, cnt_fam.get(k, 0))
    # ---- provenance of the read images: a GPU copy of the source surface
    pv = {int(r['image']): r for r in provs}
    for i, name in label_of.items():
        r = pv.get(i)
        if r is None:
            what = 'absent'
        elif int(r['source']) != 2:
            what = 'not_gpu'
        elif int(r['source_image']) in src_of_dest[i]:
            what = 'gpu_copy_of_source'
        elif int(r['source_image']) == 0:
            what = 'gpu_source_image_unfilled'
        else:
            what = 'gpu_source_image_wrong'
        S['prov']['%s:%s' % (name, what)] += 1
    # ---- colour draws: object classes, palettes' twins
    colour_effs = collections.defaultdict(set)
    colour_idx = collections.defaultdict(list)
    ci = np.nonzero(((ps == SITE_COLOUR) | (ps == SITE_MIRROR)) & (kl == pcap.K_MODEL_PART))[0]
    for i in ci:
        colour_effs[int(ob[i])].add(vs_eff[i])
        colour_idx[(int(ob[i]), int(pt[i]))].append(int(i))
    cls_of = {k: class_of_effects(v) for k, v in colour_effs.items()}
    # ---- the casters
    plane_ix = [i for p in passes if p.kind == 'plane' for i in p.draws if ps[i] == SITE_PLANE]
    map_ix = {k: [i for p in passes if p.kind == k for i in p.draws if ps[i] == SITE_MAP] for k in MAP_KINDS}
    cls_draws = collections.defaultdict(lambda: collections.defaultdict(list))  # pass kind -> class -> draws
    cls_objs = collections.defaultdict(lambda: collections.defaultdict(set))
    cmap = {}
    # plane pass: joined to the files by content (never by the object address)
    inst = collections.defaultdict(list)
    for i in plane_ix:
        dr = d[i]
        stride, els = layout_of(f, dr)
        n, ic = int(dr['vertex_count']), int(dr['index_count'])
        rows = vertex_rows(cap, dr, stride)
        key = (int(dr['vertex_offset']), n, ic, stride)
        res = geo_cache.get(('plane',) + key)
        if res is None:
            pos = positions_of(rows, n)
            ch = [m for m in files.proxy_by.get((n, ic), []) if np.array_equal(m.positions, pos)]
            rg = [m for m in files.ring_by.get((n, ic), []) if np.array_equal(m.positions, pos)]
            xd = {}
            if len(els) == 4 and els[3][0] == 37 and rows.shape[1] >= els[3][1] + 4:
                x = rows[:, els[3][1]:els[3][1] + 4]
                px = rows[:, 0:4]
                xd['be_of_x'] = int((x == px[:, ::-1]).all(axis=1).sum())
                xd['zero_then_b123'] = int(((x[:, 0] == 0) & (x[:, 1:4] == px[:, 1:4]).all(axis=1)).sum())
                xd['n'] = n
                xd['kind'] = 'chcommon'
            elif len(els) == 4 and els[3][0] == 37:
                xd['n'] = n
                xd['kind'] = 'ring'
            if len(els) == 4 and files.ring956 is not None and not ch and rg and rows.shape[1] >= els[3][1] + 4:
                x = rows[:, els[3][1]:els[3][1] + 4]
                px = rows[:, 0:4]
                xd = {'be_of_x': int((x == px[:, ::-1]).all(axis=1).sum()), 'zero_then_b123': int(((x[:, 0] == 0) & (x[:, 1:4] == px[:, 1:4]).all(axis=1)).sum()), 'n': n, 'kind': 'ring'}
            res = geo_cache[('plane',) + key] = (sorted({m.index for m in ch}), sorted({m.index for m in rg}), xd, len(ch), len(rg))
        chi, rgi, xd, nch, nrg = res
        part = int(pt[i])
        shift = 1 if plant == 'proxy_part_shift' else 0
        if nch:
            S['chc']['draws'] += 1
            ok = chi == [(part + shift) % max(len(files.proxy), 1)]
            S['chc']['part_is_mesh' if ok else 'part_not_mesh'] += 1
            S['chc']['unique_mesh' if len(chi) == 1 else 'ambiguous'] += 1
            S['chc']['index_counts_equal'] += 1
            inst[int(ob[i])].append(part)
            cmap[i] = 'wrestler_proxy'
        elif nrg:
            S['ringj']['draws'] += 1
            S['ringj']['part_is_mesh' if rgi == [part] else 'part_not_mesh'] += 1
            S['ringj']['unique_mesh' if len(rgi) == 1 else 'ambiguous'] += 1
            cmap[i] = 'ring_proxy'
            inst[int(ob[i])].append(part)
        else:
            S['chc']['unjoined'] += 1
            cmap[i] = 'unjoined'
        if xd:
            for k2 in ('be_of_x', 'zero_then_b123', 'n'):
                S['xd']['%s:%s' % (xd['kind'], k2)] += xd.get(k2, 0)
        # the eye point, the plane, the fade (VS bank)
        vb = f.bank(int(dr['vs_bank']))
        eye = regs(vb, R_EYE)[0]
        S['eye'][cmap[i]]['(%.1f,%.1f,%.1f)' % tuple(np.round(eye[:3] + 0.0, 1) + 0.0)] += 1
        S['planepos'].add(round(float(regs(vb, R_PLANEPOS)[0][1]), 3))
    for objk, parts in inst.items():
        c_ = cmap[next(i for i in plane_ix if int(ob[i]) == objk)]
        n_parts = len(files.proxy) if c_ == 'wrestler_proxy' else len(files.ring956)
        # instances: the object's draws walk the parts 0..n-1 in order, repeated
        ok = n_parts and len(parts) % n_parts == 0 and parts == list(range(n_parts)) * (len(parts) // n_parts)
        S['inst'][c_]['n'].append(len(parts) // max(n_parts, 1))
        S['scope']['%s:instances_walk_parts_in_order' % c_ if ok else '%s:instances_out_of_order' % c_] += 1
        # drawn in no other pass
        other = set(int(x) for x in ps[ob == objk]) - {SITE_PLANE}
        S['scope']['%s:drawn_in_other_pass' % c_ if other else '%s:plane_pass_only' % c_] += 1
    for i in plane_ix:
        cls_draws['plane'][cmap[i]].append(i)
        cls_objs['plane'][cmap[i]].add(int(ob[i]))
    # plane pass constants: per frame
    if plane_ix:
        dr0 = d[plane_ix[0]]
        vb0 = f.bank(int(dr0['vs_bank']))
        cam = None
        if len(first_colour):
            wi = next((i for i in first_colour if vs_eff[i].startswith('yCh')), int(first_colour[0]))
            cam = regs(f.bank(int(d[wi]['vs_bank'])), R_VIEWPROJ, 4)
        camvp_all = set()
        pinv_all = set()
        for i in plane_ix:
            vb = f.bank(int(d[i]['vs_bank']))
            camvp_all.add(regs(vb, R_CAMVP, 4).tobytes())
            pinv_all.add(regs(vb, R_PLANEINV, 4).tobytes())
        S['planec']['frames'] += 1
        S['planec']['one_camvp' if len(camvp_all) == 1 else 'many_camvp'] += 1
        S['planec']['one_planeinv' if len(pinv_all) == 1 else 'many_planeinv'] += 1
        if cam is not None:
            S['planec']['camvp_eq_scene_bits' if regs(vb0, R_CAMVP, 4).tobytes() == cam.tobytes() else 'camvp_ne_scene'] += 1
        pi = regs(vb0, R_PLANEINV, 4).astype(np.float64)
        diag_ok = (pi[0, 1] == 0 and pi[0, 2] == 0 and pi[1, 0] == 0 and pi[1, 2] == 0 and np.array_equal(pi[2], [0, 0, 1, 0]) and np.array_equal(pi[3], [0, 0, 0, 1]))
        if pi[0, 0] > 0:
            S['planeinv'].append((float(pi[0, 0]), float(pi[1, 1]), float(pi[0, 3]), float(pi[1, 3])))
        S['planec']['planeinv_shape_ok' if diag_ok else 'planeinv_shape_bad'] += 1
        if pi[0, 0] > 0:
            S['planec']['planeinv_fit_form'] += 1
            S['ty_dev'].append(abs(float(pi[1, 3] - (pi[1, 1] - 1))))
        else:
            S['planec']['planeinv_negated_identity' if np.array_equal(pi[:2, :], [[-1, 0, 0, 0], [0, -1, 0, 0]]) else 'planeinv_form_other'] += 1
        fade = regs(vb0, R_FADE0, 7).astype(np.float64)
        S['fade'].add(tuple(np.round(fade[0], 4)) + tuple(np.round(fade[1:4, 3], 4)))
        S['planepos'].add(round(float(regs(vb0, R_PLANEPOS)[0][1]), 3))
        # the ring mat reads the same matrices
        for i in np.nonzero(ps == SITE_COLOUR)[0]:
            if ps_eff[i] == 'yProp_PSMLM/screen_shadow':
                vb = f.bank(int(d[i]['vs_bank']))
                S['mat']['camvp_eq_plane' if regs(vb, R_MAT_CAMVP, 4).tobytes() == regs(vb0, R_CAMVP, 4).tobytes() else 'camvp_ne_plane'] += 1
                S['mat']['planeinv_eq_plane' if regs(vb, R_MAT_PLANEINV, 4).tobytes() == regs(vb0, R_PLANEINV, 4).tobytes() else 'planeinv_ne_plane'] += 1
    # map passes: classes
    for k in MAP_KINDS:
        for i in map_ix[k]:
            cl = cls_of.get(int(ob[i]), 'no_colour_twin')
            cmap[i] = cl
            cls_draws[k][cl].append(i)
            cls_objs[k][cl].add(int(ob[i]))
    for kk in list(cls_draws) + [k for k in MAP_KINDS + ('plane',) if k not in cls_draws]:
        byc = cls_draws.get(kk, {})
        for cl in set(list(byc) + list(S.setdefault('caster_seen', {}).get(kk, []))):
            S['caster_seen'].setdefault(kk, set()).add(cl)
        for cl in S['caster_seen'].get(kk, ()):
            mm_update(S['caster_objs'][kk], cl + '.draws', len(byc.get(cl, [])))
            mm_update(S['caster_objs'][kk], cl + '.objects', len(cls_objs[kk].get(cl, ())))
    # ---- shadow vertex layouts against the colour draw of the same (object, part); palettes
    for k in MAP_KINDS:
        for i in map_ix[k]:
            cl = cmap[i]
            dr = d[i]
            key2 = (int(ob[i]), int(pt[i]))
            ctw = colour_idx.get(key2)
            stride, els = layout_of(f, dr)
            n = int(dr['vertex_count'])
            rows = vertex_rows(cap, dr, stride)
            ids_off = next((off for fm, off, g in els if fm == 41), None)
            if ctw:
                cdr = d[ctw[0]]
                cstride, cels = layout_of(f, cdr)
                gk = ('lay', int(dr['vertex_offset']), int(cdr['vertex_offset']), stride, cstride, n)
                res = geo_cache.get(gk)
                if res is None:
                    crow = vertex_rows(cap, cdr, cstride)
                    res = []
                    for fm, off, goff in els:
                        ce = next((e for e in cels if e[0] == fm and e[1] == off and e[2] == goff), None)
                        sz = ELEM_SIZE.get(fm, 4)
                        if ce is None or int(cdr['vertex_count']) != n:
                            res.append((fmt_name(fm, goff), 'absent'))
                        else:
                            a_, b_ = rows[:, off:off + sz], crow[:, ce[1]:ce[1] + sz]
                            if np.array_equal(a_, b_):
                                st_ = 'equal'
                            elif fm == 109 and near_equal(a_, b_):
                                st_ = 'within_1e-4'
                            else:
                                st_ = 'differs'
                            res.append((fmt_name(fm, goff), st_))
                    geo_cache[gk] = res
                shape = 'skinned' if any(fm == 106 for fm, _, _ in els) else 'rigid'
                S['layout_draws']['%s.%s' % (cl, shape)] += 1
                for nm, st in res:
                    S['layout']['%s.%s.%s.%s' % (cl, shape, nm, st)] += 1
                # palette rows of the used bones against the colour draws of the same (object, part)
                if ids_off is not None and cl in ('wrestler', 'piece', 'prop'):
                    pk = ('ids', int(dr['vertex_offset']), n, stride)
                    used = geo_cache.get(pk)
                    if used is None:
                        ids = rows[:, ids_off:ids_off + 4].astype(np.int64)
                        used = geo_cache[pk] = sorted(int(x) for x in np.unique(ids[ids < 21]))
                    sb = f.bank(int(dr['vs_bank']))

                    def bone(bank, b_):
                        return bank[4 * (R_WORLD + 3 * b_):4 * (R_WORLD + 3 * b_ + 3)]
                    twins = [f.bank(int(d[j]['vs_bank'])) for j in ctw]
                    same = any(all(bone(tb, b_).tobytes() == bone(sb, b_).tobytes() for b_ in used) for tb in twins)
                    key_ = '%s.%s' % (cl, k)
                    S['pal']['%s.draws' % key_] += 1
                    S['pal_f'][fi]['%s.draws' % key_] += 1
                    if not same:
                        S['pal']['%s.differs' % key_] += 1
                        S['pal_f'][fi]['%s.differs' % key_] += 1
                        # collapsed: every differing bone has an all-zero 3x3 rotation part; the translations left are one shared point?
                        tb = twins[0]
                        coll, pts = True, set()
                        if any(bone(tb, b_).tobytes() == bone(sb, b_).tobytes() for b_ in used):
                            S['pal']['%s.differs_with_some_bones_kept' % key_] += 1
                        for b_ in used:
                            sr = bone(sb, b_).view('<f4').reshape(3, 4)
                            tr_ = bone(tb, b_).view('<f4').reshape(3, 4)
                            if not np.array_equal(sr, tr_):
                                if (sr[:, :3] == 0).all():
                                    pts.add(sr[:, 3].tobytes())
                                else:
                                    coll = False
                        S['pal']['%s.differs_all_bones_rotation_zero' % key_ if coll else '%s.differs_other' % key_] += 1
                        if coll:
                            S['pal']['%s.collapsed_to_one_point' % key_ if len(pts) == 1 else '%s.collapsed_to_several_points' % key_] += 1
                            S['pal_pts'][fi].update(pts)
            else:
                S['layout_draws']['%s.no_colour_twin' % cl] += 1
    # ---- lights: one per frame per chain; the circle; the readers' bias
    light = {}
    for k in MAP_KINDS:
        if not map_ix[k]:
            continue
        vals = set()
        for i in map_ix[k]:
            vb, pb = f.bank(int(d[i]['vs_bank'])), f.bank(int(d[i]['ps_bank']))
            vals.add(vb[4 * R_VIEWPROJ:4 * (R_VIEWPROJ + 4)].tobytes() + vb[4 * R_TRAP:4 * (R_TRAP + 4)].tobytes() + vb[4 * R_VIEWDEPTH:4 * (R_VIEWDEPTH + 1)].tobytes() + pb[4 * R_SHADOWPRM:4 * R_SHADOWPRM + 4].tobytes())
        S['light']['%s.frames' % k] += 1
        S['light']['%s.distinct%s' % (k, 1 if len(vals) == 1 else '>1')] += 1
        i0 = map_ix[k][0]
        vb, pb = f.bank(int(d[i0]['vs_bank'])), f.bank(int(d[i0]['ps_bank']))
        VP, TR, VD = regs(vb, R_VIEWPROJ, 4), regs(vb, R_TRAP, 4), regs(vb, R_VIEWDEPTH)[0]
        light[k] = (VP, TR, VD, regs(pb, R_SHADOWPRM)[0])
        S['light']['%s.viewdepth_eq_vp_row3_bits' % k if VD.tobytes() == VP[3].tobytes() else '%s.viewdepth_ne_vp_row3' % k] += 1
        S['light']['%s.trap_rows23_identity' % k if np.array_equal(TR[2], [0, 0, 1, 0]) and np.array_equal(TR[3], [0, 0, 0, 1]) else '%s.trap_rows23_other' % k] += 1
        S['light']['%s.trap_off_diag_zero' % k if TR[0, 1] == 0 and TR[1, 0] == 0 and TR[0, 2] == 0 and TR[1, 2] == 0 else '%s.trap_off_diag' % k] += 1
        S['shprm'][k].add(tuple(float(np.float32(x)) for x in np.round(light[k][3], 2)[2:]))
    if 'mapA' in light and 'mapB' in light:
        S['light']['A_B_same_light_bits' if light['mapA'][0].tobytes() + light['mapA'][1].tobytes() == light['mapB'][0].tobytes() + light['mapB'][1].tobytes() else 'A_B_light_differs'] += 1
    wk = 'mapB' if 'mapB' in light else 'mapA' if 'mapA' in light else None
    if wk:
        VP = light[wk][0]
        row3 = VP[3]
        dirv = row3[:3] / np.linalg.norm(row3[:3])
        S['light_dir'].append((float(np.linalg.norm(row3[:3])), float(dirv[1])))
        if len(first_colour):
            wi = next((i for i in first_colour if vs_eff[i].startswith('yCh')), int(first_colour[0]))
            camfwd = regs(f.bank(int(d[wi]['vs_bank'])), R_VIEWPROJ, 4)[3][:3].astype(np.float64)
            a1 = np.degrees(np.arctan2(dirv[0], dirv[2]))
            a2 = np.degrees(np.arctan2(camfwd[0], camfwd[2]))
            dd = (a1 - a2 + 180) % 360 - 180
            S['light_cam_deg'].append(float(abs(dd)))
    if 'props' in light:
        VP, TR = light['props'][0].astype(np.float64), light['props'][1].astype(np.float64)
        S['light']['props.vp_row3_up' if abs(VP[3, 0]) < 1e-3 and abs(VP[3, 1] - 1) < 1e-6 and abs(VP[3, 2]) < 1e-3 else 'props.vp_row3_not_up'] += 1
        for i in np.nonzero(cs == CALL_CIRCLE)[0]:
            vb = f.bank(int(d[i]['vs_bank']))
            c4 = regs(vb, R_CIRCLE_VP, 4).astype(np.float64)
            tv = TR @ VP
            err = float(np.abs(c4 - tv).max() / max(np.abs(tv).max(), 1e-9))
            S['circ_err'] = max(S['circ_err'], err)
            S['circ']['circle_vp_eq_trap_x_vp' if err < 2e-6 else 'circle_vp_ne'] += 1
            S['circ']['circle_vp_bits_equal' if np.array_equal(c4.astype(np.float32), tv.astype(np.float32)) else 'circle_vp_not_bit_equal'] += 1
            S['circ']['circle_c0_x_is_1' if regs(vb, R_CIRCLE_PRM)[0][0] == 1 else 'circle_c0_x_not_1'] += 1
    # the readers' projection: TexProjLight = 0.5 x (Trapezoidal x ViewProj) rows 0/1 (+/- row 1) + (0.5 + 0.5/512) x row 3
    for i in ci:
        e = vs_eff[i].split('/')[0]
        if e.startswith('yCh_') or e.startswith('yBG_') or e.startswith('yProp_PSM'):
            sl = [int(s['image']) for s in f.slots[int(tf[i]):int(tf[i]) + int(tc[i])] if int(s['slot']) == 12]
            if not sl or sl[0] not in label_of:
                continue
            lab = label_of[sl[0]]
            if lab == 'mapB_final' and 'mapB' in light:
                src = light['mapB']
            elif lab == 'props_map' and 'props' in light:
                src = light['props']
            else:
                continue
            VP, TR = src[0].astype(np.float64), src[1].astype(np.float64)
            tv = TR @ VP
            b = 0.5 + 0.5 / 512
            vb = f.bank(int(d[i]['vs_bank']))
            got = regs(vb, R_WTEXXY, 4).astype(np.float64)
            exp0 = 0.5 * tv[0] + b * tv[3]
            exp1p = 0.5 * tv[1] + b * tv[3]
            exp1m = -0.5 * tv[1] + (1.0 - b) * tv[3]   # the y flip takes the half texel with it
            scale = np.abs(tv).max()
            e0 = float(np.abs(got[0] - exp0).max() / scale)
            e1p = float(np.abs(got[1] - exp1p).max() / scale)
            e1m = float(np.abs(got[1] - exp1m).max() / scale)
            e2 = float(np.abs(got[2] - tv[2]).max() / scale)
            e3 = float(np.abs(got[3] - tv[3]).max() / scale)
            kk = '%s.%s' % (lab, 'wrestler' if e.startswith('yCh') else 'floor' if e.startswith('yBG') else 'ring_mat')
            S['bias_err'][kk] = max(S['bias_err'].get(kk, 0.0), e0, min(e1p, e1m), e2, e3)
            S['bias']['%s.row0_ok' % kk if e0 < 2e-5 else '%s.row0_off' % kk] += 1
            S['bias']['%s.row1_flipped_ok' % kk if e1m < 2e-5 else ('%s.row1_plain_ok' % kk if e1p < 2e-5 else '%s.row1_off' % kk)] += 1
            S['bias']['%s.rows23_equal_light' % kk if e2 < 2e-6 and e3 < 2e-6 else '%s.rows23_off' % kk] += 1
            if lab == 'mapB_final':
                S['bias']['wrestler_c166_eq_light_row3' if regs(vb, R_WLIGHTDEPTH)[0].tobytes() == src[0][3].tobytes() else 'wrestler_c166_ne_light_row3'] += 1
    # ---- roles of the images whose copies the readers see: the rim map is tagged like the shadow maps
    # ---- stage slots and `p`: no stage slot casts into any dynamic map; control: the colour pass does draw them
    if files.a is not None:
        map_all = [i for k in MAP_KINDS for i in map_ix[k]]
        for tag, ixs in (('cast', plane_ix + map_all), ('ctrl', [int(i) for i in ci[:: max(1, len(ci) // 400)]])):
            for i in ixs:
                dr = d[i]
                n, ic = int(dr['vertex_count']), int(dr['index_count'])
                cands = files.stage.get((n, ic))
                rcands = files.ring_all.get((n, ic))
                if not cands and not rcands:
                    continue
                stride, els = layout_of(f, dr)
                gk = ('stage', int(dr['vertex_offset']), n, ic, stride)
                hit = geo_cache.get(gk)
                if hit is None:
                    pos = positions_of(vertex_rows(cap, dr, stride), n)
                    hit = geo_cache[gk] = (sorted({s_ for s_, m in (cands or []) if np.array_equal(m.positions, pos)}),
                                           sorted({m.entry for m in (rcands or []) if np.array_equal(m.positions, pos)}))
                for s_ in hit[0]:
                    S['pslot']['%s.draws_joined' % tag] += 1
                    (S['pslot_hit_slots'] if tag == 'cast' else S['pslot_ctrl_slots']).add(s_)
                for e_ in hit[1]:
                    where = 'plane' if tag == 'cast' and i in set(plane_ix) else ('maps' if tag == 'cast' else 'colour')
                    S['ring_join'][where].add(e_)
                    S['ring_join_draws'][where + ':%d' % e_] += 1
    # ---- 956 outside the shadow passes: wm30's 24 draws in the scene pass (T_Lambert range), not a shadow
    if files.ring956:
        lam = np.nonzero((ps >= LAMBERT[0]) & (ps <= LAMBERT[1]))[0]
        before = S['lamb'].get('joined', 0)
        for i in lam:
            dr = d[i]
            n, ic = int(dr['vertex_count']), int(dr['index_count'])
            if (n, ic) in files.ring_by:
                stride, els = layout_of(f, dr)
                gk = ('lamb', int(dr['vertex_offset']), n, ic, stride)
                j = geo_cache.get(gk)
                if j is None:
                    pos = positions_of(vertex_rows(cap, dr, stride), n)
                    j = geo_cache[gk] = any(np.array_equal(m.positions, pos) for m in files.ring_by[(n, ic)])
                S['lamb']['joined' if j else 'unjoined'] += 1
        if S['lamb'].get('joined', 0) > before:
            S['lamb']['frames_with_joined'] += 1


# ---------------------------------------------------------------- the checks ---------------------------------------------------------------------------
class Checks:
    def __init__(self, quiet):
        self.rows, self.quiet = [], quiet

    def add(self, label, name, ok, detail):
        self.rows.append((label, name, bool(ok), detail))
        if not self.quiet or not ok:
            print('%s  %-8s %-14s %s' % ('PASS' if ok else 'FAIL', label, name, detail))


def plain(x):
    if isinstance(x, dict):
        return {str(k): plain(v) for k, v in sorted(x.items(), key=lambda kv: str(kv[0]))}
    if isinstance(x, (set, frozenset)):
        return sorted(plain(v) for v in x)
    if isinstance(x, (list, tuple)):
        return [plain(v) for v in x]
    if isinstance(x, np.generic):
        return x.item()
    if isinstance(x, collections.Counter):
        return {str(k): v for k, v in sorted(x.items(), key=lambda kv: str(kv[0]))}
    return x


def summarize(S, files):
    """The measured numbers, in the form expect.json holds (counts, ids and names only)."""
    N = S['frames']
    n = {}
    n['frames'] = N
    n['named'] = S['named']
    n['pass_draws'] = {k: v for k, v in sorted(S['cnt'].items())}
    n['pass_count'] = {k: v for k, v in sorted(S['npass'].items())}
    n['frames_with'] = dict(sorted(S['frames_with'].items()))
    n['order_violations'] = S['order_viol']
    n['order_mode'] = S['order_modes'].most_common(1)[0][0] if S['order_modes'] else ''
    n['order_modes'] = len(S['order_modes'])
    n['targets'] = {k: sorted(v) for k, v in sorted(S['targets'].items())}
    n['copies_per_pass_kind'] = {k: v for k, v in sorted(S['copies'].items())}
    n['copy_style'] = {k: dict(sorted(v.items())) for k, v in sorted(S['copy_style'].items())}
    n['final_same'] = dict(sorted(S['final_same'].items()))
    n['readers'] = {k: v for k, v in sorted(S.get('reader_mm', {}).items())}
    n['casters'] = {k: {c: v for c, v in sorted(d.items())} for k, d in sorted(S['caster_objs'].items())}
    n['chcommon'] = dict(sorted(S['chc'].items()))
    n['ring956'] = dict(sorted(S['ringj'].items()))
    n['scope'] = dict(sorted(S['scope'].items()))
    n['instances'] = {c: [min(v['n']), max(v['n'])] for c, v in sorted(S['inst'].items()) if v['n']}
    n['eyepos'] = {c: dict(sorted(v.items())) for c, v in sorted(S['eye'].items())}
    n['planepos_y'] = sorted(S['planepos'])
    n['plane_consts'] = dict(sorted(S['planec'].items()))
    n['mat_reader'] = dict(sorted(S['mat'].items()))
    n['fade'] = sorted(map(list, S['fade']))
    if S['planeinv']:
        a = np.array(S['planeinv'])
        n['planeinv_range'] = {'sx': [round(float(a[:, 0].min()), 3), round(float(a[:, 0].max()), 3)], 'sy': [round(float(a[:, 1].min()), 3), round(float(a[:, 1].max()), 3)]}
    n['layout_draws'] = dict(sorted(S['layout_draws'].items()))
    n['layout'] = dict(sorted(S['layout'].items()))
    n['xdword'] = dict(sorted(S['xd'].items()))
    n['light'] = dict(sorted(S['light'].items()))
    n['shadow_prm_zw'] = {k: sorted(v) for k, v in sorted(S['shprm'].items())}
    if S['light_dir']:
        a = np.array(S['light_dir'])
        n['light_dir'] = {'unit_len': [round(float(a[:, 0].min()), 3), round(float(a[:, 0].max()), 3)], 'y': [round(float(a[:, 1].min()), 5), round(float(a[:, 1].max()), 5)]}
    if S['light_cam_deg']:
        a = np.array(S['light_cam_deg'])
        n['light_vs_camera_row3_deg'] = {'max': round(float(a.max()), 2), 'mean': round(float(a.mean()), 2)}
    n['circle'] = dict(sorted(S['circ'].items()))
    n['circle_err'] = float('%.1e' % S['circ_err'])
    n['bias'] = dict(sorted(S['bias'].items()))
    n['bias_err_max'] = {k: float('%.1e' % v) for k, v in sorted(S['bias_err'].items())}
    n['palettes'] = dict(sorted(S['pal'].items()))
    f100 = collections.Counter()
    for fr in (100, 200):
        for k_, v_ in S['pal_f'].get(fr, {}).items():
            if k_.startswith('wrestler.') and k_.endswith(('.draws', '.differs')):
                f100['wrestler.' + k_.rsplit('.', 1)[1]] += v_
    n['palette_frames_100_200'] = dict(f100)
    if S['pal_pts']:
        n['collapse_points_per_frame'] = [min(len(v) for v in S['pal_pts'].values()), max(len(v) for v in S['pal_pts'].values())]
    n['provenance'] = dict(sorted(S['prov'].items()))
    n['roles'] = {k: dict(sorted(v.items())) for k, v in sorted(S['roles'].items())}
    n['ring_pieces_joined'] = {k: sorted(v) for k, v in sorted(S['ring_join'].items())}
    n['ring_pieces_joined_draws'] = dict(sorted(S['ring_join_draws'].items()))
    n['p_slots'] = {'p_slots_in_pac': len(files.p_slots), 'cast_joined_draws': S['pslot'].get('cast.draws_joined', 0), 'cast_slots': sorted(S['pslot_hit_slots']),
                    'control_slots_joined': len(S['pslot_ctrl_slots']), 'control_p_slots_joined': len(S['pslot_ctrl_slots'] & set(files.p_slots))}
    n['lambert956'] = dict(sorted(S['lamb'].items()))
    return n


def evaluate(label, S, E, files, ck, partial, plant):
    N = S['frames']
    n = summarize(S, files)
    # ---- intrinsic checks (rules the map states; no expect.json needed)
    ck.add(label, 'names', S['named'] == N and N > 0, '%d of %d frames carry draw origins' % (S['named'], N))
    if S['named'] != N:
        return n  # nothing else can be read without names
    ck.add(label, 'order', S['order_viol'] == 0, '%d frames with a pass out of the chain order; modal order %s' % (S['order_viol'], n['order_mode']))
    ck.add(label, 'passes', S['stray'] == 0 and (all(k in S['cnt'] for k in ('mapB', 'props')) or 'mapA' in S['cnt']), '%d stray draws in the shadow passes (no shadow origin); draws a frame %s' % (S['stray'], ', '.join('%s %d-%d' % (k, v[0], v[1]) for k, v in sorted(S['cnt'].items()))))
    rim_ok = S['cnt'].get('rim', [0, 0])[1] > 0
    rim_role = set(n['roles'].get('rim', {}))
    ck.add(label, 'roles', rim_ok and bool(rim_role) and rim_role <= {'shadow_map', 'rim_map'}, 'rim light map surface roles %s (shadow_map in captures before lane 86: the naming fault; rim_map after)' % sorted(rim_role))
    chc, rg = S['chc'], S['ringj']
    if S['frames_with'].get('plane'):
        ck.add(label, 'chcommon_join', chc['draws'] > 0 and chc['part_is_mesh'] == chc['draws'] and chc['unique_mesh'] == chc['draws'] and not chc['unjoined'] and not chc['part_not_mesh'],
               '%d wrestler-proxy draws: part = mesh %d, unique mesh %d, unjoined %d (positions bit-equal, index counts equal)' % (chc['draws'], chc['part_is_mesh'], chc['unique_mesh'], chc['unjoined']))
        ck.add(label, 'ring_join', rg['draws'] > 0 and rg['part_is_mesh'] == rg['draws'] and rg['unique_mesh'] == rg['draws'],
               '%d ring-proxy draws: part = mesh %d, unique mesh %d' % (rg['draws'], rg['part_is_mesh'], rg['unique_mesh']))
        sc = S['scope']
        bad = [k for k in sc if 'out_of_order' in k or 'drawn_in_other_pass' in k]
        ck.add(label, 'proxy_scope', not bad, 'proxy objects draw in the plane pass only and walk their parts in order: %s' % dict(sc))
        pc = S['planec']
        ck.add(label, 'plane_consts', not pc.get('camvp_ne_scene') and not pc.get('many_camvp') and not pc.get('many_planeinv') and not pc.get('planeinv_shape_bad') and not pc.get('planeinv_form_other')
               and S['planepos'] == {-120.0}, 'plane pass: CameraViewProj = the scene draws bit-equal, one CameraViewProj and PlaneInv a frame, PlaneInv shape ok, PlanePos.y %s: %s' % (sorted(S['planepos']), dict(pc)))
        if S['mat']:
            ck.add(label, 'mat_matrices', not S['mat'].get('camvp_ne_plane') and not S['mat'].get('planeinv_ne_plane'), 'ring mat reads the plane pass matrices bit-equal: %s' % dict(S['mat']))
    else:
        ck.add(label, 'chcommon_join', not chc and not rg, 'no plane pass in this capture; no proxy draws joined')
    lay = S['layout']
    bad_lay = {k: v for k, v in lay.items() if k.endswith('.differs') and not (k.startswith('prop.') and '.u8x4_g' in k)} | {k: v for k, v in lay.items() if k.endswith('.absent')}  # within_1e-4 is counted, pinned in expect
    ck.add(label, 'layouts', not bad_lay and sum(S['layout_draws'].values()) > 0, 'shadow vertex elements against the colour draw of the same (object, part): %d draws; elements that differ or are missing outside the props\' u8x4: %s' % (sum(S['layout_draws'].values()), bad_lay or 'none'))
    L = S['light']
    ones = all(L.get('%s.distinct>1' % k, 0) == 0 for k in MAP_KINDS)
    ck.add(label, 'lights', ones and not any(k.endswith(('_ne_vp_row3', 'ne_vp_row3')) or 'row3' in k and 'ne' in k for k in L) and not L.get('A_B_light_differs'), 'one light per frame per map pass, ViewDepth = ViewProj row 3 bit-equal, A and B share the light: %s' % {k: v for k, v in L.items() if not k.endswith('.frames')})
    ck.add(label, 'circle', S['circ'].get('circle_vp_ne', 0) == 0, 'circle ViewProj c4 = Trapezoidal x ViewProj to %.1e relative; %s' % (S['circ_err'], dict(S['circ'])))
    bad_bias = {k: v for k, v in S['bias'].items() if k.endswith(('_off', '_ne_light_row3')) or k.endswith('row1_plain_ok')}
    ck.add(label, 'bias', not bad_bias and bool(S['bias']), 'readers\' TexProjLight = 0.5 x (Trapezoidal x ViewProj) row 0 + (0.5 + 0.5/512) x row 3, -0.5 x row 1 + (0.5 - 0.5/512) x row 3, rows 2 / 3 = the light\'s (max error %s): %s' % (n['bias_err_max'], dict(S['bias'])))
    ck.add(label, 'provenance', S['prov'] and all(k.endswith(('gpu_copy_of_source', 'gpu_source_image_unfilled')) for k in S['prov']), 'every read image is declared a GPU copy (source_image names the source surface where the capture fills it): %s' % dict(S['prov']))
    ps_ = n['p_slots']
    rj = n['ring_pieces_joined']
    ck.add(label, 'p_slots', ps_['cast_joined_draws'] == 0 and ps_['control_slots_joined'] > 0 and ps_['control_p_slots_joined'] > 0 and not rj.get('maps') and rj.get('plane', [956]) == [956] and len(rj.get('colour', [])) > 1,
           '%d stage slots flagged p in the pac; stage-slot meshes joined to shadow-pass casters: %d (control: %d slots join colour-pass draws, %d of them p slots); ring entries joined: in the maps %s, in the plane pass %s, in the colour pass %d entries' % (ps_['p_slots_in_pac'], ps_['cast_joined_draws'], ps_['control_slots_joined'], ps_['control_p_slots_joined'], rj.get('maps', []), rj.get('plane', []), len(rj.get('colour', []))))
    lb = S['lamb']
    ck.add(label, 'lambert', not lb.get('unjoined'), 'ring entry 956 geometry in the T_Lambert range (the scene pass, not a shadow): %s' % (dict(lb) or 'none'))
    # ---- expect.json
    if partial:
        return n
    if not E:
        ck.add(label, 'expect', False, 'no expect.json entry for this capture (pass --frames for a partial look)')
        return n
    groups = {
        'passes': ['frames', 'pass_draws', 'pass_count', 'frames_with', 'order_mode', 'order_modes'],
        'targets': ['targets', 'roles'],
        'copies': ['copies_per_pass_kind', 'copy_style', 'final_same'],
        'readers': ['readers', 'provenance'],
        'casters': ['casters', 'instances', 'scope'],
        'chcommon_join': ['chcommon', 'ring956', 'xdword'],
        'eyepos': ['eyepos', 'planepos_y', 'fade', 'planeinv_range'],
        'layouts': ['layout_draws', 'layout'],
        'lights': ['light', 'shadow_prm_zw', 'light_dir', 'circle'],
        'bias': ['bias', 'bias_err_max'],
        'palettes': ['palettes', 'palette_frames_100_200', 'collapse_points_per_frame'],
        'p_slots': ['p_slots', 'lambert956', 'ring_pieces_joined', 'ring_pieces_joined_draws'],
    }
    for gname, keys in groups.items():
        diffs = []
        for k in keys:
            if fold_rim(k, plain(E.get(k))) != fold_rim(k, plain(n.get(k))):  # S101 lane 86: rim_map == shadow_map in the expectation (old and new captures)
                diffs.append('%s: expect %s, measured %s' % (k, json.dumps(plain(E.get(k)), sort_keys=True)[:300], json.dumps(plain(n.get(k)), sort_keys=True)[:300]))
        ck.add(label, 'expect.' + gname, not diffs, 'matches expect.json' if not diffs else '; '.join(diffs))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--expect', default=os.path.join(HERE, 'expect.json'))
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--emit-expect', help='write what was measured in expect.json form (a draft, to be read against the map)')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    expect = json.load(open(a.expect, encoding='utf-8')) if os.path.exists(a.expect) else {}
    ck = Checks(a.q)
    bad_input, emitted, report = False, {}, {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        E = expect.get(label, {})
        arena = E.get('arena')
        if arena is None:
            print('FAIL %-8s input         no arena for this label in %s (the entry names the arena whose files are joined)' % (label, a.expect))
            bad_input = True
            continue
        t0 = time.time()
        try:
            cap = Cap(path)
            files = FileSide(arena)
            if a.frames:
                lo, hi, step = (int(x) if x else None for x in (a.frames.split(':') + ['', ''])[:3])
                frames = list(range(lo or 0, hi if hi is not None else len(cap.frames), step or 1))
            else:
                frames = list(range(len(cap.frames)))
            S = measure(cap, files, frames, a.plant)
        except (pcap.PError, CensusError, af.FileReadError, OSError, KeyError, IndexError) as e:
            print('FAIL %-8s input         cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        obs = evaluate(label, S, E, files, ck, bool(a.frames), a.plant)
        emitted[label] = dict(arena=arena, **obs)
        report[label] = dict(numbers=obs, seconds=round(time.time() - t0, 1))
        print('     %-8s read %d frames in %.1f s' % (label, S['frames'], time.time() - t0))
    if a.emit_expect:
        json.dump(plain(emitted), open(a.emit_expect, 'w', encoding='utf-8'), indent=1, sort_keys=True)
    if a.json:
        json.dump(plain(report), open(a.json, 'w'), indent=1, default=str)
    fails = [r for r in ck.rows if not r[2]]
    print('%d checks, %d FAIL%s' % (len(ck.rows), len(fails), '' if not a.plant else '  (plant: %s)' % a.plant))
    return 1 if fails or bad_input else 0


if __name__ == '__main__':
    sys.exit(main())
