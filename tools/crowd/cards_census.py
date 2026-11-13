r"""Lane CC (S101, docs\renderer\CROWD_CARDS_PROMOTION_MAP.md section 6 lane CC): the cards census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's section 1 on every frame of case zips / capture folders: the card quad, the per-card constants (c76-c78 only), the fixed
state, the counts per frame, the contiguity and the type order, the type -> picture binding, the bake (20 x 64x64 targets + copies, the orthographic camera, the light set), the light-map
page (slots 13 / 16) and, where the capture carries scene records, the pairing of the record's cards with the stream's draws (record j-1) and the ORDER question of the map's C1:
does the record list a type's cards in the same order the stream draws them?

  py -I tools\crowd\cards_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--expect FILE] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--emit-expect OUT] [--dump] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder (relative paths are read from the main checkout). LABEL names the capture's entry in cards_expect.json
(brock = Check B, F = Check F, bryan, wm30, s100cards). Exit 1 when any check FAILs or an input cannot be read.
Checks (PASS / FAIL with counts):
  names       every frame carries its draw origins where cards_expect.json says it must (a version-1 capture fails loudly); the origin-keyed and the effect-keyed card draws are the same set.
  counts      cards / bakes / light-map sprites per frame equal cards_expect.json.
  stream      the card draws of a frame are one contiguous run of ops, follow the last stands block draw, and are grouped by type in ascending picture order.
  program     one (vs, ps) pair, lm_uv + lm_pixel_color_alpha, the hashes of the effect descriptions.
  quad        ONE vertex blob in the whole capture: 4 vertices stride 16 (pos float3, uv half2), 4 indices, source prim 6, the measured positions / uvs.
  registers   the vs-bank registers that differ between the cards of a frame are exactly c76-c78 (the ps bank: none); per-frame ViewProj = the blocks' (bit-equal); c184 constant.
  state       one fixed state in the whole capture, one dynamic state per frame (= the blocks'); the decoded depth / blend / alpha-test / cull values.
  textures    slot 0 = the type's 64 x 64 picture (a copy destination), slot 13 = the light-map page (512 x 512), the sampler, the slot swizzle.
  bake        20 passes of 64 x 64 (one draw, a clear, one copy to the type's own picture), ascending, the orthographic camera, ONE light set per frame, the plane mask before, the page after.
  readers     the draws that read the 20 pictures are the card draws and nothing else.
  page        the light-map page: one 512 x 512 pass of ySprite draws copied to the page image that slot 13 (cards, blocks) and slot 16 (bakes) read.
  pairing     (captures with scene records) record j-1's cards == the stream's card draws of frame j: world (c76-c78, bit-exact) 1:1.
  order       the record lists a type's cards in the order the stream draws them (the map's C1 question), and the types ascend in both.
  binding     the type -> picture binding: every card's slot-0 image = the destination of bake T (T = the record's type), and bake T's slot-0 texture = the texture the stands blocks of type T bind.
  expect      every measured group against cards_expect.json.
--plant (tests only): type_shift (the binding is taken for type T + 1: lane 35's wrong-texture class), order_swap (two cards of a type are swapped in the stream order), names_stripped (origins
dropped = a version-1 capture; must fail loudly), record_offset (record j instead of j-1), one_picture (every card expected to bind the first card's picture: lane 35's one texture for every
member), atlas_picture (the 512 x 512 light-map page taken for the cards' picture: the 'atlas' lead).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
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
sys.path.insert(0, os.path.join(ROOT, 'tools', 'particles'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import pcap  # noqa: E402
import capread  # noqa: E402

PLANTS = ('type_shift', 'order_swap', 'names_stripped', 'record_offset', 'one_picture', 'atlas_picture')
SITE_CARD, SITE_BLOCK, SITE_BAKE_PASS = 0x825130C4, 0x82518574, 0x82C01F60
SEC_FIXED, SEC_DYN, SEC_SAMPLER, SEC_COPIES = pcap.SEC['fixeds'], pcap.SEC['dynamics'], pcap.SEC['samplers'], capread.SEC['copies']
EFF_CARD_VS, EFF_BAKE_VS, EFF_PLANE = 'yAudience/lm_uv', 'yAudDefault/skinned_lit_lm_vertex', 'yPlaneShadowMap'
IMG_DT = np.dtype([('id', '<u4'), ('w', '<u2'), ('h', '<u2'), ('d', '<u2'), ('mips', 'u1'), ('layers', 'u1'), ('format', 'u1'), ('kind', 'u1'), ('usage', 'u1'), ('source', 'u1'),
                   ('flags', 'u1'), ('gf', 'u1'), ('role', 'u1'), ('role_flags', 'u1'), ('pad', 'u1'), ('pad2', 'u1', (3,))])
IREF = [('image', '<u4'), ('mip', 'u1'), ('layer', 'u1'), ('pad', '<u2')]
COPY_DT = np.dtype([('s', IREF), ('d', IREF), ('sr', '<i4', (4,)), ('dr', '<i4', (4,)), ('filt', 'u1'), ('p', 'u1', (3,))])
assert IMG_DT.itemsize == 24 and COPY_DT.itemsize == 52
OBJECT_SIZE = 124  # scene_record.h Object, asserted against the record's own element_size


class CensusError(Exception):
    """An input that cannot be read. Never a pass."""


# ---------------------------------------------------------------- the shader registers (looked up in the committed effect descriptions) ------------
def fx_registers():
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


R_WORLD = reg('yAudience', 'vertex', 'WorldMatrix')           # 76: c76-c78 (3 rows)
R_VP = reg('yAudience', 'vertex', 'ViewProj')                 # 56: c56-c59
R_LMP = reg('yAudience', 'vertex', 'LightMapParam')           # 184
R_LMP_PS = reg('yAudience', 'fragment', 'LightMapParam')      # 184
LIGHT_REGS = [reg('yAudDefault', 'vertex', f) for f in ('DifLghCol', 'DifLghVec', 'SkyDir', 'SkyCol', 'GroundCol', 'MatAmbCol', 'MatDifCol', 'LightMin')]
LIGHT_SPAN = {'DifLghCol': 4, 'DifLghVec': 4, 'SkyDir': 1, 'SkyCol': 1, 'GroundCol': 1, 'MatAmbCol': 1, 'MatDifCol': 1, 'LightMin': 1}  # c0-c3 / c8-c11 (4 lights), the rest one register each
LIGHT_SLICES = [(reg('yAudDefault', 'vertex', f), n) for f, n in LIGHT_SPAN.items()]
CARD_VS_HASH = ['%016X' % int(h, 16) for h in ('D734762D2180F0F5',)]
CARD_PS_NAME = 'yAudience/lm_pixel_color_alpha'


def words(bank, r, n=1):
    return bank[4 * r:4 * (r + n)]


def fl(bank, r, n=1):
    return bank[4 * r:4 * (r + n)].view('<f4')


# ---------------------------------------------------------------- the capture side -------------------------------------------------------------------
class Cap:
    def __init__(self, path):
        self.c = pcap.Capture(path)
        self.frames = self.c.frames
        self.path = path
        self.nums = [int(re.search(r'frame_(\d+)', os.path.basename(n.replace('\\', '/'))).group(1)) for n in self.frames]
        name = next((n for n in self.c.zf.namelist() if n.endswith('images.bin')), None) if self.c.zf is not None else os.path.join(path, 'images.bin')
        if not name:
            raise CensusError('%s: no images.bin' % path)
        raw = self.c.read(name)
        magic, ver, count, _ = struct.unpack_from('<IIII', raw, 0)
        if len(raw) < 16 + 24 * count:
            raise CensusError('%s: images.bin does not hold %d records of 24 bytes' % (path, count))
        self.imgs = {int(r['id']): r for r in np.frombuffer(raw, IMG_DT, count, 16)}
        self.geo = self.c.geo
        self._eff = {}
        self.scene = {}
        if self.c.zf is not None:
            for n in self.c.zf.namelist():
                m = re.search(r'(^|/)scene/frame_(\d+)\.rsc$', n)
                if m:
                    self.scene[int(m.group(2))] = n
        else:
            sd = os.path.join(path, 'scene')
            if os.path.isdir(sd):
                for fn in os.listdir(sd):
                    m = re.match(r'frame_(\d+)\.rsc$', fn)
                    if m:
                        self.scene[int(m.group(1))] = os.path.join(sd, fn)

    def frame(self, i):
        return self.c.frame(i)

    def record(self, num):
        n = self.scene.get(num)
        return None if n is None else self.c.read(n)

    def eff(self, h):
        e = self._eff.get(h)
        if e is None:
            e = self._eff[h] = pcap.effect_of(h)
        return e

    def effs(self, hashes):
        u, inv = np.unique(hashes, return_inverse=True)
        names = np.array([self.eff(int(x)) for x in u], dtype=object)
        return names[inv]

    def wh(self, img):
        r = self.imgs.get(int(img))
        return None if r is None else (int(r['w']), int(r['h']))


def table(f, sid, dt):
    if sid not in f.secs:
        return np.zeros(0, dt)
    es, cnt, off = f.secs[sid]
    if es != dt.itemsize:
        raise CensusError('frame %d: section %d element size %d in the file, %d here' % (f.idx, sid, es, dt.itemsize))
    return np.frombuffer(f.buf, dt, cnt, off)


def parse_record(raw):
    """-> (cards, blocks): lists of (type, member, key) in the record's object order; key = the 12 words of c76-c78 as the stream holds them (rows c76..c78, translation in the 4th column)."""
    magic, ver, nsec = struct.unpack_from('<III', raw, 0)
    if magic != 0x4E435352 or ver != 1:
        raise CensusError('not a scene record v1')
    secs = {}
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', raw, 40 + 24 * i)
        secs[sid] = (es, cnt, off)
    if 1 not in secs or 4 not in secs:
        raise CensusError('scene record without strings / objects')
    st = raw[secs[1][2]:secs[1][2] + secs[1][1]]
    es, cnt, off = secs[4]
    if es != OBJECT_SIZE:
        raise CensusError('scene record Object element size %d, this reader reads %d' % (es, OBJECT_SIZE))
    cards, blocks = [], []
    for i in range(cnt):
        b = raw[off + es * i:off + es * (i + 1)]
        o = struct.unpack_from('<I', b, 12)[0]
        if not o:
            continue
        nm = st[o - 1:st.index(b'\0', o - 1)]
        if nm.startswith(b'crowd:card:') or nm.startswith(b'crowd:block:'):
            w = np.frombuffer(b, '<u4', 16, 16)
            key = struct.pack('<12I', *[int(w[col * 4 + r]) for r in range(3) for col in range(4)])
            p = nm.split(b':')
            if p[1] == b'card':
                cards.append((int(p[2]), int(p[3]), key))
            else:
                blocks.append((int(p[2]), int(p[4]), key))
    return cards, blocks


