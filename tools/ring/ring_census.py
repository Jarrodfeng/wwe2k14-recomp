r"""Lane RJ (S101, docs\renderer\RING_PROMOTION_MAP.md section 7): the ring join census. Offline, 0 game runs. Mirrors tools\arena\join_census.py (lane 39).

Re-measures, with a committed tool, every [C] of the map's sections 1-2 on case zips / capture folders, against the ring read straight from the player's own files
(tools\ring\ring_files.py): which stream draws are ring draws, the join object -> (ring entry, mesh = part) by content intersected over the object's parts with the
texture bytes breaking the last tie, instances per piece, the rope LOD, the passes the pieces draw into, and the host vertex rules (position, skin ids, weights, the
fill word, normal, colour byte order, uv with the |u| defect, indices) byte for byte.

  py -I tools\ring\ring_census.py CAPTURE[:ARENA] [...] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--expect FILE] [-q]
  py -I tools\ring\ring_census.py --files [--plant NAME] [--expect FILE] [-q]       (the 57 bgNN.pac only, no capture)
CAPTURE = a case zip (capture\ inside it) or a capture folder. ARENA = the bg id (default: found by the join rate). Exit 1 when any check FAILs or an input cannot be read.
Capture checks: names, content_equal, part_is_mesh, position_element, index_content, join_unique, stability, sites, mirror, instances, lod, proxy, skin, fill, weights,
normal, colour_order, uv, texture_bytes, skirt_tie.   Files checks: files_read, files_ring_set, files_contents, files_skin, files_neg_u.
--plant (tests only): join_shift, colour_order, skirt_tie, lod_swap, part_only_join, lod_pin, no_names, uv_sign (capture mode); files_set, files_skin_bad (files mode).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
import hashlib
import itertools
import json
import os
import re
import struct
import sys
import time
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import arena_files as af  # noqa: E402
import join_census as jc  # noqa: E402  (the capture reader, compare_texture, the draw element readers)
import ring_files as rf  # noqa: E402

SITE_COLOUR, SITE_MIRROR, SITE_SHADOW, SITE_PLANE = 0x82C0A950, 0x82C0B94C, 0x82C04634, 0x82BF4D38
SITE_TLAMBERT = (0x82BF5C58, 0x82BF8118)
PIECE = {951: 'post', 981: 'post', 952: 'turnbuckle', 953: 'canvas', 956: 'proxy', 960: 'rope_high', 980: 'rope_low', 964: 'cover', 982: 'cover', 973: 'skirt', 974: 'skirt'}
COLOUR_RULE = (1, 2, 3, 0)  # stream byte k = file byte COLOUR_RULE[k]: the file's word is A R G B, the stream's R G B A  [measured by this tool, see docs\systems\ring.md]
PLANTS = ('join_shift', 'colour_order', 'skirt_tie', 'lod_swap', 'part_only_join', 'lod_pin', 'no_names', 'uv_sign', 'uv_abs_off', 'site_flip', 'drop_952', 'drop_952_mesh1')
FILE_PLANTS = ('files_set', 'files_pairs_bad', 'files_slot_bad', 'files_weight_bad', 'files_two_pair_bad', 'files_colour_bad')
TEX_OK = ('equal', 'tail', 'padded')
SLOT_SAMPLERS = {0: ('g_mDiffusePrm', 'sampDiffusePrm'), 4: ('sampNormalPrm',), 5: ('sampSpecularPrm',)}


class CensusError(Exception):
    """An input that cannot be read. Never a pass."""


# ---------------------------------------------------------------- the host vertex rules ------------------------------------------------------------
def half_trunc(x):
    """float32 array -> uint16 half bits, rounded toward zero (mantissa >> 13): the recompiler's vpkd3d128 float16 pack (docs\\formats\\wrestler_host_vertex.md)."""
    b = np.ascontiguousarray(x, dtype='<f4').view('<u4').astype(np.uint32)
    s = (b >> 16) & 0x8000
    e = ((b >> 23) & 0xFF).astype(np.int32) - 127 + 15
    m = b & 0x7FFFFF
    norm = s | (np.clip(e, 0, 30).astype(np.uint32) << 10) | (m >> 13)
    sub = s | (((m | 0x800000) >> np.clip(14 - e, 0, 31).astype(np.uint32)).astype(np.uint32))
    out = np.where(e >= 31, s | 0x7C00, np.where(e <= 0, np.where(e < -10, s, sub), norm))
    out = np.where(((b >> 23) & 0xFF) == 255, s | 0x7C00 | np.where(m != 0, 0x200, 0), out)
    return out.astype(np.uint16)


def snorm_field(v, bits):
    """wrestler_geom.h Snorm16FromField: the game's nearest integer of value * max, then the capture layer's x 32767."""
    mx = float((1 << (bits - 1)) - 1)
    c = np.clip(v.astype(np.float64), -1.0, 1.0)
    i = np.rint(c * mx)
    f = np.maximum(i / mx, -1.0)
    sc = f * 32767.0
    return np.trunc(np.where(sc < 0, sc - 0.5, sc + 0.5)).astype(np.int16)


def expected_normal(nrm):
    return np.stack([snorm_field(nrm[:, 0], 10), snorm_field(nrm[:, 1], 11), snorm_field(nrm[:, 2], 11)], axis=1)


def roles_of(els):
    """The host layout's elements [(fmt, offset)] in guest-dword order -> {role: offset}. 109 position, 106 weights (f32x3), 41 slot bytes, 92 normal, 83 uv, 37 (8888 unorm):
    the ones before the slot element are the fill word, the others the vertex colour. (Same roles as docs\\formats\\wrestler_host_vertex.md.)"""
    r = {}
    ids = next((off for fm, off in els if fm == 41), None)
    for fm, off in els:
        if fm == 109 and off == 0:
            r['pos'] = off
        elif fm == 106:
            r['weights'] = off
        elif fm == 41:
            r['ids'] = off
        elif fm == 92:
            r.setdefault('normal', off)
        elif fm == 83:
            r['uv'] = off
        elif fm == 37:
            r['fill' if (ids is not None and off < ids) else ('xdword' if not any(f2 == 92 for f2, _ in els) else 'colour')] = off
        else:
            r.setdefault('other', []).append((fm, off))
    return r


