#!/usr/bin/env python3
r"""Lag finder (Session 103, lane T4; Fable 5.2's review section 3 tool 6). 0 game runs, 0 Surface runs; offline, reads a kept case in place.

  py -I tools\scene\lag_finder.py CASE.zip [--frames N] [--json OUT] [--assume NAME=OFFSET ...]
  py -I tools\scene\lag_finder.py --selftest        (planted offsets on a real case; --refind runs the re-finds)
  py -I tools\scene\lag_finder.py CASE.zip --refind w1|e7|drawhalf|particles|all

Why: the clock bugs of Session 103 (W1: record j held stream j+1's palette; E7: part clocks lag +2 hook frames; the ring callback and every draw-half hook note at the RECORD frame, not the
hook frame; F5: the particle node noted at hook+1 and Take required noted == hook) were each found by hand, one run apart. Every RECORD series should equal a STREAM series (the draws' constants)
at ONE offset; this tool measures all pairs at once and prints ONE table of clocks.

Method (no second parser: records and frames are read by tools\skip\ledger_io.py, the case zip in place):
  a RECORD series = (label, key, vector) rows from a section; a STREAM series = (stream frame, key, vector) from the draws' constant banks (exact registers per fx field).
  Offset e: the stream frame s (its guest frame) is paired with the record content labelled s - e. The label is the record's own guest frame for state series (unit "rec") and the
  row's hook-clock stamp for stamped rows (unit "stamp": E7's part clocks, the ring, circles, plane rows ... the sections that carry their own `frame`). e > 0 = the record lags the stream.
  Score = the share of non-trivial stream vectors (bit patterns, per key where the join has one) found bit-equal in the record at label s - e. Offsets -3..+3.
  - best offset = the one with the most bit-equal hits; if several offsets TIE the series is "static, unproven" (a value that never changes cannot prove its clock);
  - when no offset reaches 50 % bit-equal the CLOSEST offset is also printed with its error (median over a sample of the nearest record vector's max abs difference).
  - the contract's expected offset (FRAME_CONTRACT.md / the lane notes, EXPECT below) is checked; a pair whose best offset differs is flagged "NOT EXPECTED".
Exit 0 for a report run; --refind exits 1 when a known miss does not show at its known offset.
"""
import argparse, collections, json, os, struct, sys, zipfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L  # noqa: E402
from ledger_io import Frame, Record, Registry  # noqa: E402

OFFSETS = range(-3, 4)
TRIVIAL = {0, 0x3F800000, 0xBF800000, 0x80000000}
SITES = (L.SITE_A, L.SITE_B)
M32 = 0xFFFFFFFF

# ---------------------------------------------------------------------------------------------------------------------------------------------------
# the table of expected offsets: series -> (offset in the series' own unit, where it comes from)
EXPECT = {
    'palettes':    (0, 'W1 fixed (kPaletteLead = 1: record j now holds what stream j draws); a PRE-fix case (run 26) shows +1 [C W1: 31464/31464 at record f-1]'),
    'part_clocks': (2, 'E7 [C]: section 50 equals the stream c186 at lag +2 hook frames (not 1; 21,192 draws, 0 differ)'),
    # FRAME_CONTRACT section 3: stream frame j carries the walks of hook frame j - 1, so a row stamped by a hook is paired with stream frame stamp + 1
    'ring_rows':   (1, 'FRAME_CONTRACT 3: stream j carries the walks of hook frame j-1'),
    'circles':     (1, 'FRAME_CONTRACT 3: stream j carries the walks of hook frame j-1 (rows stamped with the hook frame)'),
    'plane_rows':  (1, 'FRAME_CONTRACT 3: stream j carries the walks of hook frame j-1'),
    'arena_worlds': (1, 'FRAME_CONTRACT 3: stream j carries the walks of hook frame j-1'),
}
CENSUS_OF = {'part_clocks': 'part_clocks (section 50)', 'arena_worlds': 'arena_worlds (section 15)', 'circles': 'circles (section 16)', 'plane_rows': 'plane_draws (plane node sub_827EF070)'}
# what a hook-clock reading (HookFrameOf(j) = j - 1) would have assumed for a stamped series: the wrong assumption the draw-half class re-finds
HOOK_ASSUMED = 1


def u32s(vals):
    return struct.pack('<%dI' % len(vals), *[v & M32 for v in vals])


def f32s(vals):
    return np.asarray(vals, dtype=np.float32).tobytes()


