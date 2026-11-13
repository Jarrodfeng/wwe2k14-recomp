r"""Lane PC (S101, docs\renderer\POST_PROMOTION_MAP.md section 7 lane PC): the post census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's section 1 on every frame of the case zips / capture folders it is given: the chain's draw sequence and its SHAPE
(match, film, entrance, two_layer, editor, cards ...), the layering (Bryan's two DoF layers: the combine's slot 2 and slot 6 images, and who wrote slot 6), the combine (exactly one
per chain frame, its variant, its call site), the dead work (bloom / glare drawn and copied at zero strength), the samplers of every chain slot (the DoF depth filter per capture), the
combine's constants (c20 DOFParam, c21 DOFParam2, c27 / c29 / c30 haze, c31 BloomScale, c32 StarScale), the chain's targets / sizes / formats / roles, the copies and the provenance of the
images they make, WHO READS THE CHAIN'S OUTPUTS (only the combine), the draws after the combine, the "post 24 = 17 + 7" split, and what is NOT in any capture (yRetroFilm, the combine's
night / tonemap / adapt permutations, the luminance chain ...).

  py -I tools\post\post_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--expect FILE] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--emit-expect OUT] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder (relative paths are read from the main checkout). LABEL names the capture's entry in expect.json.
Exit 1 when any check FAILs or an input cannot be read.

Post draws are found by NAME where the frame carries origins (call site 0x82C1F48C with a PS other than yBlur/flag_any, or 0x82C1F674) and by EFFECT where it does not (a version-1
capture: a yToneMap/quad / yPostBase/uv / yGlareEtc/corner_uv vertex shader with a chain pixel shader). On named frames both are computed and must agree (check classify).
Checks: names, classify, shapes, combine, layers, dead_work, filters, constants, targets, readers, after_combine, split, notseen; plus "expect.<group>" (every measured group against
expect.json; --emit-expect writes the draft).
--plant (tests only): names_stripped (origins stripped: a version-1 capture; fails exactly `names`), combine_dropped (the combine draw is not seen: fails `combine`),
layer_merged (the combine's slot 6 is read as its slot 2: Bryan's two layers collapse; fails `layers`).
Numbers only: counts, ids, effect names, constants. Nothing from the game is written; --json is for under out\."""
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
for sub in ('particles', 'solver'):
    sys.path.insert(0, os.path.join(ROOT, 'tools', sub))
import pcap  # noqa: E402
import capread  # noqa: E402

PLANTS = ('names_stripped', 'combine_dropped', 'layer_merged')
SITE_QUEUE, SITE_COMBINE = 0x82C1F48C, 0x82C1F674
K_POST = pcap.K_POST
ROLE_NAMES = ['unnamed', 'scene_color', 'scene_depth', 'shadow_map', 'post_step', 'feedback_map', 'atlas_target', 'menu_picture', 'tron_picture', 'ui', 'present', 'other']
FMT_NAMES = {1: 'RGBA8', 16: 'RGBA16F'}
IMG_DT = np.dtype([('id', '<u4'), ('w', '<u2'), ('h', '<u2'), ('d', '<u2'), ('mips', 'u1'), ('layers', 'u1'), ('format', 'u1'), ('kind', 'u1'), ('usage', 'u1'), ('source', 'u1'),
                   ('flags', 'u1'), ('gf', 'u1'), ('role', 'u1'), ('role_flags', 'u1'), ('pad', 'u1'), ('pad2', 'u1', (3,))])
IREF = [('image', '<u4'), ('mip', 'u1'), ('layer', 'u1'), ('pad', '<u2')]
COPY_DT = np.dtype([('s', IREF), ('d', IREF), ('sr', '<i4', (4,)), ('dr', '<i4', (4,)), ('filt', 'u1'), ('p', 'u1', (3,))])
assert IMG_DT.itemsize == 24 and COPY_DT.itemsize == 52
SEC_COPIES, SEC_PROV, SEC_SAMPLERS = capread.SEC['copies'], capread.SEC['provenance'], capread.SEC['samplers']

# the chain's pixel shaders -> the short names used in sequences (family prefix dropped). yBlur/t2 only counts with the yToneMap/quad vertex shader (the rim map's own t2 is yBlur/masked).
CHAIN_PS = {'yPostBase/copy_depth': 'copy_depth', 'yDownScale/dof': 'dof', 'yBlur/hole_fill': 'hole_fill', 'yDownScale/box4_down': 'box4_down', 'yBlur/t2': 't2',
            'yBlur/t1_t2': 't1_t2', 'yBlur/t1': 't1', 'yBlur/m1_m2': 'm1_m2'}