def check_vertex(rows, n, stride, els, m, plant):
    """One stream draw's vertex bytes against ring mesh m. -> dict of per-role counts of vertices that DIFFER (0 = byte-equal) plus 'roles' and the colour permutations."""
    r = roles_of(els)
    out = {'roles': ','.join(sorted(k for k in r if k != 'other')) + ('+other' if 'other' in r else '')}
    pos = np.ascontiguousarray(rows[:, 0:12]).view('<f4').reshape(n, 3)
    out['pos'] = int((pos != m.positions).any(axis=1).sum())
    out['pos_w'] = int((np.ascontiguousarray(rows[:, 12:16]).view('<f4').reshape(n) != 1.0).sum())
    if 'ids' in r:
        k = m.skin_slot.shape[1] if m.skin_slot is not None else 0
        exp = np.full((n, 4), 255, np.uint8)
        for j in range(min(k, 4)):
            exp[:, j] = (m.skin_slot[:, j] & 0xFF).astype(np.uint8)
        out['ids'] = int((rows[:, r['ids']:r['ids'] + 4] != exp).any(axis=1).sum())
        used = m.skin_slot != 255 if m.skin_slot is not None else None
        out['slot_over_palette'] = int((m.skin_slot[used] >= max(len(m.palette), 1)).sum()) if used is not None else n
        # the weights word: k = 1 pair -> no weights element; k >= 2 -> f32x3
        if k >= 2:
            if 'weights' not in r:
                out['weights'] = n
            else:
                w3 = np.ascontiguousarray(rows[:, r['weights']:r['weights'] + 12]).view('<f4').reshape(n, 3)
                ew = np.zeros((n, 3), np.float32)
                for j in range(min(k - 1, 3)):
                    ew[:, j] = np.where(m.skin_slot[:, j] != 255, m.skin_weight[:, j], 0.0)
                out['weights'] = int((w3.view('<u4') != ew.view('<u4')).any(axis=1).sum())
        else:
            out['rigid_weights'] = n if 'weights' in r else 0   # a 1-pair mesh must carry NO weights element (vertices that break that)
        if 'fill' in r:
            fl = rows[:, r['fill']:r['fill'] + 4]
            out['fill'] = int((fl != np.array([0, 0, 0, 255], np.uint8)).any(axis=1).sum())
    if 'normal' in r:
        sn = np.ascontiguousarray(rows[:, r['normal']:r['normal'] + 8]).view('<i2').reshape(n, 4)
        en = expected_normal(m.normal)
        d = np.abs(sn[:, :3].astype(np.int32) - en.astype(np.int32))
        out['normal'] = int((d.max(axis=1) > 0).sum())
        out['normal_w'] = int((sn[:, 3] != 0).sum())
        out['normal_far'] = int((d.max(axis=1) > 70).sum())
    if 'colour' in r:
        sc = rows[:, r['colour']:r['colour'] + 4]
        fc = m.colour_file
        out['colour_perms'] = [p for p in itertools.permutations(range(4)) if all(np.array_equal(sc[:, kk], fc[:, p[kk]]) for kk in range(4))]
        out['colour_nongrey'] = int(((fc[:, 1] != fc[:, 2]) | (fc[:, 2] != fc[:, 3])).sum())
        out['colour_alpha_not_255'] = int((fc[:, 0] != 255).sum())
        rule = (0, 1, 2, 3) if plant == 'colour_order' else COLOUR_RULE
        out['colour'] = int((sc != fc[:, list(rule)]).any(axis=1).sum())
    if 'xdword' in r:  # the plane-shadow proxy's second 8888 element is NOT the vertex colour: (0, then bytes 1..3 of the position x float)
        xd = rows[:, r['xdword']:r['xdword'] + 4]
        out['xdword'] = int(((xd[:, 0] != 0) | (xd[:, 1:4] != rows[:, 1:4]).any(axis=1)).sum())
    if 'uv' in r:
        uv = np.ascontiguousarray(rows[:, r['uv']:r['uv'] + 4]).view('<u2').reshape(n, 2)
        u = m.uv[:, 0] if plant == 'uv_sign' else np.abs(m.uv[:, 0])
        eu, ev = half_trunc(u), half_trunc(m.uv[:, 1])
        out['uv'] = int(((uv[:, 0] != eu) | (uv[:, 1] != ev)).sum())
        neg = m.uv[:, 0] < 0
        out['uv_neg_file'] = int(neg.sum())
        out['uv_neg_stream'] = int(((uv[:, 0] & 0x8000) != 0)[neg].sum())
        # the half u without a sign against the file's |u|, only on the negative-u vertices: the bytes the SDK defect (assets.md) leaves
        out['uv_neg_abs_equal'] = int((uv[neg, 0] == half_trunc(m.uv[neg, 0] if plant == 'uv_abs_off' else np.abs(m.uv[neg, 0]))).sum())
    return out


# ---------------------------------------------------------------- the file side ----------------------------------------------------------------------
class RingIndex:
    def __init__(self, ring, plant=None):
        self.ring = ring
        self.by_counts = collections.defaultdict(list)
        for m in ring.meshes():
            if not m.ok or (plant == 'lod_pin' and m.entry == 980) or (plant == 'drop_952' and m.entry == 952) or (plant == 'drop_952_mesh1' and m.entry == 952 and m.index == 1):
                continue
            self.by_counts[(m.n, m.stream_index_count)].append(m)
        self.fp = {}

    def entry_fp(self, e):
        """The whole entry's content incl. its textures' bytes: two entries equal here are interchangeable for a replacement."""
        if e not in self.fp:
            h = hashlib.blake2b(digest_size=12)
            for m in self.ring.entries[e]:
                for a in (m.positions, m.normal, m.colour_file, m.uv, m.indices, m.skin_slot, m.skin_weight, m.tangent):
                    h.update(b'-' if a is None else np.ascontiguousarray(a).tobytes())
                h.update(m.effect.encode())
                h.update(repr(m.palette).encode())
                for key in sorted(m.samplers):
                    t = self.ring.textures.get(m.samplers[key].lower())
                    h.update(key.encode() + b'=' + (hashlib.blake2b(t.blob, digest_size=8).digest() if t else m.samplers[key].encode()))
            self.fp[e] = h.hexdigest()
        return self.fp[e]


def sampler_tex(ring, m, slot):
    for k in SLOT_SAMPLERS[slot]:
        if k in m.samplers:
            return ring.textures.get(m.samplers[k].lower())
    return None


def texture_result(ring, got, ft):
    """compare_texture of the bound image against the sampler's file texture; where it differs, the arena's same-family variants (ar_rop01 -> ar_rop01_blue / _red: the rope
    colours) are tried, and an exact one is reported 'variant:<name>'."""
    r = jc.compare_texture(got, ft)
    if r in TEX_OK:
        return r
    for nm, t in sorted(ring.textures.items()):
        if nm.startswith(ft.name.lower() + '_') and not nm.endswith(('_n', '_s')) and jc.compare_texture(got, t) in TEX_OK:
            return 'variant:' + nm
    return r