def trivial(vec):
    w = struct.unpack('<%dI' % (len(vec) // 4), vec)
    return all(x in TRIVIAL for x in w)


# ---------------------------------------------------------------------------------------------------------------------------------------------------
class Case:
    """A kept case read in place: a zip (capture/frames/*.bin, capture/scene/*.rsc), a case folder or a capture folder."""

    def __init__(self, path):
        self.path = path
        self.zf = None
        if path.lower().endswith('.zip'):
            if not os.path.isfile(path):
                raise L.LedgerError('case zip not found: ' + path)
            self.zf = zipfile.ZipFile(path)
            names = self.zf.namelist()
            self.prefix = 'capture/' if any(n.startswith('capture/frames/') for n in names) else ''
            self.recs = sorted(int(n[-10:-4]) for n in names if n.startswith(self.prefix + 'scene/') and n.endswith('.rsc'))
            self.frames = sorted(int(n[-10:-4]) for n in names if n.startswith(self.prefix + 'frames/') and n.endswith('.bin'))
        else:
            d = L.find_capture(path)
            self.dir = d
            self.recs = sorted(int(x[6:12]) for x in os.listdir(os.path.join(d, 'scene')) if x.endswith('.rsc')) if os.path.isdir(os.path.join(d, 'scene')) else []
            self.frames = sorted(int(x[6:12]) for x in os.listdir(os.path.join(d, 'frames')) if x.endswith('.bin'))
        if not self.recs:
            raise L.LedgerError('no scene records in ' + path)
        self._f, self._r = {}, {}
        self.reg = Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))

    def _read(self, sub, i, ext):
        if self.zf:
            return self.zf.read('%s%s/frame_%06d.%s' % (self.prefix, sub, i, ext))
        with open(os.path.join(self.dir, sub, 'frame_%06d.%s' % (i, ext)), 'rb') as fh:
            return fh.read()

    def frame(self, i):
        if i not in self._f:
            self._f[i] = Frame('frame_%06d.bin' % i, data=self._read('frames', i, 'bin'))
        return self._f[i]

    def record(self, i):
        if i not in self._r:
            self._r[i] = Record('frame_%06d.rsc' % i, data=self._read('scene', i, 'rsc'))
        return self._r[i]


# ---------------------------------------------------------------------------------------------------------------------------------------------------
# small decoders for the sections ledger_io does not read (layouts: src\renderer_scene\scene_record.h; element sizes are checked)
def rows(rec, sid, es_want, fmt):
    if sid not in rec.S:
        return []
    es, cnt, so = rec.S[sid]
    if es != es_want:
        raise ValueError('section %d element_size %d, this tool knows %d' % (sid, es, es_want))
    return [struct.unpack_from(fmt, rec.b, so + es * i) for i in range(cnt)]


def sec_circles(rec):     # 16: frame, character, slot, flags, param[4], view_proj[16], points[21][4]
    return rows(rec, 16, 432, '<4I4f16f84f')


def sec_plane_frames(rec):   # 18: version, frame, flags, have, fade[10], normal[4], fit[16], camera[16]
    return rows(rec, 18, 200, '<4I10f4f16f16f')


def sec_plane_draws(rec):    # 19: version, frame, kind, flags, object, part, have, ordinal, plane_pos[4], blur_scale[4], blur_weight[4], blur_matrix[16], blur_point[4]
    return rows(rec, 19, 160, '<8I4f4f4f16f4f')


def sec_plane_eyes(rec):     # 49: version, frame, have, flags, eye[8][4], plane_pos[4]
    return rows(rec, 49, 160, '<4I32f4f')


def sec_post(rec):           # 29: layout, flags, frame(u64), settings_addr, pad, s[176]
    es = rec.S[29][0] if 29 in rec.S else 760   # BHF4: 728 (before FXF) or 760 (fx[8] words after S)
    if es not in (728, 760):
        raise ValueError('section 29 element_size %d, this tool knows 728 and 760' % es)
    return rows(rec, 29, es, '<IIQII176I' if es == 728 else '<IIQII176I8I')


def sec_input_stamps(rec):   # 31: bit, lag, moved, clock, reserved(u32), frame(u64)
    return rows(rec, 31, 16, '<4BIQ')


def sec_model_runtime(rec):  # 30: layout, flags, object, pad, frame(u64), alpha, reserved
    return rows(rec, 30, 32, '<IIIIQfI')


def sec_part_clocks(rec):    # 50: version, frame, object, part, time, period, step, slot
    return rows(rec, 50, 32, '<IIIIfffI')


# ---------------------------------------------------------------------------------------------------------------------------------------------------
# record series: each returns an iterator of (label or None, key, vector bytes); label None = the record's own guest frame (unit "rec")
def mat_cols(m):
    bits = struct.unpack('<16I', struct.pack('<16f', *m))
    for k in range(4):
        yield u32s(bits[k::4])


def rec_palettes(rec):
    engine = rec.engine_objects()
    bones = rec.bones()
    for oi, o in enumerate(rec.objects()):
        key = engine[oi] if oi < len(engine) else 0
        if not key or not o['bones_count']:
            continue
        for b in bones[o['bones_first']:o['bones_first'] + o['bones_count']]:
            for v in mat_cols(b):
                yield None, key, v