def decode_fixed(raw):
    """DrawFixed (adapter_frame.h, 72 bytes): rdr::DrawState (api.h) then the alpha test."""
    b = np.frombuffer(raw, 'u1')
    blend = [[int(x) for x in b[24 + 8 * k:32 + 8 * k]] for k in range(4)]
    return dict(topology=int(b[4]), cull=int(b[5]), front_face=int(b[6]), depth_clamp=int(b[7]), depth_test=int(b[8]), depth_write=int(b[9]), depth_compare=int(b[10]),
                stencil_test=int(b[11]), depth_bias=int(b[12]), primitive_restart=int(b[13]), blend=blend, alpha_test=int(b[56]), alpha_func=int(b[57]),
                alpha_to_coverage=int(b[58]), clip_off=int(b[59]), alpha_ref=float(np.frombuffer(raw, '<f4', 1, 64)[0]))


def decode_dyn(raw):
    v = np.frombuffer(raw, '<f4', 6, 0)
    return dict(viewport=[float(x) for x in v], flags=int(np.frombuffer(raw, '<u4', 1, 184)[0]))


def sampler_name(raw):
    mag, mn, mip, au, av, aw, aniso, border = raw[:8]
    lod = np.frombuffer(raw, '<f4', 3, 8)
    return dict(mag=mag, min=mn, mip=mip, u=au, v=av, w=aw, aniso=aniso, border=border, lod=[float(x) for x in lod])


class Pass:
    __slots__ = ('target', 'size', 'pos', 'draws', 'copies', 'clear')

    def __init__(self, target, size, pos):
        self.target, self.size, self.pos = target, size, pos
        self.draws, self.copies, self.clear = [], [], False


def walk(f, copies, cap):
    passes, cur = [], None
    for k, (kind, idx) in enumerate(f.ops):
        kind, idx = int(kind), int(idx)
        if kind == 0:
            t = f.targets[idx]
            img = int(t['color'][0]['image']) if t['count'] else int(t['depth']['image'])
            cur = Pass(img, cap.wh(img), k)
            passes.append(cur)
        elif cur is None:
            continue
        elif kind == 1:
            cur.clear = True
        elif kind == 2:
            cur.draws.append((k, idx))
        elif kind == 3:
            cur.copies.append((k, idx))
    return passes


def sig(*a):
    return repr(a)