class DrawCache:
    """Per stream geometry (vertex bytes + layout): the position-equal ring meshes, the stream's indices and the vertex checks per mesh."""

    def __init__(self, cap, ri, plant):
        self.cap, self.ri, self.plant = cap, ri, plant
        self.d = {}

    def get(self, f, i):
        d = f.draws[i]
        L = f.layouts[int(d['layout'])]
        els = [(int(f.elements[int(L['first']) + k]['fmt']), int(f.elements[int(L['first']) + k]['off'])) for k in range(int(L['count']))]
        key = (int(d['vertex_offset']), int(d['vertex_bytes']), int(d['index_offset']), int(d['index_count']), int(d['vertex_count']), int(L['stride']), tuple(els))
        if key in self.d:
            return self.d[key]
        n, ic, stride = int(d['vertex_count']), int(d['index_count']), int(L['stride'])
        res = {'hits': [], 'elem': None, 'idx': None, 'els': els, 'checks': {}, 'rows': None, 'cands': []}
        self.d[key] = res
        if int(d['flags']) & jc.DRAW_NO_GEOMETRY:
            res['nogeo'] = True
            return res
        cands = self.ri.by_counts.get((n, ic), [])
        res['cands'] = cands
        if not cands:
            return res
        float_els, idx = jc.draw_float_elements(self.cap, f, i)
        res['idx'] = idx
        for k, p in float_els:
            hits = [m for m in cands if m.positions.shape == p.shape and np.array_equal(m.positions, p)]
            if hits:
                res['hits'], res['elem'] = hits, k
                break
        if res['hits']:
            raw = self.cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
            res['rows'] = raw[:n * stride].reshape(n, stride)
            res['stride'] = stride
        return res

    def vertex_check(self, res, m):
        k = (m.entry, m.index)
        if k not in res['checks']:
            res['checks'][k] = check_vertex(res['rows'], m.n, res['stride'], res['els'], m, self.plant)
        return res['checks'][k]


# ---------------------------------------------------------------- the census ------------------------------------------------------------------------
class Result:
    def __init__(self):
        self.checks = collections.OrderedDict()
        self.numbers = collections.OrderedDict()
        self.lists = collections.OrderedDict()

    def check(self, name, ok, detail):
        prev = self.checks.get(name)
        if prev and not prev[0]:
            return
        self.checks[name] = [bool(ok), detail]


def site_kind(site):
    if site == SITE_COLOUR:
        return 'colour'
    if site == SITE_MIRROR:
        return 'mirror'
    if site == SITE_SHADOW:
        return 'shadow_map'
    if site == SITE_PLANE:
        return 'plane'
    if SITE_TLAMBERT[0] <= site <= SITE_TLAMBERT[1]:
        return 'lambert'
    return 'other_%x' % site