def rec_worlds(rec):
    """section 15 kind 1: the evaluated world of an arena engine object (12 floats = the 3 rows c76..c78), stamped with the drawing thread's frame."""
    for w in rec.arena_worlds():
        if w['kind'] != 1 or not w['object']:
            continue
        for k in range(0, 12, 4):
            yield w['frame'], w['object'], f32s(w['world'][k:k + 4])


def rec_part_clocks(rec):
    for (ver, fr, obj, part, t, per, step, slot) in sec_part_clocks(rec):
        yield fr, obj, f32s([t])


def rec_ring(rec):
    for r in rec.ring_rows():
        for row in r['rows']:
            yield r['frame'], 0, f32s(row)


def rec_circles(rec):
    for v in sec_circles(rec):
        fr = v[0]
        flat = v[4:]
        yield fr, 0, f32s(flat[0:4])
        for k in range(4):
            yield fr, 0, f32s(flat[4 + 4 * k:8 + 4 * k])
        for p in range(21):
            yield fr, 0, f32s(flat[20 + 4 * p:24 + 4 * p])


def _matrix_vecs(m):
    """4 rows and 4 columns of a 16-float matrix as vectors."""
    for k in range(4):
        yield f32s(m[4 * k:4 * k + 4])
    for k in range(4):
        yield f32s(m[k::4])


def rec_plane(rec):
    for v in sec_plane_frames(rec):
        fr = v[1]
        fade, normal, fit, cam = v[4:14], v[14:18], v[18:34], v[34:50]
        yield fr, 0, f32s(normal)
        for x in _matrix_vecs(fit):
            yield fr, 0, x
        for x in _matrix_vecs(cam):
            yield fr, 0, x
    for v in sec_plane_draws(rec):
        fr = v[1]
        for blk in (v[8:12], v[12:16], v[16:20], v[36:40]):
            yield fr, 0, f32s(blk)
        for x in _matrix_vecs(v[20:36]):
            yield fr, 0, x
    for v in sec_plane_eyes(rec):
        fr = v[1]
        for k in range(8):
            yield fr, 0, f32s(v[4 + 4 * k:8 + 4 * k])
        yield fr, 0, f32s(v[36:40])


def rec_light_states(rec):
    for st in rec.light_states():
        lt = st['light']
        for k in range(0, 224, 4):
            yield None, 0, f32s(lt[k:k + 4])


def rec_shadow(rec):
    for st in rec.light_states():
        for m in st['shadow_m']:
            for x in _matrix_vecs(m):
                yield None, 0, x
        for x in _matrix_vecs(st['shadow_f']):
            yield None, 0, x
        for blk in st['shadow_x']:
            for k in range(0, 8, 4):
                yield None, 0, f32s(blk[k:k + 4])


def rec_post(rec):
    for v in sec_post(rec):
        fr, w = v[2], v[5:5 + 176]
        for k in range(0, 176, 4):
            yield fr, 0, u32s(w[k:k + 4])


def rec_camera(rec):
    cam = rec.camera()
    if not cam:
        return
    v = np.array(cam['view'], dtype=np.float32).reshape(4, 4)
    p = np.array(cam['proj'], dtype=np.float32).reshape(4, 4)
    for m in (v @ p, p @ v):
        flat = m.astype(np.float32).reshape(-1).tolist()
        for x in _matrix_vecs(flat):
            yield None, 0, x


def rec_params(rec):
    engine = rec.engine_objects()
    params = rec.params()
    for oi, o in enumerate(rec.objects()):
        key = engine[oi] if oi < len(engine) else 0
        if not key:
            continue
        for name, v in params[o['params_first']:o['params_first'] + o['params_count']]:
            yield None, key, f32s(v)


# ---------------------------------------------------------------------------------------------------------------------------------------------------
# stream series: each takes (frame, reg, draws list) and yields (key, vector bytes)
def vec_at(bank, r, n=1):
    return u32s(bank[4 * r:4 * (r + n)])


def draws_of(case, f):
    """[(origin, variant name, draw)] of the frame, cached."""
    if not hasattr(f, '_dl'):
        out = []
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o:
                continue
            d = f.draw(i)
            out.append((o, case.reg.variant_of(d['vs']) or '', case.reg.variant_of(d['ps']) or '', d))
        f._dl = out
    return f._dl