CHAIN_VS = ('yToneMap/quad', 'yPostBase/uv', 'yGlareEtc/corner_uv')
STEP = ('dof', 'hole_fill', 'box4_down', 't2', 't1_t2', 't1', 'm1_m2')   # draws into the post_step targets
BLOOM_SIDE = ('box4_down', 't2', 't1_t2')                                   # bright pass + the 3-pass bloom
GLARE_SIDE = ('t1', 'm1_m2')                                                # the star glare
POSTISH_FAMILIES = ('yRetroFilm', 'yAntialiasing', 'yDefocus', 'yRadialBlur', 'yComposition', 'yToneMap', 'yMonotone')


class CensusError(Exception):
    """An input that cannot be read. Never a pass."""


def fx_registers():
    """(effect, stage, field) -> register, from src\\renderer\\shaders\\fx\\*.fx.json 'solve' blocks. Looked up, never typed."""
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


def reg(effect, field):
    r = FXR.get((effect, 'fragment', field))
    if r is None:
        raise CensusError('no register for %s %s in the effect descriptions' % (effect, field))
    return r


CONST_REGS = collections.OrderedDict((n, reg('yGlareEtc', f)) for n, f in (('c20_DOFParam', 'DOFParam'), ('c21_DOFParam2', 'DOFParam2'), ('c27_RGB', 'RGB'), ('c29_Alpha', 'Alpha'),
                                                                           ('c30_ZAdj', 'ZAdj'), ('c31_BloomScale', 'BloomScale'), ('c32_StarScale', 'StarScale')))
assert [r for r in CONST_REGS.values()] == [20, 21, 27, 29, 30, 31, 32], CONST_REGS