def detect_arena(cap, frames, plant, verbose):
    """The arena whose ring the draws join best. Many arenas share one ring (the entries repeat, R.numbers 'distinct contents'), so the geometry alone cannot name the arena:
    ties are broken by how many bound slot-0 images equal the arena's own textures (the textures are per arena). -> the best arena id, or None."""
    best = ((0, 0), None)
    sample = frames[::max(1, len(frames) // 3)][:3]
    cap.catch_up(sample[-1])
    last = cap.frame(sample[-1], plant)
    cap.advance_contents(last, sample[-1])
    for aid in af.arena_ids():
        try:
            ring = rf.load_ring(aid)
        except Exception as e:
            if verbose:
                print('  detect: bg%02d unreadable (%s)' % (aid, e))
            continue
        if not ring.entries:
            continue
        ri = RingIndex(ring, None)
        dc = DrawCache(cap, ri, None)
        joined = tex_eq = 0
        for fn in sample:
            f = cap.frame(fn, plant)
            for i in range(len(f.draws)):
                if not int(f.draws[i]['vertex_count']):
                    continue
                r = dc.get(f, i)
                if not r['hits']:
                    continue
                joined += 1
                d = f.draws[i]
                img = next((int(s['image']) for s in f.slots[int(d['tex_first']):int(d['tex_first']) + int(d['tex_count'])] if int(s['slot']) == 0), 0)
                got = cap.image_data(img) if img else None
                ft = sampler_tex(ring, r['hits'][0], 0)
                if got is not None and ft is not None and jc.compare_texture(got, ft) in TEX_OK:
                    tex_eq += 1
        if (joined, tex_eq) > best[0]:
            best = ((joined, tex_eq), aid)
    return best[1]


def census(cap, arena_id, frame_sel, plant, verbose, expect):
    R = Result()
    t0 = time.time()
    if arena_id is None:
        arena_id = detect_arena(cap, frame_sel, plant, verbose)
        if arena_id is None:
            raise CensusError('%s: no ring arena joins any draw (names missing, or the capture is not a match)' % cap.path)
        cap = jc.Capture(cap.path)   # detection advanced the capture's texture-content state: start the census on a fresh reader
    ring = rf.load_ring(arena_id)
    if not ring.entries:
        raise CensusError('bg%02d has no ring entries' % arena_id)
    ri = RingIndex(ring, plant)
    dc = DrawCache(cap, ri, plant)
    R.numbers['arena'] = arena_id
    R.numbers['frames'] = len(frame_sel)
    exp = (expect or {}).get('captures', {}).get(str(arena_id), {})
    C = collections.Counter()
    obj_part = {}                          # obj -> {part: running intersection of candidate entries}
    obj_reused = set()
    frame_draws = {}                       # frame number -> {(obj, site): {part: draws}}
    frame_entries = {}                     # (obj) -> hits fingerprint of entries seen (for final mapping)
    site_by_obj = collections.defaultdict(collections.Counter)
    obj_rows = collections.defaultdict(dict)   # obj -> {frame number: tuple of row hashes of its part-lowest draws, in draw order}
    bad = collections.defaultdict(list)    # check name -> examples
    vtx = collections.Counter()            # role -> vertices that differ / vertices compared
    vtx_n = collections.Counter()
    role_sets = collections.Counter()
    perm_pool = None
    tex = collections.defaultdict(collections.Counter)   # slot -> result counts
    tex_piece = collections.defaultdict(collections.Counter)   # (piece, slot, texture name) -> result counts
    variant_seq = collections.defaultdict(dict)                # frame -> obj -> the texture variants of its part-0 draws in draw order
    skirt = collections.Counter()
    layout_ps = collections.defaultdict(collections.Counter)
    nogeo_site, nogeo_obj, unjoined_obj = collections.Counter(), collections.Counter(), collections.Counter()   # draws with no captured geometry by site / object; counts-equal but position-unequal draws by object
    tex_cache = {}
    no_names_frame = None

    for fn in frame_sel:
        cap.catch_up(fn)
        f = cap.frame(fn, plant)
        cap.advance_contents(f, fn)
        if not f.has_origins:
            no_names_frame = f.number
            break
        o, dr = f.origins, f.draws
        fd = frame_draws[f.number] = collections.defaultdict(lambda: collections.defaultdict(int))
        first_rows = collections.defaultdict(dict)
        for i in range(len(dr)):
            if not int(dr[i]['vertex_count']):
                continue
            m = dc.get(f, i)
            obj, part, site = int(o[i]['object']), int(o[i]['part']), int(o[i]['pass_site'])
            if not m['hits']:
                if m.get('nogeo'):
                    nogeo_site[site_kind(site)] += 1
                    nogeo_obj[obj] += 1
                    if ri.by_counts.get((int(dr[i]['vertex_count']), int(dr[i]['index_count']))):
                        C['no_geometry_draws_with_a_ring_mesh_count'] += 1
                else:
                    unjoined_obj[obj] += 1   # (judged after the loop: only the draws of an object that joined elsewhere count)
                    if m['cands']:   # the counts of a ring mesh, the positions of none
                        C['cands_but_no_position_hit'] += 1
                continue
            if plant == 'site_flip' and PIECE.get(m['hits'][0].entry) == 'post':
                site = SITE_MIRROR   # planted: the posts drawn through the mirror site
            C['ring_draws'] += 1
            hits = list(m['hits'])
            if plant == 'join_shift':  # the counts-only join, taking the next candidate: the mesh drawn is not looked at
                k0 = m['cands'].index(hits[0])
                hits = [m['cands'][(k0 + 1) % len(m['cands'])]]
            C['position_element_%s' % ('0' if m['elem'] == 0 else 'other')] += 1
            hp = [x for x in hits if x.index == part]
            if not hp:
                C['part_is_not_mesh'] += 1
                continue
            chosen = hp
            tex_img = next((int(s['image']) for s in f.slots[int(dr[i]['tex_first']):int(dr[i]['tex_first']) + int(dr[i]['tex_count'])] if int(s['slot']) == 0), 0)
            if len(chosen) > 1:
                if plant == 'skirt_tie':
                    chosen = [min(chosen, key=lambda x: x.entry)]   # no texture tie-break: every member of the class is the first one (lane 39's lesson)
                    C['ties_broken_by_first_entry_PLANT'] += 1
                elif tex_img:
                    payload = cap.image_data(tex_img)
                    if payload is not None:
                        keep = []
                        for x in chosen:
                            dt = sampler_tex(ring, x, 0)
                            if dt is not None and jc.compare_texture(payload, dt) in TEX_OK:
                                keep.append(x)
                        if keep and len(keep) < len(chosen):
                            chosen = keep
                            C['ties_resolved_by_texture'] += 1
            if plant == 'lod_swap' and chosen[0].entry in (960, 980):
                other = 980 if chosen[0].entry == 960 else 960
                if chosen[0].index < len(ring.entries.get(other, [])):
                    chosen = [ring.entries[other][chosen[0].index]]
            x = chosen[0]
            C['draws_joined'] += 1
            piece = PIECE.get(x.entry, '?')
            C['piece_draws_%s' % piece] += 1
            site_by_obj[obj][site_kind(site)] += 1
            layout_ps['%s/%s' % (piece, site_kind(site))][','.join(sorted(k for k in roles_of(m['els']) if k != 'other'))] += 1
            # the object's join: running intersection per part
            sset = {y.entry for y in chosen}
            pm = obj_part.setdefault(obj, {})
            if part in pm:
                pm[part] &= sset
                if not pm[part]:
                    obj_reused.add(obj)
            else:
                pm[part] = set(sset)
            fd[(obj, site)][part] += 1
            # content: positions (the join's own key, a plant moves it), indices, the vertex bytes
            if not np.array_equal(x.positions, np.ascontiguousarray(m['rows'][:, 0:12]).view('<f4').reshape(-1, 3)):
                bad['content_equal'].append((f.number, hex(obj), part, x.entry))
                if x.n != m['rows'].shape[0]:
                    continue   # (a planted join to a mesh of another size: the vertex rules have nothing to compare)
            if m['idx'] is not None:
                if m['idx'].shape == x.indices.shape and np.array_equal(m['idx'], x.indices):
                    C['index_equal'] += 1
                else:
                    C['index_differ'] += 1
            vc = dc.vertex_check(m, x)
            role_sets[vc['roles']] += 1
            for role in ('pos', 'pos_w', 'ids', 'slot_over_palette', 'weights', 'rigid_weights', 'fill', 'normal', 'normal_w', 'normal_far', 'colour', 'xdword', 'uv'):
                if role in vc:
                    vtx[role] += vc[role]
                    vtx_n[role] += x.n
                    if vc[role]:
                        bad[role].append((f.number, hex(obj), x.entry, x.index, vc[role]))
            if 'normal' in vc:
                vtx['normal_exact_rule_diff'] = vtx['normal']
            if 'uv_neg_file' in vc:
                vtx['uv_neg_file'] += vc['uv_neg_file']
                vtx['uv_neg_stream_negative'] += vc['uv_neg_stream']
                vtx['uv_neg_stream_equals_abs'] += vc['uv_neg_abs_equal']
            if 'colour_perms' in vc:
                s = set(vc['colour_perms'])
                perm_pool = s if perm_pool is None else (perm_pool & s)
                vtx['colour_nongrey'] += vc['colour_nongrey']
                vtx['colour_alpha_not_255'] += vc['colour_alpha_not_255']
            # textures, only the colour pass binds them as the model's own
            if site == SITE_COLOUR:
                slots = {int(s['slot']): int(s['image']) for s in f.slots[int(dr[i]['tex_first']):int(dr[i]['tex_first']) + int(dr[i]['tex_count'])]}
                for sl in (0, 4, 5):
                    ft = sampler_tex(ring, x, sl)
                    img = slots.get(sl, 0)
                    if ft is None:
                        tex[sl]['bound_not_named' if img else 'none_named_none_bound'] += 1
                        continue
                    if not img:
                        tex[sl]['named_not_bound'] += 1
                        continue
                    got = cap.image_data(img)
                    if got is None:
                        tex[sl]['no_capture_content'] += 1
                        continue
                    ck = (img, cap.contents.get(img), ft.name, ft.dir)
                    if ck not in tex_cache:
                        tex_cache[ck] = texture_result(ring, got, ft)
                    tex[sl][tex_cache[ck]] += 1
                    tex_piece[(piece, sl, ft.name)][tex_cache[ck]] += 1
                    if sl == 0 and tex_cache[ck].startswith('variant:') and part == 0:
                        variant_seq[f.number].setdefault(obj, []).append(tex_cache[ck][8:])
                    if sl == 0 and piece == 'skirt':
                        skirt['draws'] += 1
                        both = [y for y in hp if sampler_tex(ring, y, 0) is not None and jc.compare_texture(got, sampler_tex(ring, y, 0)) in TEX_OK]
                        skirt['texture_matches_%d_of_%d_candidates' % (len(both), len(hp))] += 1
                        if len(both) > 1 and len({ri.entry_fp(y.entry) for y in both}) == 1:
                            skirt['candidates_identical_entries_interchangeable'] += 1
                        if tex_cache[ck] in TEX_OK:
                            skirt['chosen_entry_texture_ok'] += 1
                        else:
                            skirt['chosen_entry_texture_WRONG'] += 1
                        skirt['entry_%d' % x.entry] += 1
            # the rows c76.. of the instance: the part-lowest draws, in draw order
            if site == SITE_COLOUR or site_kind(site) in ('plane', 'lambert'):
                first_rows[obj].setdefault(part, []).append(f.reg(int(dr[i]['vs_bank']), 76, 3 * max(len(x.palette), 1)).tobytes())
        for obj, parts in first_rows.items():
            obj_rows[obj][f.number] = tuple(hashlib.blake2b(b, digest_size=6).digest() for b in parts[min(parts)])
        C['frames_with_ring'] += 1 if fd else 0

    if no_names_frame is not None:
        R.check('names', False, 'frame %d has no origin section (a version-1 capture is unnamed)' % no_names_frame)
        return R, arena_id, time.time() - t0
    R.numbers.update(C)
    R.check('names', C['ring_draws'] > 0, '%d ring draws joined over %d frames (origins present)' % (C['ring_draws'], len(frame_sel)))

    # ----- the unjoined draws
    joined_objs = set(obj_part)
    unj_ring = sum(v for ob, v in unjoined_obj.items() if ob in joined_objs)
    nogeo_ring = sum(v for ob, v in nogeo_obj.items() if ob in joined_objs)
    R.numbers.update(draws_with_ring_counts_but_no_position_hit=C['cands_but_no_position_hit'], unjoined_draws_of_joined_ring_objects=unj_ring,
                     draws_with_no_captured_geometry=sum(nogeo_site.values()), no_geometry_draws_whose_counts_equal_a_ring_mesh=C['no_geometry_draws_with_a_ring_mesh_count'], no_geometry_draws_by_pass=dict(nogeo_site), no_geometry_draws_of_ring_objects=nogeo_ring)
    R.check('unjoined', C['cands_but_no_position_hit'] == 0 and unj_ring == 0 and nogeo_ring == 0 and C['no_geometry_draws_with_a_ring_mesh_count'] == 0 and C['ring_draws'] > 0,
            'draws with a ring mesh counts but no position hit: %d (objects %s); draws of a joined ring object that did not join: %d; no-geometry draws %d by pass %s, of which ring objects: %d' % (
                C['cands_but_no_position_hit'], [hex(k) for k in unjoined_obj if k in joined_objs][:3], unj_ring, sum(nogeo_site.values()), dict(nogeo_site), nogeo_ring))

    # ----- the join per object
    only_first = plant == 'part_only_join'
    final, uniq, tied_eq, distinct = {}, 0, 0, 0
    amb = []
    one_part_ties = 0
    for obj, pm in obj_part.items():
        parts = {min(pm): pm[min(pm)]} if only_first else pm
        inter = set.intersection(*parts.values()) if parts else set()
        if len(pm[min(pm)]) > 1:
            one_part_ties += 1
        final[obj] = inter
        if obj in obj_reused or not inter:
            distinct += 1
            amb.append((hex(obj), 'no entry fits every part / address reused'))
        elif len(inter) == 1:
            uniq += 1
        else:
            if len({ri.entry_fp(e) for e in inter}) == 1:
                tied_eq += 1
                amb.append((hex(obj), 'interchangeable entries %s' % sorted(inter)))
            else:
                distinct += 1
                amb.append((hex(obj), 'DISTINCT entries tie: %s' % sorted(inter)))
    R.numbers.update(objects=len(obj_part), objects_unique=uniq, objects_tied_interchangeable=tied_eq, objects_tied_distinct_or_none=distinct,
                     objects_tied_on_their_first_part_alone=one_part_ties)
    R.lists['tied_objects'] = amb[:30]
    R.check('join_unique', distinct == 0 and uniq > 0,
            '%d objects: %d join to exactly one entry (%d of them were tied on their first part alone, split by the other parts or the texture), %d tie between identical entries, %d '
            'not unique and not interchangeable' % (len(obj_part), uniq, one_part_ties, tied_eq, distinct))
    R.check('content_equal', not bad['content_equal'] and C['draws_joined'] > 0, '%d joined draws whose mesh positions differ from the stream (of %d joined)' % (len(bad['content_equal']), C['draws_joined']))
    R.check('part_is_mesh', C['part_is_not_mesh'] == 0, '%d position-equal draws whose mesh index is not the draw part (of %d ring draws)' % (C['part_is_not_mesh'], C['ring_draws']))
    R.check('position_element', C['position_element_other'] == 0 and C['position_element_0'] > 0, 'the joined position is element 0 of the stream layout on %d draws, another element on %d' % (C['position_element_0'], C['position_element_other']))
    R.check('index_content', C['index_differ'] == 0 and C['index_equal'] > 0, 'stream indices equal the file strips (groups joined by one restart) on %d draws, differ on %d' % (C['index_equal'], C['index_differ']))

    # ----- stability, pieces, sites
    obj_entry = {obj: next(iter(s)) for obj, s in final.items() if len(s) == 1 and obj not in obj_reused}
    for obj, s in final.items():
        if len(s) > 1 and obj not in obj_reused:
            obj_entry[obj] = min(s)
    ent_objs = collections.defaultdict(set)
    for obj, e in obj_entry.items():
        ent_objs[e].add(obj)
    multi = {e: sorted(hex(a) for a in v) for e, v in ent_objs.items() if len(v) > 1 and PIECE.get(e) != 'skirt'}
    skirts_total = sum(len(v) for e, v in ent_objs.items() if PIECE.get(e) == 'skirt')
    R.numbers.update(objects_per_entry={e: len(v) for e, v in sorted(ent_objs.items())}, skirt_objects=skirts_total, addresses_reused=len(obj_reused))
    R.lists['entries_with_several_objects'] = multi
    R.check('stability', not multi and not obj_reused and skirts_total <= 4 and len(ent_objs) > 0,
            'one object per entry over the capture (entries with several: %d), skirt objects %d (at most 4), addresses that changed entry %d' % (len(multi), skirts_total, len(obj_reused)))

    sites = collections.defaultdict(collections.Counter)   # piece -> site kind -> draws
    for obj, c in site_by_obj.items():
        e = obj_entry.get(obj)
        for k, v in c.items():
            sites[PIECE.get(e, '?')][k] += v
    R.lists['draws_by_piece_and_pass'] = {p: dict(c) for p, c in sorted(sites.items())}
    off_site = [(p, k, v) for p, c in sites.items() for k, v in c.items() if (p == 'proxy' and k not in ('plane', 'lambert')) or (p != 'proxy' and k != 'colour')]
    mirror_draws = sum(c.get('mirror', 0) for c in sites.values())
    shadow_draws = sum(c.get('shadow_map', 0) for c in sites.values())
    R.numbers.update(ring_draws_at_mirror_site=mirror_draws, ring_draws_in_T_ShadowMap=shadow_draws)
    R.lists['vertex_layout_by_piece_and_pass'] = {k: dict(v) for k, v in sorted(layout_ps.items())}
    want_lay = exp.get('layouts')
    if want_lay is not None:
        got_lay = {k: sorted(v) for k, v in layout_ps.items()}
        R.check('layouts', got_lay == {k: sorted(v) for k, v in want_lay.items()}, 'the host vertex layout per (piece, pass) is exactly the pinned one in expect.json: %s' % ('same' if got_lay == want_lay else 'DIFFERENT: %s' % got_lay))
    R.check('mirror', mirror_draws == 0 and C['draws_joined'] > 0, '%d ring draws go through the mirror site 0x82C0B94C' % mirror_draws)
    R.check('sites', not off_site and shadow_draws == 0 and sites.get('proxy') is not None,
            'every ring piece draws only in the colour pass 0x82C0A950, the proxy 956 only in the plane shadow / lambert range; off-site draws %s; ring draws in T_ShadowMap %d' % (off_site[:4], shadow_draws))
    px = sites.get('proxy', {})
    R.check('proxy', sum(v for k, v in px.items() if k in ('plane', 'lambert')) > 0 and not px.get('colour'),
            '956 (rope plane-shadow proxy): %s; never in the colour pass' % dict(px))

    # ----- instances per piece and frame, the rope LOD
    per_frame = {}
    exp = (expect or {}).get('captures', {}).get(str(arena_id), {})
    inst_bad, rope_bad, lod_frames, parts_inconsistent = [], [], collections.Counter(), 0
    skirt_inst_max = 0
    for fnum, fd in frame_draws.items():
        pc = collections.Counter()
        for (obj, site), parts in fd.items():
            e = obj_entry.get(obj)
            if e is None:
                continue
            counts = set(parts.values())
            if len(counts) > 1:
                parts_inconsistent += 1
            inst = max(parts.values())
            pc[PIECE.get(e, '?')] += inst
            if PIECE.get(e) == 'skirt':
                skirt_inst_max = max(skirt_inst_max, inst)
        per_frame[fnum] = pc
    logical_seen = {('rope' if p.startswith('rope_') else p) for pc in per_frame.values() for p in pc}
    frames_without_ring = 0
    for fnum, pc in per_frame.items():
        hi, lo = pc.get('rope_high', 0), pc.get('rope_low', 0)
        lod_frames[(hi, lo)] += 1
        if not pc:
            frames_without_ring += 1
            continue
        lp = collections.Counter()
        for p, v in pc.items():
            lp['rope' if p.startswith('rope_') else p] += v
        for piece, want in exp.get('instances', {}).items():
            if lp.get(piece, 0) != want:
                inst_bad.append((fnum, piece, lp.get(piece, 0), want))
        for piece in logical_seen - set(lp):    # a piece the capture shows elsewhere, absent on a frame that draws other ring pieces
            if piece not in exp.get('instances', {}):
                inst_bad.append((fnum, piece, 0, 'seen elsewhere'))
        if (hi or lo) and hi + lo != exp.get('instances', {}).get('rope', 12):
            rope_bad.append((fnum, hi, lo))
    pieces_seen = {p for pc in per_frame.values() for p in pc}
    R.numbers.update(parts_of_one_instance_drawn_unequally=parts_inconsistent, skirt_max_instances_per_object=skirt_inst_max, frames_without_any_ring_draw=frames_without_ring)
    R.lists['instances_per_frame_min_max'] = {p: [min(pc.get(p, 0) for pc in per_frame.values()), max(pc.get(p, 0) for pc in per_frame.values())] for p in sorted(pieces_seen)}
    R.lists['rope_lod_high_low_frames'] = {'%d+%d' % k: v for k, v in sorted(lod_frames.items())}
    want_pieces = set(exp.get('instances', {}))
    R.check('instances', not inst_bad and parts_inconsistent == 0 and skirt_inst_max <= 1 and want_pieces <= logical_seen and bool(want_pieces) and frames_without_ring < len(per_frame),
            'every frame that draws a ring piece draws EVERY piece with exactly its expected instance count (rope = high + low; %d bad frames %s; expected %s); parts of an instance drawn unequally %d; '
            'skirt instances per object <= 1 (max %d); expected pieces never seen: %s' % (len(inst_bad), inst_bad[:3], exp.get('instances', {}), parts_inconsistent, skirt_inst_max, sorted(want_pieces - logical_seen)))
    rope_total = exp.get('instances', {}).get('rope', 12)
    R.check('lod', bool(lod_frames) and not rope_bad and all(hi + lo == rope_total for (hi, lo) in lod_frames if hi or lo) and (not exp.get('lod_both_seen') or (any(hi for hi, lo in lod_frames) and any(lo for hi, lo in lod_frames))),
            'rope instances per frame: (high+low) %s; frames with high+low != %d: %d%s' % (dict(sorted(lod_frames.items())), rope_total, len(rope_bad),
                                                                                         ('; both LODs mixed in %d frames' % sum(v for (hi, lo), v in lod_frames.items() if hi and lo))))

    # ----- the host vertex rules
    def rule(name, role, what, na=False):
        R.check(name, (vtx_n[role] > 0 or na) and vtx[role] == 0, '%s: %d of %d vertices differ (%d bad draws %s)' % (what, vtx[role], vtx_n[role], len(bad[role]), bad[role][:2]))
    rule('skin', 'ids', 'slot-id bytes == the file pair slots (255 for an absent pair), over every skinned draw')
    R.check('skin_palette', vtx_n['slot_over_palette'] > 0 and vtx['slot_over_palette'] == 0, 'every used slot is below the mesh palette count: %d violations' % vtx['slot_over_palette'])
    wexp = exp.get('weights_compared', True)
    R.check('weights_present', (vtx_n['weights'] > 0) == bool(wexp), 'draws of 2+-pair meshes compared: %d vertices (this capture %s have any, per expect.json)' % (vtx_n['weights'], 'should' if wexp else 'should not'))
    rule('weights', 'weights', 'weights f32x3 == the first (pairs - 1) pair weights, bit-equal, 0 beyond (2+-pair draws only; n/a when the capture has none)', na=not wexp)
    rule('rigid_no_weights', 'rigid_weights', 'a 1-pair (rigid) mesh draw carries NO weights element')
    rule('fill', 'fill', 'the fill word @16 is (0,0,0,255) on every vertex of every draw that has one')
    rule('position', 'pos', 'positions bit-equal to the file')
    rule('position_w', 'pos_w', 'position w == 1.0')
    rule('normal_w', 'normal_w', 'normal w == 0')
    rule('normal', 'normal', 'normal == the game 10:11:11 pack expanded to snorm16 (wrestler_geom.h rule)')
    rule('colour_order', 'colour', 'colour == the file word rotated %s (stream byte k = file byte rule[k])' % (COLOUR_RULE,))
    rule('uv', 'uv', 'uv halves == half(|file u|), half(file v), rounded toward zero')
    pool = sorted(perm_pool) if perm_pool is not None else []
    R.numbers.update(colour_permutations_that_fit_every_vertex_of_every_draw=pool, host_layouts_seen=dict(role_sets),
                     uv_vertices_with_negative_file_u=vtx['uv_neg_file'], uv_negative_u_vertices_whose_stream_u_is_negative=vtx['uv_neg_stream_negative'],
                     uv_negative_u_vertices_whose_stream_u_equals_abs=vtx['uv_neg_stream_equals_abs'], normal_vertices_over_70_steps=vtx['normal_far'])
    if vtx_n['xdword'] or px.get('plane'):
        rule('proxy_vertex', 'xdword', 'the plane-shadow proxy 956 (plane pass) second 8888 element is NOT the file colour: it is (0, bytes 1..3 of the position x float)')
    else:
        R.check('proxy_vertex', not bad['xdword'], 'no proxy draw in the plane-shadow pass in this capture (the T_Lambert-range proxy carries the full vertex, checked by the rules above): n/a')
    ng, a_not = vtx['colour_nongrey'], vtx['colour_alpha_not_255']
    R.numbers.update(colour_vertices_with_unequal_rgb=ng, colour_vertices_with_alpha_not_255=a_not)
    R.check('colour_unique', COLOUR_RULE in pool and all(p[3] == 0 for p in pool) and (len(pool) == 1 or ng == 0),
            'colour byte orders that fit every vertex of every draw: %s (the rule %s is among them, the file alpha byte is always the stream\'s last); the order of the three RGB bytes is observable only '
            'where they differ: vertices with unequal RGB %d, with alpha not 255: %d' % (pool, COLOUR_RULE, ng, a_not))
    R.check('uv_sign', vtx['uv_neg_file'] > 0 and vtx['uv_neg_stream_negative'] == 0 and vtx['uv_neg_stream_equals_abs'] == vtx['uv_neg_file'],
            'vertices with a negative file u: %d; of them with a negative stream u: %d, with stream u == half(|u|): %d (the SDK vpkd3d128 |u| defect, assets.md)' % (
                vtx['uv_neg_file'], vtx['uv_neg_stream_negative'], vtx['uv_neg_stream_equals_abs']))

    # ----- textures and the skirt tie
    ok_all = True
    parts_txt = []
    for sl in (0, 4, 5):
        c = tex[sl]
        parts_txt.append('slot %d %s' % (sl, dict(c)))
        if c['differ'] or c['padded_differ']:
            ok_all = False
    R.lists['texture_slots'] = {str(sl): dict(tex[sl]) for sl in (0, 4, 5)}
    R.lists['texture_by_piece'] = {'%s/slot%d/%s' % k: dict(v) for k, v in sorted(tex_piece.items())}
    seqs = collections.Counter()
    for fnum, byobj in variant_seq.items():
        for obj, seq in byobj.items():
            seqs[(PIECE.get(obj_entry.get(obj), '?'), tuple(seq))] += 1
    R.lists['texture_variants_per_instance_sequences (piece, variants in draw order): frames'] = [[k[0], list(k[1]), v] for k, v in seqs.most_common(4)]
    nvar = sum(v for sl in (0, 4, 5) for k, v in tex[sl].items() if k.startswith('variant:'))
    R.numbers['draws_bound_to_a_same_family_variant_texture'] = nvar
    R.check('texture_bytes', ok_all and (tex[0]['equal'] + tex[0]['tail'] + tex[0]['padded']) > 0,
            'ring draws\' bound images against the entry\'s sampler textures (dir 400; "variant:" = an exact same-family texture of the arena, e.g. the red / blue rope): ' + '; '.join(parts_txt))
    sk_bad = skirt['chosen_entry_texture_WRONG']
    sk_one = sum(v for k, v in skirt.items() if k.startswith('texture_matches_1_of_'))
    sk_eq = skirt['candidates_identical_entries_interchangeable']
    R.lists['skirt'] = dict(skirt)
    R.check('skirt_tie', skirt['draws'] > 0 and sk_bad == 0 and sk_one + sk_eq == skirt['draws']
            and len([k for k in skirt if k.startswith('entry_')]) >= (2 if exp.get('skirt_both_entries', True) else 1),
            'skirt draws %d: texture bytes match exactly one of the geometry-identical entries on %d (both on %d, whose entries are identical, so interchangeable), the chosen entry\'s texture is wrong on %d; '
            'entries used %s' % (skirt['draws'], sk_one, sk_eq, sk_bad, {k[6:]: v for k, v in skirt.items() if k.startswith('entry_')}))

    # ----- the rows (information only: where the palette moves)
    rows_info = {}
    for obj, byf in obj_rows.items():
        e = obj_entry.get(obj)
        fl = sorted(byf)
        pairs = [(a, b) for a, b in zip(fl, fl[1:])]
        ch = sum(1 for a, b in pairs if byf[a] != byf[b])
        key = PIECE.get(e, '?')
        r = rows_info.setdefault(key, [0, 0])
        r[0] += ch
        r[1] += len(pairs)
    R.lists['palette_rows_changed_between_sampled_frames'] = {k: '%d of %d' % tuple(v) for k, v in sorted(rows_info.items())}
    return R, arena_id, time.time() - t0


# ---------------------------------------------------------------- the files-only census ---------------------------------------------------------------
def files_census(plant, expect, verbose):
    R = Result()
    ids = af.arena_ids()
    cen = rf.ring_census_all(ids)
    if plant == 'files_set':   # a planted arena that lost an entry
        first = next(a for a, r in cen.items() if r['entries'])
        cen[first] = dict(cen[first], entries=[e for e in cen[first]['entries'] if e != 953])
    first = next(a for a, r in cen.items() if r['entries'])
    if plant == 'files_pairs_bad':
        pr = dict(cen[first]['pairs'])
        pr[3] = 1
        cen[first] = dict(cen[first], pairs=pr)
    if plant == 'files_slot_bad':
        cen[first] = dict(cen[first], slot_over_palette=1)
    if plant == 'files_weight_bad':
        cen[first] = dict(cen[first], weight_sum_bad=1)
    if plant == 'files_two_pair_bad':
        cen[first] = dict(cen[first], two_pair=cen[first]['two_pair'] + [(953, 0)])
    if plant == 'files_colour_bad':
        cen[first] = dict(cen[first], nongrey=1)
    ex = (expect or {}).get('files', {})
    S = set(rf.RING_SET)
    full = [a for a, r in cen.items() if set(r['entries']) == S]
    none = [a for a, r in cen.items() if not r['entries']]
    partial = [(a, r['entries']) for a, r in cen.items() if r['entries'] and set(r['entries']) != S]
    R.numbers.update(arenas_read=len(cen), arenas_with_the_ring_set=len(full), arenas_without_ring_entries=none, arenas_with_a_partial_set=partial)
    R.check('files_read', len(cen) == ex.get('arenas', 57) and all(r['unparsed'] == 0 for r in cen.values()),
            '%d bgNN.pac read (expected %s); meshes whose streams do not tile or have no skin: %d' % (len(cen), ex.get('arenas', 57), sum(r['unparsed'] for r in cen.values())))
    R.check('files_ring_set', len(full) == ex.get('with_ring', 49) and none == ex.get('ringless', none) and not partial,
            '%d arenas carry exactly {951 952 953 956 960 964 973 974 980 981 982} (expected %s), ringless arenas %s (expected %s), partial sets %s' % (
                len(full), ex.get('with_ring'), none, ex.get('ringless'), partial))
    contents = {e: len({r['content'][e] for r in cen.values() if e in r['content']}) for e in sorted(S)}
    R.numbers['distinct_contents_per_entry'] = contents
    want = {int(k): v for k, v in ex.get('contents', {}).items()}
    R.check('files_contents', bool(want) and contents == want, 'distinct decoded entry contents across the arenas %s (expected %s)' % (contents, want))
    pairs = collections.Counter()
    for r in cen.values():
        pairs.update(r['pairs'])
    over = sum(r['slot_over_palette'] for r in cen.values())
    wbad = sum(r['weight_sum_bad'] for r in cen.values())
    R.numbers['skin_pairs_per_vertex_over_all_meshes'] = dict(pairs)
    R.check('files_skin', set(pairs) <= set(ex.get('pairs', [1, 2])) and over == 0 and wbad == 0,
            'meshes by skin pairs per vertex %s (allowed %s); meshes with a slot beyond the palette %d; meshes whose weights do not sum to 1: %d' % (dict(pairs), ex.get('pairs', [1, 2]), over, wbad))
    tp_bad = [a for a, r in cen.items() if r['entries'] and sorted(r['two_pair']) != [(960, 0), (960, 1)]]
    R.numbers['two_pair_meshes_over_all_arenas'] = sum(len(r['two_pair']) for r in cen.values())
    R.check('files_two_pair', not tp_bad and any(r['entries'] for r in cen.values()),
            'the two-pair meshes of a ring arena are exactly 960 mesh 0 and 960 mesh 1 (%d such meshes over all arenas); arenas where they are not: %s' % (sum(len(r['two_pair']) for r in cen.values()), tp_bad))
    verts, ng, an = sum(r['verts'] for r in cen.values()), sum(r['nongrey'] for r in cen.values()), sum(r['alpha_not_255'] for r in cen.values())
    R.numbers.update(ring_vertices_over_all_arenas=verts, ring_vertices_with_unequal_rgb=ng, ring_vertices_with_alpha_not_255=an)
    R.check('files_colour', verts > 0 and ng == 0 and an == 0, 'ring vertex colours over all %d arenas (%d vertices): unequal RGB %d, alpha not 255 %d (so only the alpha position of the colour word is observable)' % (len(cen), verts, ng, an))
    neg_entries = collections.Counter()
    neg_verts = 0
    neg_arenas = set()
    for a, r in cen.items():
        for e, lst in r['neg_u'].items():
            neg_entries[e] += 1
            neg_arenas.add(a)
            neg_verts += sum(c for _, c, _ in lst)
    R.numbers.update(arenas_with_a_negative_u_ring_mesh=len(neg_arenas), negative_u_vertices_over_all_arenas=neg_verts)
    R.lists['negative_u_by_entry (arenas)'] = dict(sorted(neg_entries.items()))
    R.lists['negative_u_meshes_in_bg04'] = {str(e): v for e, v in cen[4]['neg_u'].items()} if 4 in cen else {}
    want_neg = sorted(ex.get('neg_u_entries', []))
    R.check('files_neg_u', bool(want_neg) and sorted(neg_entries) == want_neg and neg_verts > 0,
            'ring meshes with a negative u (the SDK vpkd3d128 |u| defect applies to them): entries %s in %d arenas, %d vertices (expected entries %s)' % (sorted(neg_entries), len(neg_arenas), neg_verts, want_neg))
    return R


# ---------------------------------------------------------------- command line ----------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('captures', nargs='*')
    ap.add_argument('--files', action='store_true')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default='', choices=('',) + PLANTS + FILE_PLANTS)
    ap.add_argument('--json', default='')
    ap.add_argument('--expect', default=os.path.join(HERE, 'expect.json'))
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    if not a.captures and not a.files:
        ap.error('give a capture or --files')
    expect = {}
    if os.path.isfile(a.expect):
        expect = json.load(open(a.expect, encoding='utf-8'))
    elif a.expect != os.path.join(HERE, 'expect.json'):
        print('FAIL  expect file %s not found' % a.expect)
        return 1
    out, failed = {}, 0
    if a.files:
        try:
            R = files_census(a.plant or None, expect, not a.q)
        except (CensusError, af.FileReadError, OSError) as e:
            print('FAIL  files census: %s' % e)
            return 1
        print('== files-only ring census%s' % ('  PLANT ' + a.plant if a.plant else ''))
        for k, v in R.numbers.items():
            if not a.q:
                print('   %-52s %s' % (k, v))
        for k, (ok, why) in R.checks.items():
            print('   %s  %-16s %s' % ('PASS' if ok else 'FAIL', k, why))
            failed += 0 if ok else 1
        if not a.q:
            for k, v in R.lists.items():
                if v:
                    print('   list %s: %s' % (k, json.dumps(v, default=str)[:700]))
        out['files'] = {'numbers': R.numbers, 'checks': R.checks, 'lists': R.lists}
    main_co = jc.MAIN
    for spec in a.captures:
        path, arena = spec, None
        m = re.match(r'^(.*):(\d+)$', spec)
        if m and not re.match(r'^[A-Za-z]$', m.group(1)):
            path, arena = m.group(1), int(m.group(2))
        if not os.path.exists(path) and os.path.exists(os.path.join(main_co, path)):
            path = os.path.join(main_co, path)
        try:
            cap = jc.Capture(path)
            sel = jc.parse_frames(a.frames, len(cap.frames))
            if not sel:
                raise CensusError('%s: --frames %s selects no frame' % (path, a.frames))
            R, aid, secs = census(cap, arena, sel, a.plant or None, not a.q, expect)
            if arena is not None and aid != arena:
                raise CensusError('arena %d expected, %d found' % (arena, aid))
        except (CensusError, jc.CensusError, af.FileReadError, OSError, zipfile.BadZipFile) as e:
            print('FAIL  %s: %s' % (os.path.basename(path), e))
            failed += 1
            continue
        print('== %s  arena bg%02d  %d frames  %.1f s%s' % (os.path.basename(path), aid, len(sel), secs, '  PLANT ' + a.plant if a.plant else ''))
        for k, v in R.numbers.items():
            if not a.q:
                print('   %-52s %s' % (k, v))
        for k, (ok, why) in R.checks.items():
            print('   %s  %-16s %s' % ('PASS' if ok else 'FAIL', k, why))
            failed += 0 if ok else 1
        if not a.q:
            for k, v in R.lists.items():
                if v:
                    print('   list %s: %s' % (k, json.dumps(v, default=str)[:700]))
        out[os.path.basename(path)] = {'arena': aid, 'numbers': R.numbers, 'checks': R.checks, 'lists': R.lists}
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(out, open(a.json, 'w'), indent=1, default=str)
    print('ring_census: %s' % ('FAIL (%d)' % failed if failed else 'PASS'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