def st_palettes(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and vn.startswith('yCh_') and o['object']:
            vb = f.bank(d['vs_bank'])
            for r in range(76, 139):
                yield o['object'], vec_at(vb, r)


def st_worlds(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and (vn.startswith('yProp_') or vn.startswith('yBG_')) and o['object']:
            vb = f.bank(d['vs_bank'])
            for r in (76, 77, 78):
                yield o['object'], vec_at(vb, r)


def st_part_clocks(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and o['pass_site'] in SITES and (vn.startswith('yProp_') or vn.startswith('yBG_')) and o['object']:
            vb = f.bank(d['vs_bank'])
            yield o['object'], u32s(vb[4 * 186:4 * 186 + 1])


def st_ring(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and (vn.startswith('yProp_') or vn.startswith('yBG_')):
            vb = f.bank(d['vs_bank'])
            yield 0, vec_at(vb, 76, 3)


def st_technique(sub, vs_regs, ps_regs):
    def fn(case, f):
        for o, vn, pn, d in draws_of(case, f):
            if sub in o['technique']:
                vb = f.bank(d['vs_bank'])
                pb = f.bank(d['ps_bank'])
                for r in vs_regs:
                    yield 0, vec_at(vb, r)
                for r in ps_regs:
                    yield 0, vec_at(pb, r)
    return fn


def st_circles(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if 'CircleShadow' in o['technique']:
            vb = f.bank(d['vs_bank'])
            for r in [0] + list(range(4, 29)):
                yield 0, vec_at(vb, r)


def st_light(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and vn[:3] in ('yCh', 'yPr', 'yBG'):
            pb = f.bank(d['ps_bank'])
            for r in (0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 16, 17, 18, 19):
                yield 0, vec_at(pb, r)


def st_shadow(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and vn[:3] in ('yCh', 'yPr', 'yBG'):
            vb = f.bank(d['vs_bank'])
            pb = f.bank(d['ps_bank'])
            for r in (68, 69, 70, 71, 62, 63, 168, 169):
                yield 0, vec_at(vb, r)
            for r in (168, 169, 170):
                yield 0, vec_at(pb, r)


def st_post(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 8 or pn.startswith(('yBlur', 'yDownScale', 'yGlareEtc', 'yMonotone', 'yAntialiasing')):
            pb = f.bank(d['ps_bank'])
            vb = f.bank(d['vs_bank'])
            for r in range(0, 64):
                yield 0, vec_at(pb, r)
                yield 0, vec_at(vb, r)


def st_camera(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and vn[:3] in ('yCh', 'yPr', 'yBG'):
            vb = f.bank(d['vs_bank'])
            for r in (56, 57, 58, 59):
                yield 0, vec_at(vb, r)
            return


def st_params(case, f):
    for o, vn, pn, d in draws_of(case, f):
        if o['klass'] == 1 and vn[:3] in ('yCh', 'yPr', 'yBG') and o['object']:
            pb = f.bank(d['ps_bank'])
            for r in (189, 190, 181, 182, 180, 161, 162, 195):
                yield o['object'], vec_at(pb, r)


# name, unit ('rec' / 'stamp'), record fn, stream fn, keyed (key from the engine object address), what is compared
SERIES = [
    ('palettes',      'rec',   rec_palettes,     st_palettes,     True,  'section 5 bones of an object  <->  VS c76..c138 of its yCh_ draws'),
    ('arena_worlds', 'stamp', rec_worlds,       st_worlds,       True,  'section 15 evaluated world of an arena object  <->  VS c76..c78 of its yProp_/yBG_ draws'),
    ('part_clocks',   'stamp', rec_part_clocks,  st_part_clocks,  True,  'section 50 time  <->  VS c186 of the arena main-pass draws'),
    ('ring_rows',     'stamp', rec_ring,         st_ring,         False, 'sections 20/21 node rows  <->  VS c76..c78 of the prop draws'),
    ('circles',       'stamp', rec_circles,      st_circles,      False, 'section 16 ShadowParam / ViewProj / 21 body points  <->  VS c0, c4..c28 of T_CircleShadow'),
    ('plane_rows',    'stamp', rec_plane,        st_technique('Plane', range(0, 80), range(0, 16)), False, 'sections 18/19/49 plane frame, blur, eyes  <->  constants of the T_Plane* draws'),
    ('light_states',  'rec',   rec_light_states, st_light,        False, 'section 13 light rows  <->  PS DifLghCol / DifLghVec / DifLghPrm of model draws'),
    ('shadow_cameras', 'rec',  rec_shadow,       st_shadow,       False, 'section 13 shadow camera rows  <->  VS TexProjLight / ViewProjLightDepth / ShadowPrm of model draws'),
    ('post_settings', 'stamp', rec_post,         st_post,         False, 'section 29 settings words  <->  any constant of the post draws'),
    ('camera',        'rec',   rec_camera,       st_camera,       False, 'section 2 view x proj  <->  VS ViewProj c56..c59 (closest, not a bit-equal product by contract)'),
    ('material_params', 'rec', rec_params,       st_params,       True,  'section 6 named values of an object  <->  PS MatAmbCol / MatDifCol / Sky / Ground / Rim / Fog'),
]


# ---------------------------------------------------------------------------------------------------------------------------------------------------
def collect(case, spec, frames):
    """-> (store, stream): store[label][key] = set(vec); stream[guest][key] = set(vec)."""
    name, unit, recfn, stfn, keyed, desc = spec
    store = collections.defaultdict(lambda: collections.defaultdict(set))
    for i in case.recs:
        rec = case.record(i)
        g = rec.guest_frame & M32
        for label, key, vec in recfn(rec):
            if trivial(vec):
                continue
            store[(g if label is None else label & M32)][key if keyed else 0].add(vec)
    stream = {}
    for i in frames:
        f = case.frame(i)
        d = collections.defaultdict(set)
        for key, vec in stfn(case, f):
            if not trivial(vec):
                d[key if keyed else 0].add(vec)
        stream[f.guest_frame & M32] = d
    return store, stream


def score(store, stream, e):
    hits = tot = 0
    for s, d in stream.items():
        rs = store.get((s - e) & M32)
        if rs is None:
            continue
        for key, vs in d.items():
            r = rs.get(key, ())
            tot += len(vs)
            hits += len(vs & r) if r else 0
    return hits, tot


def near_error(store, stream, e, cap=250):
    errs = []
    n = 0
    for s, d in sorted(stream.items()):
        rs = store.get((s - e) & M32)
        if rs is None:
            continue
        for key, vs in d.items():
            r = rs.get(key)
            if not r:
                continue
            rl = [v for v in r if len(v) == len(next(iter(vs)))]
            if not rl:
                continue
            ra = np.frombuffer(b''.join(rl), dtype=np.float32).reshape(len(rl), -1)
            for v in list(vs)[:3]:
                va = np.frombuffer(v, dtype=np.float32)
                errs.append(float(np.nanmin(np.max(np.abs(ra - va), axis=1))))
                n += 1
                if n >= cap:
                    return float(np.median(errs))
    return float(np.median(errs)) if errs else None


def analyse(case, spec, frames, assume=None):
    store, stream = collect(case, spec, frames)
    name, unit = spec[0], spec[1]
    res = dict(name=name, unit=unit, desc=spec[5], stream_vecs=sum(len(v) for d in stream.values() for v in d.values()),
               record_vecs=sum(len(v) for d in store.values() for v in d.values()), scores={})
    for e in OFFSETS:
        h, t = score(store, stream, e)
        res['scores'][e] = (h, t)
    best = max(h for h, t in res['scores'].values())
    top = [e for e, (h, t) in res['scores'].items() if h == best]
    tot = max(t for h, t in res['scores'].values()) if res['scores'] else 0
    res['best_hits'] = best
    res['total'] = tot
    frac = best / tot if tot else 0.0
    res['frac'] = frac
    res['ties'] = top
    if res['record_vecs'] == 0 or res['stream_vecs'] == 0:
        res['verdict'] = 'no join (empty side)'
        res['offset'] = None
    elif best == 0 or frac < 0.02:
        res['verdict'] = 'no join'
        res['offset'] = None
    elif frac < 0.25 and len(top) == 1:
        res['verdict'] = 'weak join'   # one offset leads, but most stream vectors have no record twin: the series is not fully read
        res['offset'] = None
        res['ties'] = top
    elif len(top) > 1 or sorted(h for h, t_ in res['scores'].values())[-2] >= 0.9 * best:   # a runner-up within 10 %: the series barely moves, its clock is not proven
        res['verdict'] = 'static, unproven'
        res['ties'] = [e for e, (h, t_) in res['scores'].items() if h >= 0.9 * best]
        res['offset'] = None
    else:
        res['verdict'] = 'proven'
        res['offset'] = top[0]
    if frac < 0.5:
        errs = {e: near_error(store, stream, e) for e in OFFSETS}
        errs = {e: v for e, v in errs.items() if v is not None}
        if errs:
            ce = min(errs, key=lambda e: errs[e])
            res['closest'] = (ce, errs[ce])
    exp = EXPECT.get(name)
    if exp is not None and res['verdict'] == 'proven':
        res['expected'] = exp[0]
        res['flag'] = 'ok' if res['offset'] == exp[0] else 'NOT EXPECTED (contract %+d: %s)' % exp
    elif exp is not None:
        res['expected'] = exp[0]
        res['flag'] = 'NOT PROVEN (contract %+d: %s)' % exp
    else:
        res['expected'] = None
        res['flag'] = 'no contract entry'
    return res


# ---------------------------------------------------------------------------------------------------------------------------------------------------
def stamp_census(case):
    """Per stamped section: (row stamp) - HookFrameOf(record) = stamp - (record guest - 1), over every record. 0 = update-half (hook clock), +1 = draw-half (the record's own frame)."""
    out = collections.OrderedDict()
    def add(name, stamps_of):
        c = collections.Counter()
        for i in case.recs:
            rec = case.record(i)
            hook = (rec.guest_frame - 1) & M32
            for st in stamps_of(rec):
                d = ((st - hook) & M32)
                d = d - (1 << 32) if d > (1 << 31) else d
                c[d if -3 <= d <= 3 else 'other'] += 1
        if c:
            out[name] = c
    add('ring_rows (sub_825D0438, draw half)', lambda r: [x['frame'] for x in r.ring_rows()])
    add('plane_draws (plane node sub_827EF070)', lambda r: [v[1] for v in sec_plane_draws(r)])
    add('plane_frames', lambda r: [v[1] for v in sec_plane_frames(r)])
    add('plane_eyes', lambda r: [v[1] for v in sec_plane_eyes(r)])
    add('circles (section 16)', lambda r: [v[0] for v in sec_circles(r)])
    add('part_clocks (section 50)', lambda r: [v[1] for v in sec_part_clocks(r)])
    add('arena_worlds (section 15)', lambda r: [x['frame'] for x in r.arena_worlds()])
    add('caster_calls (22)', lambda r: [x['frame'] for x in r.caster_calls()])
    add('light_refs (14)', lambda r: [x[3] for x in r.light_refs()])
    add('post_params (29)', lambda r: [v[2] for v in sec_post(r)])
    add('model_runtime (30)', lambda r: [v[4] for v in sec_model_runtime(r)])
    add('input_stamps (31, hook clock)', lambda r: [v[5] for v in sec_input_stamps(r)])
    return out


def print_table(results, census, case_name):
    print('LAG FINDER  case %s  (offset e = stream guest frame - record label; e > 0 = the record lags; stamp = hook-clock stamp of a row, rec = the record\'s own frame)' % case_name)
    print('%-16s %-5s %-9s %-8s %-7s %-20s %s' % ('series', 'unit', 'offset', 'hook-lag', 'bit-eq', 'verdict', 'contract / note'))
    for r in results:
        if r['offset'] is not None:
            best = '%+d' % r['offset']
        elif r['ties'] and r['verdict'] == 'static, unproven':
            best = 'tie ' + (','.join('%+d' % e for e in r['ties']) if len(r['ties']) < 7 else 'all')
        elif r['verdict'] == 'weak join':
            best = '(%+d)' % r['ties'][0]
        else:
            best = '-'
        cen = census.get(CENSUS_OF.get(r['name'], ''))
        hook_lag = '-'
        if r['unit'] == 'stamp' and cen and r['offset'] is not None:
            one = [d for d in cen if cen[d] > 0.95 * sum(cen.values())]
            if len(one) == 1 and isinstance(one[0], int):
                # the walk's hook frame h = stamp - bucket (a draw-half hook reads the clock already advanced); stream = stamp + e = h + bucket + e
                hook_lag = '%+d' % (r['offset'] + one[0])
        pct = '%.1f%%' % (100.0 * r['frac']) if r['total'] else '-'
        note = r['flag']
        if 'closest' in r:
            note += '; closest %+d (median max-abs err %.3g)' % r['closest']
        print('%-16s %-5s %-9s %-8s %-7s %-20s %s' % (r['name'], r['unit'], best, 'hook' + hook_lag, pct, r['verdict'], note))
        print('    vectors: record %d, stream %d' % (r['record_vecs'], r['stream_vecs']))
        print('    scores: ' + '  '.join('%+d:%d/%d' % (e, h, t) for e, (h, t) in sorted(r['scores'].items())))
    print('particles        stamp -                 -       none recorded     no particle section / draws in these cases (F5): see the draw-half census')
    print()
    print('stamp census, row stamp - HookFrameOf(record) (0 = update-half hook, +1 = draw-half hook noted at the RECORD frame; FRAME_CONTRACT "the draw-half clock"):')
    for name, c in census.items():
        print('  %-42s %s' % (name, '  '.join(('%+d: %d' % (d, n)) if isinstance(d, int) else ('%s: %d' % (d, n)) for d, n in sorted(c.items(), key=lambda kv: (isinstance(kv[0], str), kv[0] if isinstance(kv[0], int) else 0)))))


# ---------------------------------------------------------------------------------------------------------------------------------------------------
def pick(name):
    return [s for s in SERIES if s[0] == name][0]


def run_refinds(case, frames, which, pre, pre_frames):
    """The known past misses: each must show at its known offset. Returns the number of failures."""
    bad = 0
    def check(cond, msg):
        nonlocal bad
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        if not cond:
            bad += 1
    def line(r):
        return '  '.join('%+d:%d/%d' % (e, h, t) for e, (h, t) in sorted(r['scores'].items()))
    if which in ('w1', 'all'):
        print('RE-FIND W1: the palette lead (pre-fix case %s, fixed case %s)' % (os.path.basename(pre.path), os.path.basename(case.path)))
        r = analyse(pre, pick('palettes'), pre_frames)
        s = r['scores']
        print('  pre-fix  palettes: ' + line(r))
        check(r['verdict'] == 'proven' and r['offset'] == 1, 'the pre-fix palettes pair at offset +1 (stream f = record f-1: "the record held stream j+1\'s palette"), bit-equal %.1f%%' % (100.0 * r['frac']))
        check(s[0][0] * 20 < s[1][0], 'the pairing the old code assumed (offset 0, record j = stream j) holds %d of %d' % (s[0][0], s[0][1]))
        r2 = analyse(case, pick('palettes'), frames)
        s2 = r2['scores']
        print('  fixed    palettes: ' + line(r2))
        check(r2['verdict'] == 'proven' and r2['offset'] == 0, 'the FIXED pairing (kPaletteLead = 1 merged): offset 0 in %s, bit-equal %.1f%%, and +1 holds %d of %d' % (os.path.basename(case.path), 100.0 * r2['frac'], s2[1][0], s2[1][1]))
    if which in ('e7', 'all'):
        print('RE-FIND E7: part clocks at +2')
        for c, fr in ((pre, pre_frames), (case, frames)):
            r = analyse(c, pick('part_clocks'), fr)
            s = r['scores']
            print('  %s part_clocks: %s' % (os.path.basename(c.path), line(r)))
            check(r['verdict'] == 'proven' and r['offset'] == 2, 'the part clocks pair at offset +2 (not the +1 the first reading assumed: it holds %d of %d), bit-equal %.1f%%' % (s[1][0], s[1][1], 100.0 * r['frac']))
    if which in ('drawhalf', 'all'):
        print('RE-FIND the draw-half +1 (runs 14 / 16: a hook in the DRAW half notes at the RECORD frame, one later than the hook clock)')
        cen = stamp_census(case)
        ring = cen.get('ring_rows (sub_825D0438, draw half)', {})
        plane = cen.get('plane_draws (plane node sub_827EF070)', {})
        circ = cen.get('circles (section 16)', {})
        pcl = cen.get('part_clocks (section 50)', {})
        aw = cen.get('arena_worlds (section 15)', {})
        check(ring.get(1, 0) > 0, 'ring callback rows noted at +1 against HookFrameOf(record): %d of %d (the rest sit in the +0 bucket: %d)' % (ring.get(1, 0), sum(ring.values()), ring.get(0, 0)))
        check(plane and plane.get(1, 0) == sum(plane.values()), 'plane node rows (draw half): %d of %d at +1' % (plane.get(1, 0), sum(plane.values())))
        check(circ and circ.get(1, 0) == sum(circ.values()), 'circle rows: %d of %d at +1' % (circ.get(1, 0), sum(circ.values())))
        check(pcl and pcl.get(0, 0) == sum(pcl.values()) and aw and aw.get(0, 0) == sum(aw.values()), 'update-half rows (part clocks %d, arena worlds %d) are all at +0: the class split is by hook half' % (sum(pcl.values()), sum(aw.values())))
        rc = analyse(case, pick('circles'), frames)
        print('  circles: ' + line(rc))
        check(rc['verdict'] == 'proven' and rc['offset'] == 1, 'circle rows (stamped at the record frame) pair at stamp offset +1 = hook+2, bit-equal %.1f%%; the update-half reading (+2) holds %d of %d' % (100.0 * rc['frac'], rc['scores'][2][0], rc['scores'][2][1]))
        rr = analyse(case, pick('ring_rows'), frames)
        print('  ring_rows: ' + line(rr))
        check(rr['verdict'] == 'proven' and rr['offset'] == 1, 'ring rows pair at stamp offset +1, bit-equal %.1f%%' % (100.0 * rr['frac']))
    if which in ('particles', 'all'):
        print('RE-FIND F5: the particle node (a draw-half task) noted at hook+1; Take required noted == hook')
        cen = stamp_census(case)
        dh = [n for n, c in cen.items() if n.startswith(('plane_draws', 'plane_frames', 'plane_eyes', 'circles')) and c.get(1, 0) == sum(c.values())]
        check(len(dh) >= 3, 'every draw-half task of the record notes at +1 (the class F5 hit): %s' % ', '.join(dh))
        print('  note: the particle node writes no record section and the cases hold no game particle draws (F5 proved it by a fixture flare), so its own delta cannot be read here; the class is re-found. A Take that required noted == HookFrameOf(record) refuses all of these rows: %s' % ('yes: they are all +1' if len(dh) >= 3 else 'not shown'))
    return bad


def selftest(case, frames):
    """A planted clock: a record store shifted by +2 frames must be found at +2 / -1 (the planted offset), and a constant series must come out 'static, unproven'."""
    bad = 0
    spec = pick('part_clocks')
    store, stream = collect(case, spec, frames)
    for plant in (-2, 1, 3):
        shifted = {((lab + plant) & M32): v for lab, v in store.items()}   # content of label x now sits at x + plant
        h = {e: score(shifted, stream, e)[0] for e in OFFSETS}
        base = {e: score(store, stream, e)[0] for e in OFFSETS}
        eb = max(base, key=lambda e: base[e])
        want = eb - plant   # the content of label x now sits at x + plant, so the stream's offset becomes e0 - plant
        got = max(h, key=lambda e: h[e]) if max(h.values()) > 0 else None
        if want not in h:
            ok = got is None   # out of the measured range: nothing may be found
        else:
            ok = got == want and list(h.values()).count(max(h.values())) == 1
        print('  plant %+d on the part clocks (true %+d, expected %+d): found %s -> %s' % (plant, eb, want, 'at %+d' % got if got is not None else 'nothing', 'ok' if ok else 'FAIL'))
        if not ok:
            bad += 1
    # a static series: the same vector in every record and every stream frame ties at every offset
    v = f32s([1.5, 2.5, 3.5, 4.5])
    st_store = {g: {0: {v}} for g in range(100, 140)}
    st_stream = {g: {0: {v}} for g in range(110, 130)}
    hs = {e: score(st_store, st_stream, e)[0] for e in OFFSETS}
    tie = len(set(hs.values())) == 1
    print('  a constant series ties at every offset (%s): %s' % (list(hs.values())[0], 'static, unproven: ok' if tie else 'FAIL'))
    bad += 0 if tie else 1
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('case', nargs='?')
    ap.add_argument('--frames', type=int, default=0, help='stream frames to read (default: every frame that has a record neighbour, max 140 evenly spread)')
    ap.add_argument('--json', default='')
    ap.add_argument('--series', default='', help='comma list of series names (default all)')
    ap.add_argument('--refind', default='', help='w1 | e7 | drawhalf | particles | all')
    ap.add_argument('--pre', default='', help='the PRE-fix case for the W1 / E7 re-finds (default: run26_case.zip next to the case)')
    ap.add_argument('--selftest', action='store_true')
    x = ap.parse_args(argv)
    default_case = os.path.join(L.main_checkout(), 'out', 's103', 'run28_case.zip')
    path = x.case or default_case
    try:
        case = Case(path)
    except (L.LedgerError, OSError) as e:
        print('FAIL  %s' % e)
        return 1
    lo, hi = case.recs[0] + 4, case.recs[-1] - 4
    cand = [i for i in case.frames if lo <= i <= hi and case.record(i) is not None] if False else [i for i in case.frames if lo <= i <= hi]
    n = x.frames or 140
    step = max(1, len(cand) // n)
    frames = cand[::step]
    if x.selftest:
        print('lag_finder selftest on %s (%d stream frames)' % (os.path.basename(path), len(frames)))
        bad = selftest(case, frames)
        print('lag_finder selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
        return 1 if bad else 0
    if x.refind:
        pre_path = x.pre or os.path.join(os.path.dirname(os.path.abspath(path)), 'run26_case.zip')
        try:
            pre = Case(pre_path)
        except (L.LedgerError, OSError) as e:
            print('FAIL  the pre-fix case (run 26, W1 / E7 were found on it): %s' % e)
            return 1
        plo, phi = pre.recs[0] + 4, pre.recs[-1] - 4
        pcand = [i for i in pre.frames if plo <= i <= phi]
        pre_frames = pcand[::max(1, len(pcand) // n)]
        bad = run_refinds(case, frames, x.refind, pre, pre_frames)
        print('lag_finder refind %s: %s' % (x.refind, 'FAIL (%d)' % bad if bad else 'PASS'))
        return 1 if bad else 0
    names = x.series.split(',') if x.series else None
    results = [analyse(case, s, frames) for s in SERIES if not names or s[0] in names]
    census = stamp_census(case)
    print_table(results, census, os.path.basename(path))
    if x.json:
        with open(x.json, 'w', encoding='utf-8') as fh:
            json.dump(dict(case=os.path.basename(path), frames=len(frames),
                           series=[{k: (v if k != 'scores' else {str(e): list(s) for e, s in v.items()}) for k, v in r.items()} for r in results],
                           census={k: {str(d): n for d, n in c.items()} for k, c in census.items()}), fh, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