def monotone_registers():
    j = json.load(open(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx', 'yMonotone.fx.json'), encoding='utf-8'))
    v = next(x for x in j['fragment'] if x['name'] == 'monotone')
    return [(c['field'], c['register']) for c in v['constants']]


MONO_REGS = monotone_registers()


def table(f, sid, dt):
    if sid not in f.secs:
        return np.zeros(0, dt)
    es, cnt, off = f.secs[sid]
    if es != dt.itemsize:
        raise CensusError('frame %d: section %d element size %d in the file, %d here' % (f.idx, sid, es, dt.itemsize))
    return np.frombuffer(f.buf, dt, cnt, off)


class Cap:
    def __init__(self, path):
        self.c = pcap.Capture(path)
        self.frames = self.c.frames
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
        if not len(hashes):
            return np.array([], dtype=object)
        u, inv = np.unique(hashes, return_inverse=True)
        names = np.array([self.eff(int(x)) for x in u], dtype=object)
        return names[inv]

    def img_desc(self, i):
        r = self.imgs.get(i)
        if r is None:
            return '%d unknown' % i
        return '%d %dx%d %s %s' % (i, r['w'], r['h'], FMT_NAMES.get(int(r['format']), 'fmt%d' % int(r['format'])), ROLE_NAMES[int(r['role'])] if int(r['role']) < len(ROLE_NAMES) else 'role%d' % int(r['role']))


def mm(d, k, v):
    cur = d.get(k)
    d[k] = [v, v] if cur is None else [min(cur[0], v), max(cur[1], v)]


def filt_name(raw):
    """A sampler's 20 raw bytes -> point / linear / mixed (bytes 0 and 1 are mag and min filter, VkFilter)."""
    mag, mn = raw[0], raw[1]
    return 'point' if mag == 0 and mn == 0 else 'linear' if mag == 1 and mn == 1 else 'mixed'


def shape_of(seq, n_dof, n_restore):
    """The shape of a frame's chain from its variant sequence (names, not hashes)."""
    names = [s.split(':')[0] for s in seq]
    if 'combine' not in names:
        return 'none' if not names else 'other'
    film = 'monotone' in names or 'retro' in names
    full = 'box4_down' in names
    if film:
        return 'film' if not full and n_dof == 1 else 'other'
    if not full:
        return 'match' if n_dof == 1 and n_restore == 0 else 'other'
    if n_dof >= 2:
        return 'two_layer'
    return {0: 'editor', 1: 'entrance', 2: 'cards'}.get(n_restore, 'other')


def rle(seq):
    out = []
    for s in seq:
        if out and out[-1][0] == s:
            out[-1][1] += 1
        else:
            out.append([s, 1])
    return ' '.join(s if n == 1 else '%sx%d' % (s, n) for s, n in out)


# ---------------------------------------------------------------- the measuring ----------------------------------------------------------------------
def new_stats():
    return dict(frames=0, named=0, versions=set(), classify_bad=0, classify_bad_first=None, seq=collections.Counter(), shape=collections.Counter(), shape_frames=collections.defaultdict(list),
                post_draws={}, post_copies={}, combine_n=collections.Counter(), combine_var=collections.Counter(), combine_site=collections.Counter(), combine_slots=collections.Counter(), combine_dof=0, dof_depth=collections.Counter(), null_slots=collections.Counter(),
                layers=collections.Counter(), slot26=collections.Counter(), slot6_writer=collections.Counter(), dead=collections.Counter(), dead_draws={}, dead_copies={},
                filters=collections.defaultdict(collections.Counter), consts={}, const_distinct=collections.defaultdict(set), film_banks=set(), film_vals=collections.Counter(), targets=collections.defaultdict(set),
                dest_roles=collections.defaultdict(set), prov=collections.Counter(), readers=collections.Counter(), reader_frames=0, outputs=set(), after={}, after_sites=collections.Counter(),
                after_frames=0, post_class={}, helper={}, helper_sites=collections.Counter(), helper_frames=0, effects_seen=collections.Counter(), combine_variants_all=collections.Counter(),
                postish=collections.Counter(), pre_chain=collections.Counter(), no_chain_frames=0, restores=collections.Counter(), copies_by_src=collections.Counter(), seconds=0.0)


def measure(cap, frames, plant):
    S = new_stats()
    t0 = time.time()
    for fi in frames:
        f = cap.frame(fi)
        S['frames'] += 1
        S['versions'].add(f.version)
        named = f.has_origins and plant != 'names_stripped'
        S['named'] += bool(named)
        do_frame(cap, f, fi, S, named, plant)
    S['seconds'] = time.time() - t0
    return S


def do_frame(cap, f, fi, S, named, plant):
    d = f.draws
    nd = len(d)
    vs_eff, ps_eff = cap.effs(d['vs_hash']), cap.effs(d['ps_hash'])
    copies = table(f, SEC_COPIES, COPY_DT)
    provs = table(f, SEC_PROV, capread.PROV_DT)
    imgs = cap.imgs
    smp = f.raw[SEC_SAMPLERS]
    # ---- every effect family seen in the frame's pixel shaders that is post-ish but not a chain draw (the not-seen list)
    cs = pcap.queue_site(f.origins) if named else None   # S101 lane PN: the generic queue site, whether or not a post effect scope renamed call_site
    # ---- the two classifications
    by_eff = np.array([vs_eff[i] in CHAIN_VS and (ps_eff[i] in CHAIN_PS or ps_eff[i].startswith(('yGlareEtc/', 'yMonotone/', 'yRetroFilm/'))) for i in range(nd)], dtype=bool) if nd else np.zeros(0, bool)
    if named:
        by_name = ((cs == SITE_QUEUE) & (ps_eff != 'yBlur/flag_any')) | (cs == SITE_COMBINE)
        if (by_name != by_eff).any():
            S['classify_bad'] += 1
            if S['classify_bad_first'] is None:
                i = int(np.nonzero(by_name != by_eff)[0][0])
                S['classify_bad_first'] = 'frame %d draw %d: name %s, effect %s (%s / %s, site %#x)' % (fi, i, bool(by_name[i]), bool(by_eff[i]), vs_eff[i], ps_eff[i], int(cs[i]))
        chain = by_name
    else:
        chain = by_eff
    # ---- the walk: ops in order, with the target at each draw and copy
    cur_t = None  # (colour image, depth image)
    label = {}    # image -> 'variant#layer' of the last chain draw into it (copied along), 'draw' for any other draw, absent = 'ext'
    seq, rec = [], []   # seq: variant names in order; rec: one dict per chain draw
    ncopy, n_dof, n_restore = 0, 0, 0
    chain_copies = []   # (op position, src, dst)
    comb_pos = None
    step_targets = set()
    draw_op = {}
    for k, (kind, idx) in enumerate(f.ops):
        kind, idx = int(kind), int(idx)
        if kind == 0:
            t = f.targets[idx]
            cur_t = int(t['color'][0]['image']) if t['count'] else int(t['depth']['image'])
        elif kind == 2:
            draw_op[idx] = k
            if not chain[idx]:
                if cur_t is not None:
                    label[cur_t] = 'draw'
                continue
            pse = ps_eff[idx]
            if pse in CHAIN_PS:
                v = CHAIN_PS[pse]
            elif pse.startswith('yGlareEtc/'):
                v = 'combine'
            elif pse.startswith('yMonotone/'):
                v = 'monotone'
            else:
                v = 'retro'
            if v == 'combine' and plant == 'combine_dropped':
                continue
            if v == 'dof':
                n_dof += 1
            if v == 'copy_depth':
                n_restore += 1
            layer = n_dof
            seq.append(v)
            # slots
            tf, tc = int(d['tex_first'][idx]), int(d['tex_count'][idx])
            slots = []
            for s in f.slots[tf:tf + tc]:
                sid = int(s['sampler'])
                if sid >= len(smp):
                    raise CensusError('frame %d: draw %d names sampler %d, the frame holds %d' % (fi, idx, sid, len(smp)))
                slots.append((int(s['slot']), int(s['image']), filt_name(smp[sid])))
            rec.append(dict(v=v, idx=idx, op=k, target=cur_t, layer=layer, slots=slots, ps=pse, labels={sl: label.get(im, 'ext') for sl, im, _ in slots}))
            if v in STEP:
                step_targets.add(cur_t)
            if v == 'combine':
                comb_pos = k
            label[cur_t] = '%s#%d' % (v, layer)
        elif kind == 3:
            c = copies[idx]
            src, dst = int(c['s']['image']), int(c['d']['image'])
            chain_copies.append((k, src, dst))
            label[dst] = label.get(src, 'ext')
            ncopy += 1
    nchain = len(rec)
    S['seq'][rle(seq)] += 1
    shape = shape_of(seq, n_dof, n_restore)
    S['shape'][shape] += 1
    if len(S['shape_frames'][shape]) < 3:
        S['shape_frames'][shape].append(fi)
    if not nchain:
        S['no_chain_frames'] += 1
        return
    mm(S['post_draws'], 'all', nchain)
    # ---- combine
    comb = [r for r in rec if r['v'] == 'combine']
    S['combine_n'][len(comb)] += 1
    # ---- copies: only those whose source is a chain draw's target (the chain's own copies)
    chain_targets = {r['target'] for r in rec}
    own_copies = [(k, s, t) for k, s, t in chain_copies if s in chain_targets]
    mm(S['post_copies'], 'all', len(own_copies))
    mm(S['post_copies'], 'from_step_targets', sum(1 for k, s_, t in own_copies if s_ in step_targets))
    mm(S['post_copies'], 'from_scratch_and_scene', sum(1 for k, s_, t in own_copies if s_ not in step_targets))
    for k, s, t in own_copies:
        S['copies_by_src'][s] += 1
    out_imgs = set(step_targets)
    for k, s, t in own_copies:
        if s in step_targets:
            out_imgs.add(t)
    S['outputs'] |= out_imgs
    for r in rec:
        S['targets'][r['v']].add(cap.img_desc(r['target']))
    for k, s, t in own_copies:
        S['dest_roles']['%s' % cap.img_desc(t)].add(cap.img_desc(s))
    # ---- provenance of the images the chain's copies make
    pv = {int(r['image']): r for r in provs}
    for k, s, t in own_copies:
        r = pv.get(t)
        if r is None:
            what = 'absent'
        elif int(r['source']) != 2:
            what = 'not_gpu'
        elif int(r['source_image']) == s:
            what = 'gpu_copy_of_source'
        elif int(r['source_image']) == 0:
            what = 'gpu_source_image_unfilled'
        else:
            what = 'gpu_source_image_wrong'
        S['prov'][what] += 1
    # ---- combine details
    for r in comb:
        var = r['ps'].split('/')[1]
        S['combine_var'][var] += 1
        S['combine_dof'] += 'dof' in var
        if named:
            S['combine_site'][hex(int(cs[r['idx']]))] += 1
        slots = r['slots']
        if plant == 'layer_merged':
            s2 = [x for x in slots if x[0] == 2]
            slots = [(sl, (s2[0][1] if sl == 6 and s2 else im), fl) for sl, im, fl in slots]
            r['labels'] = {sl: (r['labels'].get(2, 'ext') if sl == 6 else lb) for sl, lb in r['labels'].items()}
        S['combine_slots'][' '.join('s%d' % sl for sl, _, _ in slots)] += 1
        im = {sl: i for sl, i, _ in slots}
        if 2 in im and 6 in im:
            rel = 'same' if im[2] == im[6] else 'different'
            S['slot26'][rel] += 1
            if rel == 'different':
                S['slot6_writer'][r['labels'].get(6, 'ext').split('#')[0] + ' <- slot2 ' + r['labels'].get(2, 'ext').split('#')[0]] += 1
                S['slot6_writer']['layers ' + r['labels'].get(6, 'ext').split('#')[1] + ' vs ' + r['labels'].get(2, 'ext').split('#')[1] if '#' in r['labels'].get(6, '') and '#' in r['labels'].get(2, '') else 'layers ?'] += 1
        # constants
        bank = f.bank(int(d['ps_bank'][r['idx']]))
        fl = bank.view('<f4')
        for nm, rg in CONST_REGS.items():
            if nm.startswith(('c20', 'c21')) and 'dof' not in var:
                continue
            if nm.startswith(('c27', 'c29', 'c30')) and 'fog' not in var:
                continue
            if nm.startswith(('c31', 'c32')) and 'bloom' not in var:
                continue
            v4 = fl[4 * rg:4 * rg + 4].astype(np.float64)
            cur = S['consts'].get(nm)
            S['consts'][nm] = [v4.copy(), v4.copy()] if cur is None else [np.minimum(cur[0], v4), np.maximum(cur[1], v4)]
            S['const_distinct'][nm].add(bank[4 * rg:4 * rg + 4].tobytes())
        # dead work: bloom / glare at zero strength
        if 'bloom' in var:
            bz = bool(fl[4 * CONST_REGS['c31_BloomScale']] == 0)
            gz = bool(fl[4 * CONST_REGS['c32_StarScale']] == 0)
            S['dead']['bloom_zero=%d glare_zero=%d' % (bz, gz)] += 1
            nb = sum(1 for x in rec if x['v'] in BLOOM_SIDE)
            ng = sum(1 for x in rec if x['v'] in GLARE_SIDE)
            bt = {x['target'] for x in rec if x['v'] in BLOOM_SIDE}
            gt = {x['target'] for x in rec if x['v'] in GLARE_SIDE}
            cb = sum(1 for k, s, t in own_copies if s in bt)
            cg = sum(1 for k, s, t in own_copies if s in gt)
            if bz:
                mm(S['dead_draws'], 'bloom', nb)
                mm(S['dead_copies'], 'bloom', cb)
            if gz:
                mm(S['dead_draws'], 'glare', ng)
                mm(S['dead_copies'], 'glare', cg)
            if bz and gz:
                mm(S['dead_draws'], 'both', nb + ng)
                mm(S['dead_copies'], 'both', cb + cg)
            mm(S['dead_draws'], 'bloom_side_drawn', nb)
            mm(S['dead_draws'], 'glare_side_drawn', ng)
            mm(S['dead_copies'], 'bloom_side_copies', cb)
            mm(S['dead_copies'], 'glare_side_copies', cg)
    # ---- filters per (variant, slot)
    for r in rec:
        for sl, im, fl in r['slots']:
            S['filters']['%s.s%d' % (r['v'], sl)][fl] += 1
            if im == 0:
                S['null_slots']['%s.s%d' % (r['v'], sl)] += 1
    # ---- the dof depth sampler against the frame's shape (the filter rule is [U] in the map: this measures what goes with it)
    for r in rec:
        if r['v'] == 'dof':
            s1 = [(im, fl) for sl, im, fl in r['slots'] if sl == 1]
            if s1 and s1[0][0]:
                im1 = imgs.get(s1[0][0])
                fmt = FMT_NAMES.get(int(im1['format']), 'fmt%d' % int(im1['format'])) if im1 is not None else 'unknown'
                S['dof_depth']['%s | restores in frame %d | image written by %s | %s' % (s1[0][1], n_restore, r['labels'].get(1, 'ext').split('#')[0], fmt)] += 1
            else:
                S['dof_depth']['%s | image 0 (a fetch of nothing)' % (s1[0][1] if s1 else 'no slot 1')] += 1
    # ---- monotone bank identical?
    for r in rec:
        if r['v'] == 'monotone':
            bk = f.bank(int(d['ps_bank'][r['idx']]))
            S['film_banks'].add(b''.join(bk[4 * rg:4 * rg + 4].tobytes() for _, rg in MONO_REGS))
            S['film_vals'].update('%s=%s' % (nm, [r5(x) for x in bk.view('<f4')[4 * rg:4 * rg + 4]]) for nm, rg in MONO_REGS)
    # ---- readers of the chain's outputs outside the chain
    tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
    sl_img = f.slots['image'].astype(np.int64)
    if len(sl_img) and out_imgs:
        in_out = np.isin(sl_img, np.array(sorted(out_imgs), dtype=np.int64))
        owner = np.repeat(np.arange(nd), tc)
        if len(owner) != len(sl_img):
            raise CensusError('frame %d: texture slots not laid out per draw' % fi)
        hit = np.unique(owner[in_out]) if in_out.any() else np.zeros(0, np.int64)
        bad = False
        rec_v = {r['idx']: r['v'] for r in rec}
        for i in hit:
            if int(i) in rec_v:
                S['readers']['chain:' + rec_v[int(i)]] += 1
            else:
                S['readers']['OUTSIDE:%s/%s' % (vs_eff[int(i)], ps_eff[int(i)])] += 1
                bad = True
        S['reader_frames'] += bad
    # ---- draws after the combine, and the post-class split
    if comb:
        cop = comb[-1]['op']
        later = [i for i in range(nd) if draw_op.get(i, -1) > cop and not chain[i]]
        mm(S['after'], 'all', len(later))
        S['after_frames'] += 1
        if named:
            for i in later:
                S['after_sites'][hex(int(cs[i]))] += 1
        else:
            for i in later:
                S['after_sites']['vs ' + vs_eff[i]] += 1
        earlier_ch = [r for r in rec if r['op'] > cop]
        mm(S['after'], 'chain_after_combine', len(earlier_ch))
    # draws before the first chain draw that belong to post-ish families (yComposition ...)
    first_op = rec[0]['op']
    for i in range(nd):
        if draw_op.get(i, 1 << 30) < first_op and ps_eff[i].startswith('yComposition'):
            S['pre_chain']['yComposition'] += 1
    if named:
        kl = f.origins['klass'].astype(np.int64)
        post_cls = np.nonzero((kl == K_POST) | ((kl == pcap.K_SHADOW) & np.isin(cs, pcap.GENERIC_QUEUE_SITES)))[0]   # S101 lane PN: the shadow reductions queue through the same helper and are class shadow now
        mm(S['post_class'], 'all', len(post_cls))
        helpers = [int(i) for i in post_cls if not chain[int(i)]]
        mm(S['post_class'], 'chain', len(post_cls) - len(helpers))
        mm(S['post_class'], 'helpers', len(helpers))
        S['helper_frames'] += 1
        for i in helpers:
            S['helper_sites']['%s %s' % (hex(int(cs[i])), ps_eff[i])] += 1
    # ---- what is in the frame that the chain does not use
    for i in range(nd):
        e = ps_eff[i]
        fam = e.split('/')[0]
        if fam in POSTISH_FAMILIES and not chain[i]:
            S['postish'][e] += 1
        if fam in ('yRetroFilm',):
            S['effects_seen']['yRetroFilm draws'] += 1
    S['combine_variants_all'].update(set(r['ps'].split('/')[1] for r in comb))
    for r in rec:
        if r['v'] == 'copy_depth':
            S['restores'][fi] += 1
    for v in ('dof',):
        mm(S['post_draws'], 'dof_layers', n_dof)
    mm(S['post_draws'], 'restores', n_restore)
    S['layers'][n_dof] += 1


# ---------------------------------------------------------------- the numbers ------------------------------------------------------------------------
def r5(x):
    x = float(x)
    if x != x:
        return 'nan'
    if x in (float('inf'), float('-inf')):
        return 'inf' if x > 0 else '-inf'
    return round(x, 5)


def plain(x):
    if isinstance(x, dict):
        return {str(k): plain(v) for k, v in sorted(x.items(), key=lambda kv: str(kv[0]))}
    if isinstance(x, (set, frozenset)):
        return sorted(plain(v) for v in x)
    if isinstance(x, (list, tuple)):
        return [plain(v) for v in x]
    if isinstance(x, np.generic):
        return x.item()
    return x


def filt_summary(c):
    return next(iter(c)) if len(c) == 1 else dict(sorted(c.items()))


def summarize(S):
    n = {}
    n['frames'] = S['frames']
    n['named_frames'] = S['named']
    n['sequences'] = dict(sorted(S['seq'].items()))
    n['shapes'] = dict(sorted(S['shape'].items()))
    n['shape_first_frames'] = {k: v for k, v in sorted(S['shape_frames'].items())}
    n['post_draws'] = S['post_draws']
    n['post_copies'] = S['post_copies']
    n['combine_per_frame'] = dict(sorted(S['combine_n'].items()))
    n['combine_variants'] = dict(sorted(S['combine_var'].items()))
    n['combine_call_sites'] = dict(sorted(S['combine_site'].items()))
    n['combine_slots'] = dict(sorted(S['combine_slots'].items()))
    n['dof_layers'] = dict(sorted(S['layers'].items()))
    n['slot2_vs_slot6'] = dict(sorted(S['slot26'].items()))
    n['slot6_source'] = dict(sorted(S['slot6_writer'].items()))
    n['dead_work_flags'] = dict(sorted(S['dead'].items()))
    n['dead_draws'] = S['dead_draws']
    n['dead_copies'] = S['dead_copies']
    n['dof_depth'] = dict(sorted(S['dof_depth'].items()))
    n['filters'] = {k: filt_summary(v) for k, v in sorted(S['filters'].items())}
    n['constants'] = {k: {'min': [r5(x) for x in v[0]], 'max': [r5(x) for x in v[1]], 'distinct': len(S['const_distinct'][k])} for k, v in S['consts'].items()}
    n['monotone_distinct_banks'] = len(S['film_banks'])
    n['monotone_values'] = dict(sorted(S['film_vals'].items()))
    n['targets'] = {k: sorted(v) for k, v in sorted(S['targets'].items())}
    n['copy_destinations'] = {k: sorted(v) for k, v in sorted(S['dest_roles'].items())}
    n['provenance'] = dict(sorted(S['prov'].items()))
    n['readers'] = dict(sorted(S['readers'].items()))
    n['null_slots'] = dict(sorted(S['null_slots'].items()))
    n['outputs'] = sorted(S['outputs'])
    n['after_combine'] = S['after']
    n['after_combine_sites'] = dict(sorted(S['after_sites'].items(), key=lambda kv: (-kv[1], kv[0]))[:12])
    n['post_class'] = S['post_class']
    n['helper_sites'] = dict(sorted(S['helper_sites'].items()))
    n['postish_not_chain'] = dict(sorted(S['postish'].items()))
    n['pre_chain'] = dict(S['pre_chain'])
    n['combine_variants_seen'] = dict(sorted(S['combine_variants_all'].items()))
    n['no_chain_frames'] = S['no_chain_frames']
    n['retro_draws'] = S['effects_seen'].get('yRetroFilm draws', 0)
    return n


class Checks:
    def __init__(self, quiet):
        self.rows, self.quiet = [], quiet

    def add(self, label, name, ok, detail):
        self.rows.append((label, name, bool(ok), detail))
        if not self.quiet or not ok:
            print('%s  %-8s %-14s %s' % ('PASS' if ok else 'FAIL', label, name, detail))


def evaluate(label, S, E, ck, partial, plant):
    n = summarize(S)
    named_all = S['named'] == S['frames']
    expect_named = E.get('named', True)
    ck.add(label, 'names', (named_all if expect_named else S['named'] == 0) and S['frames'] > 0,
           '%d of %d frames carry origins (expected %s; a capture without origins is classified by effect and says so)' % (S['named'], S['frames'], 'all' if expect_named else 'none'))
    if S['named']:
        ck.add(label, 'classify', S['classify_bad'] == 0, 'post draws by call site / PS and by vertex + pixel effect agree on every named frame%s' % ('' if not S['classify_bad'] else ': ' + str(S['classify_bad_first'])))
    else:
        ck.add(label, 'classify', True, 'unnamed: classified by effect only (call sites unknowable)')
    sh = S['shape']
    ck.add(label, 'shapes', S['frames'] > 0 and not sh.get('none') and not sh.get('other') and sum(sh.values()) == S['frames'],
           'shapes %s (every frame has a known chain shape; none / other = a shape PO1 would refuse: %s)' % (dict(sh), dict(S['seq']) if sh.get('other') else 'none'))
    cn = S['combine_n']
    ck.add(label, 'combine', set(cn) == {1} and sum(cn.values()) == S['frames'],
           'exactly one combine per frame: draws per frame %s, variants %s, call sites %s' % (dict(cn), dict(S['combine_var']), dict(S['combine_site']) or 'unnamed'))
    ly = S['layers']
    s26 = S['slot26']
    bad_layers = False
    if ly.get(2, 0) or s26.get('different', 0):
        # a two-layer frame has two dof draws AND a combine whose slot 6 is another image than slot 2, made by a hole_fill copy of the earlier layer
        for nm in S['slot6_writer']:
            if nm.startswith('layers') and nm != 'layers 1 vs 2':
                bad_layers = True
            if not nm.startswith('layers') and not nm.startswith('hole_fill <- slot2 hole_fill'):
                bad_layers = True
        if ly.get(2, 0) != s26.get('different', 0):
            bad_layers = True
    else:
        bad_layers = sum(1 for x in ly if x >= 2) != 0
    ck.add(label, 'layers', not bad_layers and s26.get('same', 0) + s26.get('different', 0) == S['combine_dof'],
           'dof layers per frame %s; combine slot 2 vs slot 6: %s; slot 6 written by: %s' % (dict(ly), dict(s26), dict(S['slot6_writer']) or 'n/a'))
    dz = S['dead']
    ck.add(label, 'dead_work', True, 'combine bloom variants (bloom strength zero, glare strength zero): %s; draws %s, copies %s' % (dict(dz) or 'no bloom combine', S['dead_draws'], S['dead_copies']))
    ck.add(label, 'filters', bool(S['filters']), 'dof depth (slot 1): %s; dof colour (slot 0): %s' % (filt_summary(S['filters'].get('dof.s1', collections.Counter({'n/a': 1}))), filt_summary(S['filters'].get('dof.s0', collections.Counter({'n/a': 1})))))
    ck.add(label, 'inputs', True, 'chain slots that fetch image 0 (a fetch of nothing, reads 0): %s' % (dict(S['null_slots']) or 'none'))
    ck.add(label, 'constants', bool(S['consts']) or True, ', '.join('%s %d distinct' % (k, len(v)) for k, v in sorted(S['const_distinct'].items())) or 'no combine constants')
    ck.add(label, 'targets', bool(S['targets']), '%d chain pass kinds, %d chain output images' % (len(S['targets']), len(S['outputs'])))
    outside = {k: v for k, v in S['readers'].items() if k.startswith('OUTSIDE')}
    chain_readers = {k: v for k, v in S['readers'].items() if k.startswith('chain:')}
    ck.add(label, 'readers', not outside and bool(chain_readers), 'the chain\'s outputs are read by chain draws only: %s; outside the chain: %s' % (dict(chain_readers), dict(outside) or 'none'))
    af = S['after']
    ck.add(label, 'after_combine', S['after_frames'] == S['frames'] or partial, 'draws after the combine (not chain): %s, chain draws after it: %s; sites %s' % (af.get('all'), af.get('chain_after_combine'), dict(list(sorted(S['after_sites'].items(), key=lambda kv: -kv[1]))[:4])))
    if S['named']:
        pc = S['post_class']
        ck.add(label, 'split', S['helper_frames'] == S['named'], 'class-post draws per frame %s = chain %s + helpers %s; helper sites %s' % (pc.get('all'), pc.get('chain'), pc.get('helpers'), dict(S['helper_sites'])))
    else:
        ck.add(label, 'split', True, 'unnamed: the class split needs origins')
    ck.add(label, 'notseen', True, 'yRetroFilm draws %d; combine variants seen %s; other post-family draws outside the chain %s' % (n['retro_draws'], dict(S['combine_variants_all']), dict(S['postish']) or 'none'))
    if partial or plant == 'names_stripped':
        return n
    if not E:
        ck.add(label, 'expect', False, 'no expect.json entry for this capture (pass --frames for a partial look)')
        return n
    groups = {
        'shapes': ['frames', 'named_frames', 'shapes', 'sequences', 'post_draws', 'post_copies'],
        'combine': ['combine_per_frame', 'combine_variants', 'combine_call_sites', 'combine_slots', 'combine_variants_seen'],
        'layers': ['dof_layers', 'slot2_vs_slot6', 'slot6_source'],
        'dead_work': ['dead_work_flags', 'dead_draws', 'dead_copies'],
        'filters': ['filters', 'dof_depth'],
        'constants': ['constants', 'monotone_distinct_banks', 'monotone_values'],
        'targets': ['targets', 'copy_destinations', 'provenance', 'outputs'],
        'readers': ['readers'],
        'inputs': ['null_slots'],
        'after_combine': ['after_combine', 'after_combine_sites'],
        'split': ['post_class', 'helper_sites'],
        'notseen': ['postish_not_chain', 'pre_chain', 'retro_draws', 'no_chain_frames'],
    }
    for gname, keys in groups.items():
        diffs = []
        for k in keys:
            if plain(E.get(k)) != plain(n.get(k)):
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
        t0 = time.time()
        try:
            cap = Cap(path)
            if a.frames:
                lo, hi, step = (int(x) if x else None for x in (a.frames.split(':') + ['', ''])[:3])
                frames = list(range(lo or 0, hi if hi is not None else len(cap.frames), step or 1))
            else:
                frames = list(range(len(cap.frames)))
            S = measure(cap, frames, a.plant)
        except (pcap.PError, CensusError, OSError, KeyError, IndexError, struct.error) as e:
            print('FAIL %-8s input         cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        obs = evaluate(label, S, E, ck, bool(a.frames), a.plant)
        emitted[label] = dict(obs, named=bool(S['named'] == S['frames']))
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
