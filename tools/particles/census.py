r"""Lane P0 (S101, lane P0 of docs\renderer\PARTICLES_PROMOTION_MAP.md section 9): the particles census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's sections 1-3 on case zips / capture folders: the one-draw-per-particle shape, the runs of byte-equal state (the
unit of the first slice P2a; compared as BYTES, never floats: c200.y is a NaN the game leaves in the camera-facing sprites), the call sites, the yPreLight-bake label, the crowd-flash pass,
the screen-flash T_Rect and the every-other-frame sprite lead.

  py -I tools\particles\census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--expect FILE] [--frames A:B:STEP] [--plant NAME] [--json OUT] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder (frames\ beside geometry.bin). LABEL names the capture's entry in expect.json (brock, bryan, wm30).
Checks (PASS / FAIL with counts; exit code 1 when any FAILs or an input cannot be read). Intrinsic checks need no numbers; "expect" checks compare with expect.json
(the map's [C] numbers where this tool confirmed them, the tool's own where it corrected them: see the page docs\systems\particles.md).
  names        every frame carries its draw origins (version-2 names); without them the call sites are unknowable (a version-1 capture fails loudly).
  shape        every particle draw is op 19 inline, 4 vertices, no indices, source prim 6, layout stride 32 with attributes at +0 / +12 / +16 / +24.
  regs         c201.x = 0, c231 = (0,0,0,1), c200.x = 0 in every particle draw; c200.y a NaN in the sprites (its bit patterns pinned) and 0 in the quads.
  varying      the VS-bank registers that differ between the particle draws of a frame are exactly the expected set (c200 lanes z/w and c228-c230; LocalWorld is 4 rows, c228-c231, c231 constant).
  sites        draws per call site equal expect; the third method (0x82A0ED2C) and ySoftParticle draw 0; no yParticle draw is off the known sites.
  runs         runs of adjacent, byte-equal-state particle draws (key: everything but c200.yzw and c228-c231, the call site included by choice): total draws, runs, heaviest
               frame, frame groups; boundaries: why each run ended, incl. the boundaries where ONLY the call site differs (pinned).
  block        the particle draws of a frame are one contiguous block (no other draw between the first and last) and sit between two post-chain draws and before the combine draw.
  sprite_lead  the camera-facing sprites of the lead-in come on alternate frames only (parity and counts).
  flash        crowd flash pass 0x82C139FC: draws, on-screen centres, per-frame max, one object / part, 20 vertices / 33 indices, all after the combine.
  prelight     0x82C20B90 draws yPreLight/sss (the skin-scattering cube bake): the label correction. 726 vertices / 1380 indices, draws and frames, the target size / layers. "Cube" and "skin-scattering" are [L] (not measured here).
  screen_flash T_Rect 0x82BC3EE8 (the generic rect: the GMEV flash, fades, bands): draws, span, rects, colour phases per rect, alpha limit. Reads stride 12 / fUseProjMatrix 0 only.
               (The RGB values, rect coordinates and alpha bytes in expect.json are behaviour facts of the rect, not game content: counts, ids and names are the rest.)
--plant (tests only): float_key (c200 unmasked + a float compare of the VS bank), nan_float (c200.xzw masked, floats compared: only the sprites' NaN splits), shuffle (the particle draws of each frame are put in another
order), merge_sites (the key forgets the call site), no_names (origins stripped: a version-1 capture).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
import json
import os
import random
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcap  # noqa: E402

PLANTS = ('float_key', 'nan_float', 'shuffle', 'merge_sites', 'no_names')
SITE_SPRITE, SITE_QUAD, SITE_THIRD = 0x82A0DA58, 0x82A0E338, 0x82A0ED2C
PSITES = (SITE_SPRITE, SITE_QUAD, SITE_THIRD)
FLASH_PASS, PRELIGHT_SITE, SCREEN_FLASH_SITE, COMBINE_SITE = 0x82C139FC, 0x82C20B90, 0x82BC3EE8, 0x82C1F674
SEC_FIXED, SEC_DYN, SEC_BOOL = pcap.SEC['fixeds'], pcap.SEC['dynamics'], pcap.SEC['bool_loops']
MASK_REGS = (228, 229, 230, 231)  # LocalWorld: the only per-particle constants the variant would read per vertex
EXPECT_ELEMENT_OFFSETS = [0, 12, 16, 24]


def reg_slice(r, n=1):
    return slice(4 * r, 4 * (r + n))


def sh(h):
    return '0x%08X' % h


# ---------------------------------------------------------------- measuring --------------------------------------------------------------------------
def measure(cap, frames, plant=None):
    rng = random.Random(1234)
    m = dict(frames=0, frames_named=0, version=set(),
             p_draws=0, p_sites=collections.Counter(), p_frames=0, p_max_frame=(0, -1), p_max_frame_runs=0, p_frame_groups={},
             runs=0, runs_by_site=collections.Counter(), site_only=0, c200y_bits=collections.Counter(), lone=0, op_between=0, draw_between=0, key_differs=0,
             shape_bad=0, layouts=set(), reg_bad=collections.Counter(), varying=set(), images=set(), per_frame_images=[0, 99], fixeds=set(),
             soft=0, off_site=0, gap_frames=0, op_gap_frames=0, after_post=0, no_post_frames=0,
             c200y_nan=0, before_block=collections.Counter(), after_block=collections.Counter(), sprite_frames=[], quad_first=None, sprite_counts=[], flash=None, prelight=None, screen=None)
    fl = dict(draws=0, on_screen=0, frames=0, max_frame=0, objects=set(), parts=set(), vcounts=set(), icounts=set(), effects=set(), before_combine=0, no_combine=0, first_frame=None)
    pl = dict(draws=0, frames_with=0, frames_without=0, per_frame=collections.Counter(), vcounts=set(), icounts=set(), vs=set(), ps=set(), sites_class=set(), targets=set())
    sf = dict(draws=0, frames=[], vs=set(), ps=set(), rect=set(), colours=[], alpha_max=0.0, per_frame=collections.Counter())
    for fi in frames:
        f = cap.frame(fi)
        m['frames'] += 1
        m['version'].add(f.version)
        has_org = f.has_origins and plant != 'no_names'
        m['frames_named'] += bool(has_org)
        d = f.draws
        n = len(d)
        org = f.origins if has_org else None
        didx, dpos = f.draw_ops()
        rank = np.full(n, -1, np.int64)
        rank[didx] = np.arange(len(didx))
        pos = np.full(n, -1, np.int64)
        pos[didx] = dpos
        vs_eff = [pcap.effect_of(h) for h in d['vs_hash']] if n else []

        def site(i):
            return pcap.queue_site(org[i]) if org is not None else 0  # S101 lane PN: the generic queue site (COMBINE_SITE) even when a post scope renamed call_site

        # ---- classification (draw order = the op list's)
        parts, post_ranks, combine_rank = [], [], None
        flash_i, prelight_i, screen_i = [], [], []
        for i in didx:
            i = int(i)
            eff = vs_eff[i]
            if org is not None:
                s, k = site(i), int(org[i]['klass'])
                if s in PSITES:
                    parts.append(i)
                elif eff.startswith('yParticle'):
                    m['off_site'] += 1
                if int(org[i]['pass_site']) == FLASH_PASS:
                    flash_i.append(i)
                if s == PRELIGHT_SITE:
                    prelight_i.append(i)
                if s == SCREEN_FLASH_SITE:
                    screen_i.append(i)
                if k == pcap.K_POST or (k == pcap.K_SHADOW and s in pcap.GENERIC_QUEUE_SITES):  # S101 lane PN: shadow reductions share the queue helper
                    post_ranks.append(int(rank[i]))
                    if s == COMBINE_SITE:
                        combine_rank = int(rank[i])
            elif eff.startswith('yParticle'):
                parts.append(i)
            if eff.startswith('ySoftParticle'):
                m['soft'] += 1
        if plant == 'shuffle' and parts:
            ranks = sorted(parts, key=lambda i: rank[i])
            rng.shuffle(ranks)
            # the shuffled draws keep their op-list slots: slot j now holds the draw that used to be elsewhere
            slots = sorted(parts, key=lambda i: rank[i])
            order = [(int(rank[s]), int(pos[s]), i) for s, i in zip(slots, ranks)]
        else:
            order = [(int(rank[i]), int(pos[i]), i) for i in sorted(parts, key=lambda i: rank[i])]

        # ---- particles
        if order:
            m['p_frames'] += 1
            m['p_draws'] += len(order)
            ranks_only = [r for r, _, _ in order]
            if ranks_only[-1] - ranks_only[0] + 1 != len(order):
                m['gap_frames'] += 1
            if [p for _, p, _ in order][-1] - [p for _, p, _ in order][0] + 1 != len(order):
                m['op_gap_frames'] += 1
            if combine_rank is None:
                m['no_post_frames'] += 1
            elif max(ranks_only) > combine_rank:
                m['after_post'] += 1
            # what sits right before and right after the block (class + call site of the neighbour draws)
            b0, b1 = ranks_only[0] - 1, ranks_only[-1] + 1
            nb = lambda rk: ('none' if rk < 0 or rk >= len(didx) else 'unnamed' if org is None else '%s %s' % (pcap.CLASS_NAMES[int(org[didx[rk]]['klass'])], sh(pcap.queue_site(org[didx[rk]]))))  # noqa: E731
            m['before_block'][nb(b0)] += 1
            m['after_block'][nb(b1)] += 1
            prev, runs_here = None, 0
            groups = set()
            imgs = set()
            frame_sites = collections.Counter()
            banks = []
            for r, p, i in order:
                x = d[i]
                s = site(i)
                frame_sites[s] += 1
                m['p_sites'][s] += 1
                # shape
                lay = f.layouts[int(x['layout'])]
                offs = [int(e['off']) for e in f.elements[lay['first']:lay['first'] + lay['count']]]
                m['layouts'].add((int(lay['stride']), tuple(offs)))
                if not (x['flags'] & pcap.DRAW_INLINE and x['vertex_count'] == 4 and x['index_count'] == 0 and x['source_prim'] == 6 and int(lay['stride']) == 32
                        and offs == EXPECT_ELEMENT_OFFSETS):
                    m['shape_bad'] += 1
                vb = f.bank(int(x['vs_bank']))
                banks.append(vb)
                fv = vb.view('<f4')
                if vb[reg_slice(201)][0] != 0:
                    m['reg_bad']['c201.x'] += 1
                if not (fv[reg_slice(231)] == np.array([0, 0, 0, 1], np.float32)).all():
                    m['reg_bad']['c231'] += 1
                if fv[800] != 0:
                    m['reg_bad']['c200.x'] += 1
                # c200.y: a NaN the camera-facing sprites leave there (measured, S101 P0); the oriented quads write 0. Both are byte patterns, never compared as floats.
                if s == SITE_SPRITE:
                    m['c200y_nan'] += bool(np.isnan(fv[801]))
                    m['c200y_bits']['0x%08X' % int(vb[801])] += 1
                    if not np.isnan(fv[801]):
                        m['reg_bad']['c200.y_sprite_not_nan'] += 1
                elif fv[801] != 0 or np.isnan(fv[801]):
                    m['reg_bad']['c200.y_quad_not_zero'] += 1
                img = int(f.slots[int(x['tex_first'])]['image']) if x['tex_count'] else -1
                imgs.add(img)
                m['images'].add(img)
                m['fixeds'].add(int(x['fixed']))
                groups.add((s, int(x['fixed']), img))
                # the run key: everything the variant would share, as bytes
                masked = vb.copy()
                masked[801:804] = 0  # c200.yzw: ZOffset.x (c200.x) is read by yParticle.vert:25 (depth pulled to the camera) and stays in the key; y / z / w are never read
                masked[reg_slice(228, 4)] = 0
                kb = (int(x['vs_hash']), int(x['ps_hash']), int(x['vs_patched']), f.raw[SEC_FIXED][int(x['fixed'])], f.raw[SEC_DYN][int(x['dynamic'])], f.layout_bytes(int(x['layout'])),
                      f.slot_bytes(x), f.raw[SEC_BOOL][int(x['bool_loop'])] if f.raw[SEC_BOOL] else b'', int(x['source_prim']), int(x['vertex_count']), int(x['index_count']),
                      int(x['flags']), int(x['index_32bit']), f.bank(int(x['ps_bank'])).tobytes(), masked.tobytes(), 0 if plant == 'merge_sites' else s)
                if plant in ('float_key', 'nan_float'):
                    fl_all = vb.copy()
                    fl_all[reg_slice(228, 4)] = 0
                    if plant == 'nan_float':
                        fl_all[[800, 802, 803]] = 0  # c200.xzw masked: the only thing left for a float compare to trip on is the NaN in c200.y
                    fl_all = fl_all.view('<f4')
                else:
                    fl_all = None
                if prev is None:
                    new_run, reason = True, None
                else:
                    pr, pp, pk, pf, ps_ = prev
                    keys_equal = pk == kb and (fl_all is None or bool(np.array_equal(pf, fl_all)))
                    if r != pr + 1:
                        new_run, reason = True, ('draw_between' if keys_equal else 'key_differs')
                    elif p != pp + 1:
                        new_run, reason = True, ('op_between' if keys_equal else 'key_differs')
                    elif not keys_equal:
                        new_run, reason = True, 'key_differs'
                    else:
                        new_run, reason = False, None
                if new_run:
                    runs_here += 1
                    m['runs'] += 1
                    m['runs_by_site'][s] += 1
                    if reason:
                        m[reason] += 1
                        if reason == 'key_differs' and r == pr + 1 and p == pp + 1 and pk[:-1] == kb[:-1] and ps_ != s:
                            m['site_only'] += 1  # a boundary where nothing but the call site differs: the site is in the key by choice
                    cur_sites = {s}
                    cur_len = 1
                    run_open = [s, 1]
                    runs_list = m.setdefault('_runs', [])
                    runs_list.append(run_open)
                else:
                    run_open[1] += 1
                prev = (r, p, kb, fl_all, s)
            # varying registers inside the frame: the registers whose bytes differ between any two draws of a site
            for st in frame_sites:
                B = np.stack([banks[j] for j, (_, _, i) in enumerate(order) if site(i) == st])
                if len(B) > 1:
                    diff = np.nonzero((B != B[0]).any(axis=0))[0]
                    m['varying'].update((int(v) // 4, int(v) % 4) for v in diff)
            m['p_frame_groups'][fi] = len(groups)
            m['per_frame_images'][0] = max(m['per_frame_images'][0], len(imgs))
            m['per_frame_images'][1] = min(m['per_frame_images'][1], len(imgs))
            if len(order) > m['p_max_frame'][0]:
                m['p_max_frame'] = (len(order), fi)
                m['p_max_frame_runs'] = runs_here
            if frame_sites[SITE_SPRITE] and not frame_sites[SITE_QUAD] and m['quad_first'] is None:
                m['sprite_frames'].append((fi, f.guest_frame, frame_sites[SITE_SPRITE]))
            m['sprite_counts'].append((fi, frame_sites[SITE_SPRITE], frame_sites[SITE_QUAD]))
            if frame_sites[SITE_QUAD] and m['quad_first'] is None:
                m['quad_first'] = fi
        else:
            m['sprite_counts'].append((fi, 0, 0))

        # ---- crowd flashes
        if flash_i:
            fl['frames'] += 1
            fl['max_frame'] = max(fl['max_frame'], len(flash_i))
            if fl['first_frame'] is None:
                fl['first_frame'] = fi
        for i in flash_i:
            x = d[i]
            fl['draws'] += 1
            fl['objects'].add(int(org[i]['object']))
            fl['parts'].add(int(org[i]['part']))
            fl['vcounts'].add(int(x['vertex_count']))
            fl['icounts'].add(int(x['index_count']))
            fl['effects'].add('%s+%s' % (vs_eff[i], pcap.effect_of(x['ps_hash'])))
            fv = f.bank(int(x['vs_bank'])).view('<f4')
            vp = fv[reg_slice(56, 4)].reshape(4, 4)
            clip = vp @ np.append(fv[reg_slice(214)][:3], np.float32(1.0))
            cx, cy, cz, cw = (float(v) for v in clip)
            if cw > 0 and abs(cx) <= cw and abs(cy) <= cw and 0 <= cz <= cw:
                fl['on_screen'] += 1
            if combine_rank is None:
                fl['no_combine'] += 1
            elif rank[i] < combine_rank:
                fl['before_combine'] += 1

        # ---- prelight bake
        if prelight_i:
            pl['frames_with'] += 1
        else:
            pl['frames_without'] += 1
        pl['per_frame'][len(prelight_i)] += 1
        for i in prelight_i:
            x = d[i]
            pl['draws'] += 1
            pl['vcounts'].add(int(x['vertex_count']))
            pl['icounts'].add(int(x['index_count']))
            pl['vs'].add(vs_eff[i])
            pl['ps'].add(pcap.effect_of(x['ps_hash']))
            pl['sites_class'].add(pcap.CLASS_NAMES[int(org[i]['klass'])])
            pl['targets'].add(f.target_of(int(pos[i])))

        # ---- the screen flash
        sf['per_frame'][len(screen_i)] += 1
        for i in screen_i:
            x = d[i]
            sf['draws'] += 1
            sf['frames'].append(fi)
            sf['vs'].add(vs_eff[i])
            sf['ps'].add(pcap.effect_of(x['ps_hash']))
            # the reading below assumes pixel-style positions: 3 floats a vertex (stride 12) and fUseProjMatrix.x = 0 (yRect.vert). Anything else is not read as a rect: fail loudly.
            stride = int(f.layouts[int(x['layout'])]['stride'])
            use_proj = f.bank(int(x['vs_bank'])).view('<f4')[4 * 28]
            if stride != 12 or use_proj != 0 or not x['vertex_bytes']:
                raise pcap.PError('frame %d: T_Rect draw %d has vertex stride %d and fUseProjMatrix.x %s (this reader handles stride 12 and 0 only)' % (fi, i, stride, use_proj))
            v = f.vertices(x).view('<f4').reshape(-1, 3)
            rect = (float(v[:, 0].min()), float(v[:, 1].min()), float(v[:, 0].max()), float(v[:, 1].max()))
            sf['rect'].add(rect)
            c1 = f.bank(int(x['vs_bank'])).view('<f4')[reg_slice(1)]
            sf['colours'].append((fi, rect, tuple(round(float(q), 3) for q in c1)))
            sf['alpha_max'] = max(sf['alpha_max'], float(c1[3]))
    m['flash'], m['prelight'], m['screen'] = fl, pl, sf
    m['lone'] = sum(1 for _, n in m.get('_runs', []) if n == 1)
    return m


# ---------------------------------------------------------------- reporting --------------------------------------------------------------------------
def plain(v):
    if isinstance(v, (set, frozenset)):
        return sorted(plain(x) for x in v) if all(not isinstance(x, tuple) for x in v) else sorted(v)
    if isinstance(v, collections.Counter):
        return {str(k): v[k] for k in v}
    if isinstance(v, dict):
        return {str(k): plain(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [plain(x) for x in v]
    if isinstance(v, np.generic):
        return v.item()
    return v


def lead_summary(m):
    """The camera-facing sprites before the first oriented quad: which frames hold them (capture index and guest parity), counts, spacing."""
    fr = m['sprite_frames']
    out = dict(frames=len(fr), counts=[min((c for _, _, c in fr), default=0), max((c for _, _, c in fr), default=0)])
    if fr:
        early = [c for fi, _, c in fr if fi <= 198]
        out['counts_to_198'] = [min(early, default=0), max(early, default=0)]  # the map's "36-40 sprites, frames 0-198"; frames 200-208 hold more
        out['capture_parity'] =sorted({fi % 2 for fi, _, _ in fr})
        out['guest_parity'] = sorted({g % 2 for _, g, _ in fr})
        out['spacing'] = sorted({b[0] - a[0] for a, b in zip(fr, fr[1:])})
        first, last = fr[0][0], fr[-1][0]
        out['span'] = [first, last]
        out['gaps_inside_span'] = (last - first + 1) - len(fr)
    out['window_end'] = m['quad_first']
    if m['quad_first'] is not None:
        after = [(s + q) for fi, s, q in m['sprite_counts'] if fi >= m['quad_first']]
        out['after_window'] = [len(after), sum(1 for x in after if x)]  # frames from the first oriented quad on, and how many of them hold particles
    return out


def colour_phases(colours):
    """Run-length phases of the T_Rect colour, per distinct rect: [rect, first frame, last frame, rgb, alpha min, alpha max]."""
    out = []
    for rect in sorted({r for _, r, _ in colours}):
        cur = None
        for fi, r, c in colours:
            if r != rect:
                continue
            rgb = c[:3]
            if cur and cur[3] == rgb:
                cur[2], cur[4], cur[5] = fi, min(cur[4], c[3]), max(cur[5], c[3])
            else:
                cur = [list(rect), fi, fi, rgb, c[3], c[3]]
                out.append(cur)
    return [[r, a, b, list(rgb), lo, hi] for r, a, b, rgb, lo, hi in out]


class Checks:
    def __init__(self, quiet):
        self.rows, self.quiet = [], quiet

    def add(self, label, name, ok, detail):
        self.rows.append((label, name, ok, detail))
        if not self.quiet or not ok:
            print('%-4s %-8s %-13s %s' % ('PASS' if ok else 'FAIL', label, name, detail))

    def eq(self, label, name, got, want, what=''):
        self.add(label, name, got == want, '%s%s = %s%s' % (what, '' if what == '' else ' ', got, '' if got == want else ' (expected %s)' % (want,)))


def observed(m):
    """Everything the expect.json entries hold, as measured now (the same structure, so --emit-expect writes a file the checks read back)."""
    fl, pl, sf = m['flash'], m['prelight'], m['screen']
    lead = plain(lead_summary(m))
    ph = colour_phases(sf['colours'])
    span = [min(sf['frames']), max(sf['frames'])] if sf['frames'] else []
    return dict(
        varying=sorted([r, c] for r, c in m['varying']),
        sites={sh(s): c for s, c in sorted(m['p_sites'].items())},
        runs=[m['p_draws'], m['runs']],
        frame_stats=[m['p_frames'], m['p_max_frame'][0], m['p_max_frame_runs']],
        boundaries=dict(lone=m['lone'], op_between=m['op_between'], draw_between=m['draw_between'], key_differs=m['key_differs'], site_only=m['site_only']),
        c200y_sprite_bits=sorted(m['c200y_bits']),
        groups216=m['p_frame_groups'].get(216),
        images=sorted(m['images']), images_per_frame=list(m['per_frame_images']), fixeds=sorted(m['fixeds']),
        op_gap_frames=m['op_gap_frames'],
        lead=lead,
        flash=dict(draws=fl['draws'], frames=fl['frames'], max_frame=fl['max_frame'], on_screen=fl['on_screen'], objects=[sh(o) for o in sorted(fl['objects'])], parts=sorted(fl['parts']),
                   vcounts=sorted(fl['vcounts']), icounts=sorted(fl['icounts']), effects=sorted(fl['effects'])),
        prelight=dict(draws=pl['draws'], frames_with=pl['frames_with'], frames_without=pl['frames_without'], per_frame={str(k): v for k, v in sorted(pl['per_frame'].items())},
                      vcounts=sorted(pl['vcounts']), icounts=sorted(pl['icounts']), vs=sorted(pl['vs']), ps=sorted(pl['ps']), klass=sorted(pl['sites_class']),
                      targets=sorted([list(t) for t in pl['targets']])),  # [width, height, layers] of the colour target the bake draws into
        screen=dict(draws=sf['draws'], span=span, missing_in_span=[f for f in range(span[0], span[1] + 1) if f not in set(sf['frames'])] if span else [],
                    per_frame={str(k): v for k, v in sorted(sf['per_frame'].items())}, vs=sorted(sf['vs']), ps=sorted(sf['ps']), rect=[list(r) for r in sorted(sf['rect'])],
                    phases=[[r, a, b, rgb, int(round(hi * 255))] for r, a, b, rgb, lo, hi in ph], alpha_max_u8=int(round(sf['alpha_max'] * 255))),
    )


def evaluate(label, m, E, ck):
    named = m['frames_named'] == m['frames']
    ck.add(label, 'names', named, '%d of %d frames carry their draw origins' % (m['frames_named'], m['frames']))
    if not named:
        return  # without origins every call-site number below would be a guess
    tot = m['p_draws']
    ck.add(label, 'shape', m['shape_bad'] == 0 and (tot == 0 or m['layouts'] == {(32, tuple(EXPECT_ELEMENT_OFFSETS))}),
           '%d particle draws, %d off the shape (op 19 inline, 4 vertices, no indices, prim 6, stride 32, attributes at %s)' % (tot, m['shape_bad'], EXPECT_ELEMENT_OFFSETS))
    ck.add(label, 'regs', tot == 0 or not m['reg_bad'], 'c201.x = 0, c231 = (0,0,0,1), c200.x = 0 in all %d draws; c200.y NaN in the sprites (%d of %d) and 0 in the quads; broken: %s'
           % (tot, m['c200y_nan'], m['p_sites'].get(SITE_SPRITE, 0), dict(m['reg_bad']) or 'none'))
    ck.add(label, 'sites_none', m['p_sites'].get(SITE_THIRD, 0) == 0 and m['soft'] == 0 and m['off_site'] == 0,
           'third method %d, ySoftParticle %d, yParticle off the known sites %d (all must be 0)' % (m['p_sites'].get(SITE_THIRD, 0), m['soft'], m['off_site']))
    ck.add(label, 'block', m['gap_frames'] == 0 and m['after_post'] == 0,
           '%d frames with another draw between the first and last particle draw, %d with a particle after the combine draw (frames without one: %d); before the block %s, after it %s'
           % (m['gap_frames'], m['after_post'], m['no_post_frames'], dict(m['before_block']), dict(m['after_block'])))
    fl = m['flash']
    ck.add(label, 'flash_order', fl['before_combine'] == 0 and fl['no_combine'] == 0,
           '%d flash draws before the combine draw, %d in frames without one (must be 0)' % (fl['before_combine'], fl['no_combine']))
    obs = observed(m)
    names = dict(c200y_sprite_bits='c200y_bits', varying='varying', sites='sites', runs='runs', frame_stats='runs_frame', boundaries='boundaries', groups216='groups', images='images', images_per_frame='images_pf',
                 fixeds='fixeds', op_gap_frames='block_ops', lead='sprite_lead', flash='flash', prelight='prelight', screen='screen_flash')
    for key, name in names.items():
        if key in E:
            ck.eq(label, name, obs[key], E[key], key)
    lim = E.get('screen_alpha_limit_u8')
    if lim is not None:
        ck.add(label, 'screen_alpha', obs['screen']['alpha_max_u8'] <= lim, 'screen flash alpha max %d/255 (limit %d/255)' % (obs['screen']['alpha_max_u8'], lim))
    return obs


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
    report, bad_input, emitted = {}, False, {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        t0 = time.time()
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)  # captures live in the main checkout's out\ (also when this runs from a worktree)
        try:
            cap = pcap.Capture(path)
            if a.frames:
                lo, hi, step = (int(x) if x else None for x in (a.frames.split(':') + ['', ''])[:3])
                frames = list(range(lo or 0, hi if hi is not None else len(cap.frames), step or 1))
            else:
                frames = list(range(len(cap.frames)))
            m = measure(cap, frames, a.plant)
        except (pcap.PError, OSError, KeyError, IndexError) as e:
            print('FAIL %-8s input         cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        E = expect.get(label, {}) if not a.frames else {}
        if not E and not a.frames:
            if a.emit_expect:
                print('NOTE %s: no expect.json entry (writing a draft)' % label)
            else:
                ck.add(label, 'expect', False, 'no expect.json entry for this capture (a census with nothing to compare to proves little; pass --frames for a partial look)')
        obs = evaluate(label, m, E, ck)
        if obs is not None:
            emitted[label] = obs
        report[label] = dict(measure=plain({k: v for k, v in m.items() if not k.startswith('_')}), lead=plain(lead_summary(m)), phases=colour_phases(m['screen']['colours']), seconds=round(time.time() - t0, 1))
        print('     %-8s read %d frames in %.1f s' % (label, m['frames'], time.time() - t0))
    if a.emit_expect:
        json.dump(emitted, open(a.emit_expect, 'w', encoding='utf-8'), indent=1)  # a draft: read it against the map before committing it as expect.json
    if a.json:
        json.dump(plain(report), open(a.json, 'w'), indent=1, default=str)
    fails = [r for r in ck.rows if not r[2]]
    print('%d checks, %d FAIL%s' % (len(ck.rows), len(fails), '' if not a.plant else '  (plant: %s)' % a.plant))
    return 1 if fails or bad_input else 0


if __name__ == '__main__':
    sys.exit(main())