def measure(cap, frames, plant=None):
    m = collections.Counter()
    S = collections.defaultdict(set)       # sets of facts
    C = collections.defaultdict(collections.Counter)
    per_frame = collections.defaultdict(list)
    vp_names = {}
    for fi in frames:
        f = cap.frame(fi)
        num = cap.nums[fi]
        m['frames'] += 1
        D = f.draws
        n = len(D)
        named = f.has_origins and plant != 'names_stripped'
        m['frames_named'] += int(named)
        veff = cap.effs(D['vs_hash']) if n else np.array([], dtype=object)
        peff = cap.effs(D['ps_hash']) if n else np.array([], dtype=object)
        eff_card = (veff == EFF_CARD_VS) & (D['index_count'] == 4)
        if named:
            site = f.origins['call_site']
            card_mask = site == SITE_CARD
            m['name_disagree'] += int((card_mask != eff_card).sum())
            block_mask = site == SITE_BLOCK
        else:
            card_mask, block_mask = eff_card, np.zeros(n, bool)
        idx_ops, pos_ops = f.draw_ops()
        pos_of = np.full(n, -1, np.int64)
        pos_of[idx_ops] = pos_ops
        cards = idx_ops[card_mask[idx_ops]]            # in stream (op) order
        blocks = idx_ops[block_mask[idx_ops]]
        ncard, nblock = len(cards), len(blocks)
        per_frame['cards'].append(ncard)
        per_frame['blocks'].append(nblock)
        # ---- slots of every draw: image by (draw, slot)
        sl = f.slots
        tf, tc = D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
        tot = int(tc.sum())
        owner = np.repeat(np.arange(n), tc)
        rows = (np.repeat(tf - (np.cumsum(tc) - tc), tc) + np.arange(tot)) if tot else np.zeros(0, np.int64)
        slot_no = sl['slot'][rows] if tot else np.zeros(0, 'u1')
        slot_img = sl['image'][rows] if tot else np.zeros(0, '<u4')

        def image_of(s):
            out = np.zeros(n, np.int64)
            sel = slot_no == s
            out[owner[sel]] = slot_img[sel]
            return out
        img0, img13, img16 = image_of(0), image_of(13), image_of(16)
        # ---- bakes and the page
        copies_t = table(f, SEC_COPIES, COPY_DT)
        extras = table(f, 16, np.dtype([('b', 'u1', (4,))]))['b'] if 16 in f.secs else np.zeros((0, 4), 'u1')
        passes = walk(f, copies_t, cap)
        bakes = [p for p in passes if p.size == (64, 64) and p.draws and all(veff[d] == EFF_BAKE_VS for _, d in p.draws)]
        nbake = len(bakes)
        per_frame['bakes'].append(nbake)
        if named:
            m['bake_pass_site_bad'] += sum(1 for p in bakes for _, d in p.draws if int(f.origins[d]['pass_site']) != SITE_BAKE_PASS)
            baked = set(d for p in bakes for _, d in p.draws)
            for d in np.nonzero(f.origins['pass_site'] == SITE_BAKE_PASS)[0]:
                if int(d) not in baked:
                    m['bake_pass_site_other'] += 1
                    C['bake_site_other_effects'][veff[d]] += 1
        bake_dest, bake_tex0, bake_tex16 = [], [], []
        for p in bakes:
            m['bake_draws_ne1'] += int(len(p.draws) != 1)
            m['bake_no_clear'] += int(not p.clear)
            m['bake_copies_ne1'] += int(len(p.copies) != 1)
            d = p.draws[0][1]
            C['bake_vertex_counts'][(len(bake_dest), int(D[d]['vertex_count']))] += 1
            C['bake_tris_idx'][(int(D[d]['vertex_count']) > 0, int(D[d]['index_count']) > 0)] += 1
            if p.copies:
                cp = copies_t[p.copies[0][1]]
                dst = int(cp['d']['image'])
                bake_dest.append(dst)
                m['bake_copy_src_bad'] += int(int(cp['s']['image']) != p.target)
                m['bake_copy_rect_bad'] += int(list(cp['sr']) != [0, 0, 64, 64] or list(cp['dr']) != [0, 0, 64, 64])
                m['bake_dest_size_bad'] += int(cap.wh(dst) != (64, 64))
                S['pic_flags'].add(int(cap.imgs[dst]['flags']) if dst in cap.imgs else -1)
                S['pic_mips'].add(int(cap.imgs[dst]['mips']) if dst in cap.imgs else -1)
                if len(extras) > p.copies[0][1]:
                    S['bake_copy_extra'].add(tuple(int(x) for x in extras[p.copies[0][1]]))
            else:
                bake_dest.append(0)
            bake_tex0.append(int(img0[d]))
            bake_tex16.append(int(img16[d]))
            bank = f.bank(int(D[d]['vs_bank']))
            S['bake_viewproj'].add(words(bank, R_VP, 4).tobytes())
            S['bake_lmp'].add(words(bank, R_LMP).tobytes())
            S['bake_mat'].add(tuple(round(float(x), 6) for r in (reg('yAudDefault', 'vertex', 'MatAmbCol'), reg('yAudDefault', 'vertex', 'MatDifCol'), reg('yAudDefault', 'vertex', 'LightMin')) for x in fl(bank, r)))
            ls = b''.join(words(bank, r, k).tobytes() for r, k in LIGHT_SLICES)
            S['bake_lightset_f%d' % fi].add(ls)
            S['bake_lightset_all'].add(ls)
            S['bake_viewport'].add(tuple(decode_dyn(f.raw[SEC_DYN][int(D[d]['dynamic'])])['viewport']))
            S['bake_fixed'].add(f.raw[SEC_FIXED][int(D[d]['fixed'])])
            S['bake_tex_dims'].add(cap.wh(img0[d]))
            S['bake_ps'].add((cap.eff(int(D[d]['vs_hash'])), cap.eff(int(D[d]['ps_hash']))))
        if nbake:
            m['frames_with_bakes'] += 1
            m['bake_dest_dup'] += int(len(set(bake_dest)) != nbake)
            S['bake_dest_first'].add(bake_dest[0])
            S['bake_tex0_first'].add(bake_tex0[0])
            m['bake_tex0_not_consecutive'] += int(any(bake_tex0[i + 1] != bake_tex0[i] + 1 for i in range(nbake - 1)))
            m['bake_dest_not_ascending'] += int(any(bake_dest[i] >= bake_dest[i + 1] for i in range(nbake - 1)))
            m['bake_dest_not_consecutive'] += int(any(bake_dest[i + 1] != bake_dest[i] + 1 for i in range(nbake - 1)))
            m['bake_tex0_dup'] += int(len(set(bake_tex0)) != nbake)
            m['bake_tex0_not_ascending'] += int(any(bake_tex0[i] >= bake_tex0[i + 1] for i in range(nbake - 1)))
            m['bake_tex16_multi'] += int(len(set(bake_tex16)) != 1)
            m['bake_lightsets_per_frame_gt1'] += int(len(S['bake_lightset_f%d' % fi]) != 1)
            S['bake_lightset_counts'].add(len(S.pop('bake_lightset_f%d' % fi)))
            first_bake = bakes[0].pos
            last_bake = bakes[-1].pos
            plane = [pos_of[d] for d in idx_ops if veff[d].startswith(EFF_PLANE)]
            m['frames_plane_after_bake'] += int(bool(plane) and max(plane) >= first_bake)
        else:
            S.pop('bake_lightset_f%d' % fi, None)
        # ---- the light-map page: the copy whose destination the bakes' slot 16 reads
        page_img = bake_tex16[0] if bake_tex16 else (int(img13[cards[0]]) if ncard else 0)
        page_pass = None
        if page_img:
            for p in passes:
                for _, ci in p.copies:
                    if int(copies_t[ci]['d']['image']) == page_img:
                        page_pass = p
        per_frame['sprites'].append(len(page_pass.draws) if page_pass else 0)
        if page_pass is not None:
            m['frames_with_page'] += 1
            S['page_target'].add(page_pass.size)
            S['page_img_ids'].add(page_img)
            S['page_effects'].add(tuple(sorted(set(veff[d] for _, d in page_pass.draws))))
            S['page_copy_dest_dims'].add(cap.wh(page_img))
            C['page_sprite_tex'].update((cap.wh(img0[d]) or (0, 0)) for _, d in page_pass.draws)
            m['page_copies_ne1'] += int(len(page_pass.copies) != 1)
            m['page_no_clear_gt0'] += int(page_pass.clear)
            if nbake:
                m['page_after_bakes'] += int(page_pass.pos > bakes[-1].pos)
            if ncard:
                m['cards_after_page'] += int(pos_of[cards[0]] > page_pass.pos)
        # ---- the readers of the pictures
        pics = set(d for d in bake_dest if d)
        if pics:
            hit = np.isin(slot_img, np.array(sorted(pics), np.uint32))
            rd = np.unique(owner[hit])
            m['reader_draws'] += len(rd)
            for d in rd:
                C['reader_by'][('%08X' % int(f.origins[d]['call_site'])) if named else veff[d]] += 1
            C['reader_slots'].update(int(x) for x in slot_no[hit])
            m['reader_draws_not_card'] += int((~card_mask[rd]).sum())
            m['frames_with_readers'] += int(len(rd) > 0)
        # ---- the card draws
        if ncard:
            m['frames_with_cards'] += 1
            m['cards'] += ncard
            p = pos_of[cards]
            m['noncontiguous_frames'] += int(p[-1] - p[0] + 1 != ncard)
            if named:
                prev = int(p[0]) - 1
                before_block = prev >= 0 and int(f.ops[prev]['kind']) == 2 and bool(block_mask[int(f.ops[prev]['index'])])
                m['follows_block_frames'] += int(before_block)
                m['block_after_cards_frames'] += int(bool(len(blocks)) and int(pos_of[blocks].max()) > int(p[0]))
            pic = img0[cards]
            m['type_order_bad_frames'] += int(bool((np.diff(pic) < 0).any()))
            m['pic_distinct_max'] = max(m['pic_distinct_max'], len(set(int(x) for x in pic)))
            S['program'].update((int(D[d]['vs_hash']), int(D[d]['ps_hash'])) for d in cards[:1])
            S['program_all'].update(set(zip(D['vs_hash'][cards].tolist(), D['ps_hash'][cards].tolist())))
            # quad
            geo = {}
            keys = set((int(D[d]['vertex_offset']), int(D[d]['vertex_bytes']), int(D[d]['index_offset']), int(D[d]['index_bytes']), int(D[d]['layout']), int(D[d]['vertex_count']),
                        int(D[d]['index_count']), int(D[d]['index_32bit']), int(D[d]['source_prim']), int(D[d]['flags'])) for d in cards)
            m['quad_offsets_gt1_frames'] += int(len({k[:4] for k in keys}) != 1)
            for k in keys:
                vo, vb, io, ib, lay, vc, ic, i32, sp, fg = k
                blob = bytes(cap.geo[vo:vo + vb]) + b'|' + bytes(cap.geo[io:io + ib])
                S['quad_blob'].add(blob)
                S['quad_shape'].add((vc, ic, i32, sp, fg, vb, ib))
                S['quad_layout'].add((int(f.layouts[lay]['stride']), tuple((int(e['fmt']), int(e['off'])) for e in f.elements[int(f.layouts[lay]['first']):int(f.layouts[lay]['first']) + int(f.layouts[lay]['count'])])))
            # registers
            vids = np.unique(D['vs_bank'][cards])
            vrows = np.stack([f.bank(int(i)) for i in vids])
            vpos = {int(i): j for j, i in enumerate(vids)}
            diff = (vrows != vrows[0]).any(axis=0)
            S['vs_varying'].update(int(x) // 4 for x in np.nonzero(diff)[0])
            pids = np.unique(D['ps_bank'][cards])
            prows = np.stack([f.bank(int(i)) for i in pids])
            S['ps_varying'].update(int(x) // 4 for x in np.nonzero((prows != prows[0]).any(axis=0))[0])
            S['c184_vs'].add(words(vrows[0], R_LMP).tobytes())
            S['c184_ps'].add(words(prows[0], R_LMP_PS).tobytes())
            vp = words(vrows[0], R_VP, 4).tobytes()
            S['card_viewproj_frames'].add(vp)
            if len(blocks):
                bids = np.unique(D['vs_bank'][blocks])
                bvp = {words(f.bank(int(i)), R_VP, 4).tobytes() for i in bids}
                m['viewproj_block_equal'] += int(bvp == {vp})
                m['frames_with_cards_and_blocks'] += 1
                S['c184z_blocks'].update(float(fl(f.bank(int(i)), R_LMP)[2]) for i in bids)
            # state
            fixed_ids = np.unique(D['fixed'][cards])
            S['fixed'].update(f.raw[SEC_FIXED][int(i)] for i in fixed_ids)
            dyn_ids = np.unique(D['dynamic'][cards])
            m['dyn_gt1_frames'] += int(len(dyn_ids) != 1)
            dyn_raw = {f.raw[SEC_DYN][int(i)] for i in dyn_ids}
            if len(blocks):
                bd = {f.raw[SEC_DYN][int(i)] for i in np.unique(D['dynamic'][blocks])}
                m['dyn_equal_blocks'] += int(dyn_raw == bd)
            S['dyn_card'].update(dyn_raw)
            # textures
            trow = rows[np.isin(owner, cards)] if tot else rows
            if len(trow):
                sub = sl[trow]
                for r in np.unique(sub):
                    smp = f.raw[SEC_SAMPLER][int(r['sampler'])]
                    S['card_slot_rows'].add((int(r['slot']), bytes(r['swizzle']), bytes(r['sign']), int(r['view_format']), int(r['dimension']), int(r['min_level']), int(r['max_level']),
                                             round(float(r['lod_bias']), 6), smp))
            S['card_slots'].update(tuple(sorted(slot_no[owner == d].tolist())) for d in cards[:1])
            m['slotset_gt1_frames'] += int(len({tuple(sorted(slot_no[owner == d].tolist())) for d in cards[:50]}) != 1)
            m['slot13_multi_frames'] += int(len(set(img13[cards].tolist())) != 1)
            S['slot13_images_dims'].add(cap.wh(img13[cards[0]]))
            S['slot0_dims'].update({cap.wh(i) for i in set(pic.tolist())})
            S['slot0_flags'].update(int(cap.imgs[i]['flags']) if i in cap.imgs else -1 for i in set(pic.tolist()))
            if page_img:
                m['slot13_is_page'] += int(set(img13[cards].tolist()) == {page_img})
            if len(blocks):
                m['block_slot13_is_page'] += int(set(img13[blocks].tolist()) == {page_img})
            if pics:
                m['card_pics_in_bake_dests'] += int(set(pic.tolist()) <= pics)
            # ---- the record
            rnum = num - 1 if plant != 'record_offset' else num
            raw = cap.record(rnum) if named else None
            if raw is not None and rnum >= 0:
                m['frames_with_records'] += 1
                rcards, rblocks = parse_record(raw)
                m['record_cards'] += len(rcards)
                m['pair_stream_cards'] += ncard
                m['record_blocks'] += len(rblocks)
                bykey = collections.defaultdict(list)
                for j, (T, M, k) in enumerate(rcards):
                    bykey[k].append(j)
                # stream keys
                keys_of = []
                for d in cards:
                    bank = vrows[vpos[int(D[d]['vs_bank'])]]
                    keys_of.append(bank[4 * R_WORLD:4 * (R_WORLD + 3)].tobytes())
                seq, unp, dup = [], 0, 0
                used = collections.Counter()
                for k in keys_of:
                    l = bykey.get(k)
                    if not l:
                        unp += 1
                        seq.append(-1)
                        continue
                    if len(l) > 1:
                        dup += 1
                    j = l[min(used[k], len(l) - 1)]
                    used[k] += 1
                    seq.append(j)
                m['pair_unpaired_stream'] += unp
                m['pair_dup_keys'] += dup
                paired = sum(1 for s in seq if s >= 0)
                m['pair_paired'] += paired
                rec_unpaired = len(rcards) - len(set(s for s in seq if s >= 0))
                m['pair_unpaired_record'] += rec_unpaired
                m['pair_count_equal_frames'] += int(len(rcards) == ncard)
                m['pair_exact_frames'] += int(unp == 0 and rec_unpaired == 0 and dup == 0 and len(rcards) == ncard)
                if plant == 'order_swap':
                    # swap two adjacent paired cards of one type in the stream order
                    for a in range(len(seq) - 1):
                        if seq[a] >= 0 and seq[a + 1] >= 0 and rcards[seq[a]][0] == rcards[seq[a + 1]][0]:
                            seq[a], seq[a + 1] = seq[a + 1], seq[a]
                            break
                ps_ = [s for s in seq if s >= 0]
                inv = sum(1 for a in range(len(ps_) - 1) if ps_[a] > ps_[a + 1])
                m['order_inversions'] += inv
                m['order_bad_frames'] += int(inv > 0)
                tys = [rcards[s][0] for s in ps_]
                m['order_type_desc'] += sum(1 for a in range(len(tys) - 1) if tys[a] > tys[a + 1])
                per_t = collections.defaultdict(list)
                for s in ps_:
                    per_t[rcards[s][0]].append(rcards[s][1])
                m['order_member_desc'] += sum(1 for v in per_t.values() for a in range(len(v) - 1) if v[a] > v[a + 1])
                m['order_adjacent_pairs'] += max(0, len(ps_) - 1)
                # binding
                if len(bake_dest) == 20:
                    first_pic = int(pic[0])
                    for s, d in zip(seq, cards):
                        if s < 0:
                            continue
                        T = rcards[s][0]
                        if plant == 'type_shift':
                            T = (T + 1) % 20
                        want = bake_dest[T]
                        if plant == 'one_picture':
                            want = first_pic
                        if plant == 'atlas_picture':
                            want = page_img
                        m['bind_cards'] += 1
                        m['bind_bad'] += int(int(img0[d]) != want)
                else:
                    m['bind_frames_without_20_bakes'] += 1
                # bake T slot 0 == the stands blocks of type T
                if len(bake_tex0) == 20 and len(blocks):
                    bk = collections.defaultdict(list)
                    for j, (T, M, k) in enumerate(rblocks):
                        bk[k].append(T)
                    types_img = collections.defaultdict(set)
                    for d in blocks:
                        bank = f.bank(int(D[d]['vs_bank']))
                        k = bank[4 * R_WORLD:4 * (R_WORLD + 3)].tobytes()
                        for T in bk.get(k, [])[:1]:
                            types_img[T].add(int(img0[d]))
                    m['block_unpaired'] += sum(1 for d in blocks if not bk.get(f.bank(int(D[d]['vs_bank']))[4 * R_WORLD:4 * (R_WORLD + 3)].tobytes()))
                    for T, imgs_ in types_img.items():
                        T2 = (T + 1) % 20 if plant == 'type_shift' else T
                        m['blocktex_types'] += 1
                        m['blocktex_bad'] += int(imgs_ != {bake_tex0[T2]})
                        m['blocktex_multi'] += int(len(imgs_) != 1)
                    m['blocks_paired'] += len(blocks)
        # blocks of a frame without cards: nothing to join
    m['_per_frame'] = per_frame
    m['_S'] = S
    m['_C'] = C
    return m


# ---------------------------------------------------------------- the report ------------------------------------------------------------------------
def pct(v, q):
    return v[min(len(v) - 1, int(q * len(v)))]


def f32(x):
    return float(np.float32(x))


def observed(m):
    pf = m['_per_frame']
    S, C = m['_S'], m['_C']
    cards = sorted(pf['cards'])
    out = dict(
        frames=m['frames'], named=m['frames_named'] == m['frames'],
        cards=dict(frames_with=m['frames_with_cards'], sum=int(sum(cards)), min=cards[0] if cards else 0, max=cards[-1] if cards else 0,
                   min_nonzero=min([c for c in cards if c], default=0), median=pct(cards, 0.5) if cards else 0, p90=pct(cards, 0.9) if cards else 0),
        bakes=sorted(set(pf['bakes'])), sprites=sorted(set(pf['sprites'])),
        frames_bakes_20=sum(1 for x in pf['bakes'] if x == 20),
        blocks=dict(sum=int(sum(pf['blocks'])), max=max(pf['blocks']) if pf['blocks'] else 0),
    )
    qb = sorted(S['quad_blob'])
    quad = None
    if len(qb) == 1:
        vb, ib = qb[0].split(b'|')
        pos = np.frombuffer(vb, '<f4').reshape(-1, 4)[:, :3]
        uv = np.frombuffer(vb, '<f2').reshape(-1, 8)[:, 6:8].astype(np.float32)
        quad = dict(pos=[[float(x) for x in r] for r in pos], uv=[[float(x) for x in r] for r in uv], indices=[int(x) for x in np.frombuffer(ib, '<u2')])
    out['quad'] = dict(blobs=len(qb), shape=sorted(S['quad_shape']), layout=sorted(S['quad_layout']), data=quad, offsets_gt1_frames=m.get('quad_offsets_gt1_frames', 0))
    out['program'] = dict(pairs=len(S['program_all']), names=sorted({(pcap.effect_of(v), pcap.effect_of(p)) for v, p in S['program_all']}),
                          hashes=sorted(('%016X' % v, '%016X' % p) for v, p in S['program_all']))
    out['registers'] = dict(vs_varying=sorted(S['vs_varying']), ps_varying=sorted(S['ps_varying']), c184=sorted([list(map(f32, np.frombuffer(b, '<f4'))) for b in S['c184_vs']]),
                            c184_ps_equal_vs=S['c184_vs'] == S['c184_ps'], viewproj_distinct=len(S['card_viewproj_frames']),
                            viewproj_block_equal_frames=m.get('viewproj_block_equal', 0), frames_with_cards_and_blocks=m.get('frames_with_cards_and_blocks', 0),
                            block_c184_z=sorted(S['c184z_blocks']))
    out['state'] = dict(fixed_distinct=len(S['fixed']), fixed=[decode_fixed(x) for x in sorted(S['fixed'])], dyn_gt1_frames=m.get('dyn_gt1_frames', 0), dyn_equal_blocks=m.get('dyn_equal_blocks', 0),
                        dyn_distinct=len(S['dyn_card']), dyn=[decode_dyn(x) for x in sorted(S['dyn_card'])][:3])
    out['textures'] = dict(slotsets=sorted(S['card_slots']), slotset_gt1_frames=m.get('slotset_gt1_frames', 0), slot13_multi_frames=m.get('slot13_multi_frames', 0),
                           slot13_dims=sorted(S['slot13_images_dims'], key=str), slot0_dims=sorted(S['slot0_dims'], key=str), slot0_flags=sorted(S['slot0_flags']),
                           rows=[dict(slot=r[0], swizzle=list(r[1]), sign=list(r[2]), view_format=r[3], dimension=r[4], levels=[r[5], r[6]], lod_bias=r[7], sampler=sampler_name(r[8])) for r in sorted(S['card_slot_rows'])],
                           slot13_is_page=m.get('slot13_is_page', 0), block_slot13_is_page=m.get('block_slot13_is_page', 0), card_pics_in_bake_dests=m.get('card_pics_in_bake_dests', 0))
    vc = collections.defaultdict(set)
    for (k, v), c in C['bake_vertex_counts'].items():
        vc[k].add(v)
    allv = [v for s in vc.values() for v in s]
    vp = sorted(S['bake_viewproj'])
    out['bake'] = dict(frames_with=m.get('frames_with_bakes', 0), draws_ne1=m.get('bake_draws_ne1', 0), no_clear=m.get('bake_no_clear', 0), copies_ne1=m.get('bake_copies_ne1', 0),
                       copy_src_bad=m.get('bake_copy_src_bad', 0), copy_rect_bad=m.get('bake_copy_rect_bad', 0), dest_size_bad=m.get('bake_dest_size_bad', 0),
                       dest_dup_frames=m.get('bake_dest_dup', 0), dest_not_ascending=m.get('bake_dest_not_ascending', 0), dest_not_consecutive=m.get('bake_dest_not_consecutive', 0),
                       tex0_dup_frames=m.get('bake_tex0_dup', 0), tex0_not_ascending=m.get('bake_tex0_not_ascending', 0), tex16_multi_frames=m.get('bake_tex16_multi', 0),
                       pic_flags=sorted(S['pic_flags']), tex0_dims=sorted(S['bake_tex_dims'], key=str), effects=sorted(S['bake_ps']),
                       vertex_count_range=[min(allv), max(allv)] if allv else [], vertex_counts_per_slot_distinct={str(k): len(v) for k, v in sorted(vc.items())},
                       pass_site_bad=m.get('bake_pass_site_bad', 0), pass_site_other_draws=m.get('bake_pass_site_other', 0),
                       viewproj_distinct=len(vp), viewproj=[list(map(f32, np.frombuffer(b, '<f4'))) for b in vp][:2],
                       pass_site_other_effects=dict(C['bake_site_other_effects']), mat=sorted(S['bake_mat']), lightsets_all=len(S['bake_lightset_all']), copy_extra=sorted(S['bake_copy_extra']),
                       pic_mips=sorted(S['pic_mips']), dest_first_ids=sorted(S['bake_dest_first']), tex0_first_ids=sorted(S['bake_tex0_first']), tex0_not_consecutive=m.get('bake_tex0_not_consecutive', 0),
                       lmp=sorted([list(map(f32, np.frombuffer(b, '<f4'))) for b in S['bake_lmp']]), lightset_counts_per_frame=sorted(S['bake_lightset_counts']),
                       lightsets_gt1_frames=m.get('bake_lightsets_per_frame_gt1', 0), viewport=sorted(S['bake_viewport']), fixed_distinct=len(S['bake_fixed']),
                       fixed=[decode_fixed(x) for x in sorted(S['bake_fixed'])], plane_after_bake_frames=m.get('frames_plane_after_bake', 0))
    out['page'] = dict(frames_with=m.get('frames_with_page', 0), target=sorted(S['page_target'], key=str), effects=[list(e) for e in sorted(S['page_effects'])],
                       img_ids=sorted(S['page_img_ids']), copy_dest_dims=sorted(S['page_copy_dest_dims'], key=str), copies_ne1=m.get('page_copies_ne1', 0), clear_frames=m.get('page_no_clear_gt0', 0),
                       sprite_textures={'%dx%d' % k: v for k, v in sorted(C['page_sprite_tex'].items())}, after_bakes=m.get('page_after_bakes', 0), cards_after_page=m.get('cards_after_page', 0))
    out['readers'] = dict(draws=m.get('reader_draws', 0), not_card=m.get('reader_draws_not_card', 0), by=dict(C['reader_by']), slots=dict(C['reader_slots']), frames_with=m.get('frames_with_readers', 0))
    out['stream'] = dict(noncontiguous=m.get('noncontiguous_frames', 0), follows_block=m.get('follows_block_frames', 0), block_after_cards=m.get('block_after_cards_frames', 0),
                         type_order_bad=m.get('type_order_bad_frames', 0), pic_distinct_max=m.get('pic_distinct_max', 0))
    out['pairing'] = dict(frames_with_records=m.get('frames_with_records', 0), record_cards=m.get('record_cards', 0), stream_cards=m.get('pair_stream_cards', 0), record_blocks=m.get('record_blocks', 0),
                          paired=m.get('pair_paired', 0), unpaired_stream=m.get('pair_unpaired_stream', 0), unpaired_record=m.get('pair_unpaired_record', 0),
                          dup_keys=m.get('pair_dup_keys', 0), count_equal_frames=m.get('pair_count_equal_frames', 0), exact_frames=m.get('pair_exact_frames', 0))
    out['order'] = dict(inversions=m.get('order_inversions', 0), bad_frames=m.get('order_bad_frames', 0), type_desc=m.get('order_type_desc', 0), member_desc=m.get('order_member_desc', 0),
                        adjacent_pairs=m.get('order_adjacent_pairs', 0))
    out['binding'] = dict(cards=m.get('bind_cards', 0), bad=m.get('bind_bad', 0), frames_without_20_bakes=m.get('bind_frames_without_20_bakes', 0), block_types=m.get('blocktex_types', 0),
                          block_bad=m.get('blocktex_bad', 0), block_multi=m.get('blocktex_multi', 0), blocks_paired=m.get('blocks_paired', 0), block_unpaired=m.get('block_unpaired', 0))
    return out


# ---------------------------------------------------------------- the claims of the map (section 1), each re-measured --------------------------------
QUAD_POS = [[82.0, 0.0, 0.0], [-82.0, 0.0, 0.0], [82.0, -250.0, 0.0], [-82.0, -250.0, 0.0]]
QUAD_UV = [[1.0, 1.0], [0.0, 1.0], [1.0, 0.0], [0.0, 0.0]]
BAKE_VP = [2 / 164, 0.0, 0.0, 0.0, 0.0, -2 / 250, 0.0, -1.0, 0.0, 0.0, 0.001, 0.5, 0.0, 0.0, 0.0, 1.0]  # orthographic: x 2/164, y -2/250 (offset -1), z 0.001 + 0.5


class Checks:
    def __init__(self, quiet=False):
        self.rows = []
        self.quiet = quiet

    def add(self, label, name, ok, text):
        self.rows.append((label, name, bool(ok), text))
        if not (self.quiet and ok):
            print('%s  %-9s %-9s %s' % ('PASS' if ok else 'FAIL', label, name, text))

    def eq(self, label, name, got, want, what):
        ok = got == want
        self.add(label, name, ok, '%s equals cards_expect.json' % what if ok else '%s differs from cards_expect.json: measured %s, expected %s' % (what, json.dumps(got, sort_keys=True)[:300], json.dumps(want, sort_keys=True)[:300]))


def close(a, b, tol=1e-6):
    return len(a) == len(b) and all(abs(x - y) <= tol * max(1.0, abs(y)) for x, y in zip(a, b))


def evaluate(label, m, E, ck):
    o = observed(m)
    want_named = E.get('named', True)
    named = m['frames_named'] == m['frames']
    ck.add(label, 'names', named == want_named and m.get('name_disagree', 0) == 0,
           '%d of %d frames carry draw origins (expected %s); card draws keyed by origin vs by effect disagree on %d draws' % (m['frames_named'], m['frames'], 'all' if want_named else 'none', m.get('name_disagree', 0)))
    if want_named and not named:
        return o  # without origins the call-site numbers below would be guesses
    cards = o['cards']
    ck.add(label, 'counts', True, 'cards: %d draws in %d of %d frames (min %d / nonzero min %d / median %d / p90 %d / max %d); bakes per frame %s; page sprites per frame %s; stands blocks %d'
           % (cards['sum'], cards['frames_with'], o['frames'], cards['min'], cards['min_nonzero'], cards['median'], cards['p90'], cards['max'], o['bakes'], o['sprites'], o['blocks']['sum']))
    if cards['sum']:
        st = o['stream']
        ck.add(label, 'stream', st['noncontiguous'] == 0 and st['type_order_bad'] == 0 and (not named or (st['follows_block'] == cards['frames_with'] and st['block_after_cards'] == 0)),
               '%d frames with cards: %d non-contiguous, %d with the pictures out of ascending order, %d directly after a stands block draw, %d with a block draw after the cards (max %d pictures in a frame)'
               % (cards['frames_with'], st['noncontiguous'], st['type_order_bad'], st['follows_block'], st['block_after_cards'], st['pic_distinct_max']))
        pr = o['program']
        ck.add(label, 'program', pr['pairs'] == 1 and [list(x) for x in pr['names']] == [[EFF_CARD_VS, CARD_PS_NAME]],
               '%d (vs, ps) pair(s): %s' % (pr['pairs'], pr['names']))
        q = o['quad']
        qd = q['data']
        ok = (q['blobs'] == 1 and q['offsets_gt1_frames'] == 0 and q['shape'] == [(4, 4, 0, 6, 0, 64, 8)] and q['layout'] == [(16, ((106, 0), (83, 12)))] and qd is not None
              and qd['pos'] == QUAD_POS and qd['uv'] == QUAD_UV and qd['indices'] == [0, 1, 2, 3])
        ck.add(label, 'quad', ok, '%d vertex blob(s) in the capture, %d frames with more than one offset; shape (vertices, indices, 32-bit, source prim, flags, vertex bytes, index bytes) %s, layout %s, positions %s, uvs %s, indices %s'
               % (q['blobs'], q['offsets_gt1_frames'], q['shape'], q['layout'], qd and qd['pos'], qd and qd['uv'], qd and qd['indices']))
        rg = o['registers']
        ok = (rg['vs_varying'] == [76, 77, 78] and rg['ps_varying'] == [] and len(rg['c184']) == 1 and rg['c184'][0][2:] == [2.125, 1.0] and rg['c184_ps_equal_vs']
              and rg['viewproj_block_equal_frames'] == rg['frames_with_cards_and_blocks'] and rg['block_c184_z'] == ([2.5] if rg['frames_with_cards_and_blocks'] else []))
        ck.add(label, 'registers', ok, 'vs registers that differ between the cards of a frame: %s; ps: %s; c184 (vs == ps: %s) %s; ViewProj equals the blocks\' in %d of %d frames; the blocks\' c184.z %s'
               % (rg['vs_varying'], rg['ps_varying'], rg['c184_ps_equal_vs'], rg['c184'], rg['viewproj_block_equal_frames'], rg['frames_with_cards_and_blocks'], rg['block_c184_z']))
        sx = o['state']
        fx = sx['fixed'][0] if len(sx['fixed']) == 1 else None
        b0 = fx['blend'][0] if fx else None
        ok = (fx is not None and fx['topology'] == 5 - 1 and fx['depth_test'] == 1 and fx['depth_write'] == 1 and fx['depth_compare'] == 6 and fx['cull'] == 2 and fx['front_face'] == 1
              and fx['primitive_restart'] == 1 and b0 == [1, 6, 7, 0, 6, 7, 0, 15] and all(b[7] == 0 for b in fx['blend'][1:]) and fx['alpha_test'] == 1 and fx['alpha_func'] == 6
              and abs(fx['alpha_ref'] - 0.753) < 0.001 and sx['dyn_gt1_frames'] == 0 and sx['dyn_equal_blocks'] == rg['frames_with_cards_and_blocks'])
        ck.add(label, 'state', ok, '%d fixed state(s) in %d card draws: %s; %d frames with more than one dynamic state; dynamic == the blocks\' in %d of %d frames; viewport %s'
               % (sx['fixed_distinct'], cards['sum'], json.dumps(fx), sx['dyn_gt1_frames'], sx['dyn_equal_blocks'], rg['frames_with_cards_and_blocks'], sx['dyn'][:1]))
        tx = o['textures']
        rows = {r['slot']: r for r in tx['rows']}
        smp = rows.get(0, {}).get('sampler', {})
        ok = (tx['slotsets'] == [(0, 13)] and tx['slotset_gt1_frames'] == 0 and tx['slot13_multi_frames'] == 0 and tx['slot13_dims'] == [(512, 512)] and tx['slot0_dims'] == [(64, 64)]
              and set(rows) == {0, 13} and smp.get('mag') == 1 and smp.get('min') == 1 and smp.get('u') == 2 and smp.get('v') == 2 and tx['slot13_is_page'] == cards['frames_with']
              and tx['card_pics_in_bake_dests'] == cards['frames_with'])
        ck.add(label, 'textures', ok, 'slots %s; slot 13 one image per frame (%d frames with several), %s, = the page copy in %d of %d frames (the blocks\' too in %d); pictures %s, image flags %s, all among the bake copy destinations in %d frames; sampler of slot 0 %s; swizzle slot 0 %s, slot 13 %s'
               % (tx['slotsets'], tx['slot13_multi_frames'], tx['slot13_dims'], tx['slot13_is_page'], cards['frames_with'], tx['block_slot13_is_page'], tx['slot0_dims'], tx['slot0_flags'],
                  tx['card_pics_in_bake_dests'], smp, rows.get(0, {}).get('swizzle'), rows.get(13, {}).get('swizzle')))
    b = o['bake']
    if o['frames_bakes_20']:
        lm = b['lmp']
        ok = (b['frames_with'] == o['frames_bakes_20'] == o['frames'] and b['draws_ne1'] == 0 and b['no_clear'] == 0 and b['copies_ne1'] == 0 and b['copy_src_bad'] == 0 and b['copy_rect_bad'] == 0
              and b['dest_size_bad'] == 0 and b['dest_dup_frames'] == 0 and b['dest_not_ascending'] == 0 and b['tex0_dup_frames'] == 0 and b['tex0_not_ascending'] == 0 and b['tex16_multi_frames'] == 0
              and b['viewport'] == [(0.0, 0.0, 64.0, 64.0, 1.0, 0.0)] and [list(e) for e in b['effects']] == [[EFF_BAKE_VS, 'yAudience/base']] and b['plane_after_bake_frames'] == 0)
        ck.add(label, 'bake', ok,
               '%d of %d frames bake 20 passes of 64 x 64: %d with a draw count != 1, %d without a clear, %d with a copy count != 1, %d copy source / %d rect / %d destination-size faults, %d frames with a repeated destination, %d with descending destinations (%d non-consecutive), '
               '%d with a repeated or descending slot-0 texture, %d with several slot-16 images; viewport %s; effects %s; slot-0 texture dims %s, picture image flags %s'
               % (o['frames_bakes_20'], o['frames'], b['draws_ne1'], b['no_clear'], b['copies_ne1'], b['copy_src_bad'], b['copy_rect_bad'], b['dest_size_bad'], b['dest_dup_frames'], b['dest_not_ascending'],
                  b['dest_not_consecutive'], b['tex0_dup_frames'] + b['tex0_not_ascending'], b['tex16_multi_frames'], b['viewport'], b['effects'], b['tex0_dims'], b['pic_flags']))
        vp = b['viewproj']
        ck.add(label, 'bake_cam', b['viewproj_distinct'] == 1 and vp and close(vp[0], BAKE_VP, 1e-6) and b['mat'] == [(1.0,) * 8 + (0.0,) * 4] and b['lightsets_gt1_frames'] == 0 and b['lightset_counts_per_frame'] == [1] and len(lm) == 1 and lm[0][2:] == [2.5, 1.0] and (not cards['sum'] or lm[0][:2] == o['registers']['c184'][0][:2]) and b['pass_site_bad'] == 0 and b['fixed_distinct'] == 1,
               '%d distinct ViewProj over all bakes of all frames: %s; %d frames with more than one light set (per-frame set counts %s); LightMapParam (the bake reads the page with the card x / y scale and z 2.5 against the cards 2.125) %s; %d bake draws off pass site 0x%08X; %d other draws at that site; vertex counts %s; bake fixed state %s'
               % (b['viewproj_distinct'], [round(x, 6) for x in vp[0]] if vp else None, b['lightsets_gt1_frames'], b['lightset_counts_per_frame'], lm, b['pass_site_bad'], SITE_BAKE_PASS, b['pass_site_other_draws'], b['vertex_count_range'],
                  json.dumps(b['fixed'][0]) if b['fixed'] else None))
        pg = o['page']
        ck.add(label, 'page', pg['frames_with'] == o['frames'] and pg['target'] == [(512, 512)] and pg['copy_dest_dims'] == [(512, 512)] and pg['copies_ne1'] == 0 and pg['after_bakes'] == pg['frames_with']
               and all(set(e) == {'ySprite/tint_constant'} for e in pg['effects']),
               'light-map page: %d of %d frames, target %s copied once to a %s image, effects %s, sprite sources %s, after the bakes in %d frames, cards after the page in %d, a clear in %d' %
               (pg['frames_with'], o['frames'], pg['target'], pg['copy_dest_dims'], pg['effects'], pg['sprite_textures'], pg['after_bakes'], pg['cards_after_page'], pg['clear_frames']))
        rd = o['readers']
        ck.add(label, 'readers', rd['not_card'] == 0 and rd['draws'] == cards['sum'] and set(rd['slots']) <= {0},
               '%d draws read the 20 pictures (%d frames): by %s, slots %s; %d of them are not card draws; card draws %d' % (rd['draws'], rd['frames_with'], rd['by'], rd['slots'], rd['not_card'], cards['sum']))
    elif cards['sum']:
        ck.add(label, 'bake', False, 'cards but no frame with 20 bakes')
    pa, od, bd = o['pairing'], o['order'], o['binding']
    if pa['frames_with_records']:
        ck.add(label, 'pairing', pa['unpaired_stream'] == 0 and pa['unpaired_record'] == 0 and pa['dup_keys'] == 0 and pa['exact_frames'] == pa['frames_with_records'] and pa['record_cards'] == pa['stream_cards'] == pa['paired'],
               'record j-1 against stream frame j in %d frames: %d record cards, %d stream card draws, %d paired by world (c76-c78 bit-exact), %d stream draws without a record card, %d record cards without a draw, %d duplicate worlds, %d frames exact'
               % (pa['frames_with_records'], pa['record_cards'], pa['stream_cards'], pa['paired'], pa['unpaired_stream'], pa['unpaired_record'], pa['dup_keys'], pa['exact_frames']))
        ck.add(label, 'order', od['bad_frames'] == 0 and od['type_desc'] == 0 and od['member_desc'] == 0,
               'the stream draws the record\'s cards in the record\'s order: %d inversions over %d adjacent card pairs in %d frames (%d frames with any), type order broken %d times, member order within a type broken %d times'
               % (od['inversions'], od['adjacent_pairs'], pa['frames_with_records'], od['bad_frames'], od['type_desc'], od['member_desc']))
        ck.add(label, 'binding', bd['cards'] > 0 and bd['bad'] == 0 and bd['frames_without_20_bakes'] == 0 and bd['block_bad'] == 0 and bd['block_multi'] == 0 and bd['block_unpaired'] == 0 and bd['block_types'] > 0,
               '%d cards: %d bind a picture other than the destination of bake T (T = the record\'s type); %d frames without 20 bakes; stands blocks: %d types joined by the record (%d blocks, %d unpaired), %d whose blocks bind a texture other than bake T\'s slot 0, %d with more than one texture'
               % (bd['cards'], bd['bad'], bd['frames_without_20_bakes'], bd['block_types'], bd['blocks_paired'], bd['block_unpaired'], bd['block_bad'], bd['block_multi']))
    elif E.get('records', False):
        ck.add(label, 'pairing', False, 'cards_expect.json says this capture carries scene records, none was read')
    return o


def plain(o):
    if isinstance(o, dict):
        return {str(k): plain(v) for k, v in o.items()}
    if isinstance(o, (list, tuple, set)):
        return [plain(v) for v in o]
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, bytes):
        return o.hex()
    return o


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    lo, hi, step = (int(x) if x else None for x in (spec.split(':') + ['', ''])[:3])
    return list(range(lo or 0, min(hi if hi is not None else total, total), step or 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--expect', default=os.path.join(HERE, 'cards_expect.json'))
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--emit-expect')
    ap.add_argument('--dump', action='store_true', help='print every measured group')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    expect = json.load(open(a.expect, encoding='utf-8')) if os.path.exists(a.expect) else {}
    ck = Checks(a.q)
    bad_input, emitted = False, {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        t0 = time.time()
        try:
            cap = Cap(path)
            frames = parse_frames(a.frames, len(cap.frames))
            m = measure(cap, frames, a.plant)
        except (pcap.PError, CensusError, OSError, KeyError, IndexError, zipfile.BadZipFile) as e:
            print('FAIL  %-9s input     cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        E = expect.get(label, {})
        if not E and not a.frames and not a.emit_expect:
            ck.add(label, 'expect', False, 'no cards_expect.json entry for this capture')
        o = evaluate(label, m, E, ck)
        emitted[label] = plain(o)
        if a.dump:
            print(json.dumps(plain(o), indent=1, sort_keys=True))
        if E.get('data') is not None and not a.frames:
            ck.eq(label, 'expect', plain(o), E['data'], 'every measured group')
        print('      %-9s read %d frames in %.1f s' % (label, m['frames'], time.time() - t0))
    if a.emit_expect:
        json.dump({k: {'named': v['named'], 'data': v} for k, v in emitted.items()}, open(a.emit_expect, 'w', encoding='utf-8'), indent=1, sort_keys=True)
    if a.json:
        json.dump(emitted, open(a.json, 'w'), indent=1, sort_keys=True)
    fails = [r for r in ck.rows if not r[2]]
    print('%d checks, %d FAIL%s' % (len(ck.rows), len(fails), '' if not a.plant else '  (plant: %s)' % a.plant))
    return 1 if fails or bad_input else 0


if __name__ == '__main__':
    sys.exit(main())
