r"""S101 lane 128: the arena's D3 readiness ("skip the object": the game's own draw for an arena object never built, ours drawn from the record). Offline, 0 game runs.

The values ledger (ledger.py) measures per class; D3 needs every register an arena draw reads to come from a source we own. This tool runs the ledger with an
ARENA-WIDE class (what the arena hooks draw, not the ledger's variant rule) and splits it into object FAMILIES:
  static_A     pass 0x82C0A950, yBG_* variants: the set pieces (slot meshes drawn at the slot's own site)
  moving_A     pass 0x82C0A950, yBG_* whose world rows c76-c78 have a non-identity rotation: the slow-turning pieces (beam lights, ARENA_PROMOTION_MAP.md 73) [L]
  mirror_B     pass 0x82C0B94C: the mirrored `r` copies (camera = the main pass's with ViewProj x negated)
  placed       pass 0x82C0A950, yProp_DefaultLM_skinned* whose world rows c76-c78 are a pure translation with |x| and |z| >= 400: the placed slots 390-397
               (ARENA_OURS.md "bone-id props"; the ledger's own rule files them under ring because the record has no world for them) [L: a world-shape
               rule, cross-checked against the 16 draws a frame ARENA_OURS counts on arena 4]
  prop_static  pass 0x82C0A950, other yProp_* whose world equals an arena object's world in the record (the ledger's arena-prop rule)
  animated     any yProp_*UVScrol* (UV-scrolling surfaces: the animation is a constant the game advances)
  screens      pass 0x82C0F4BC yMovieDefault (the video screens, keys 800-809: planes from the movie decoder)
A draw takes the first family that fits in the order screens, mirror_B, animated, placed, moving_A, static_A, prop_static.

  py -I tools\skip\arena_ready.py CAPTURE_DIR [--frames A:B:STEP] [--json OUT] [--plant NAME] [-q]
Per family: draws, draws 100 % sourced, register reads by source (record / pass / mirror / template / role / light_state / derived / unsourced), the "provable" share
(sourced WITHOUT template: template is capture-constant, not proven constant, ledger section 7.5), the share of draws that would be 100 % sourced once the
light-state contributor (PER_DRAW_LIGHTS.md: R read at the model call) supplies its registers (the "projected" column: unsourced registers inside LIGHT_STATE_REGS
closed), and the unsourced registers that remain. --plant: placed_off (the placed family not recognised: --expect placed must fail), no_mirror_rule, offset0
(ledger plants passed through). Nothing from the game is written; --json is for under out\."""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402

SITE_SCREEN = 0x82C0F4BC
FAMILIES = ('static_A', 'moving_A', 'mirror_B', 'placed', 'prop_static', 'animated', 'screens')
# the registers PER_DRAW_LIGHTS.md section 3 sources from a light state read at the model call (R): DifLghCol c0-c4, DifLghVec c8-c12, TexProjLight c68-c71,
# ViewLight c164-c166, ShadowPrm PS c168-c170, sky / ground c181 / c182 (the derived rules c197 / c199 / KeyLight are already a ledger source)
LIGHT_STATE_REGS = ({('v', r) for r in list(range(0, 5)) + list(range(8, 13)) + list(range(68, 72)) + [164, 165, 166, 181, 182]} |
                    {('p', r) for r in list(range(0, 5)) + list(range(8, 13)) + [168, 169, 170, 181, 182]})
# per-object material constants the stream draw carries (MdlAlpha flag: two values over a capture, one per object; MatAmbCol / MatDifCol; PS MipmapBias): the file's material
# or a template proven by the proof frames; counted separately because the ledger's per-key template needs the key to repeat in two frames
MATERIAL_REGS = {('v', 185), ('v', 189), ('v', 190), ('p', 248)}
PLANTS = ('placed_off', 'no_mirror_rule', 'offset0')


def fl(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def placed_world(vb):
    """True when VS c76-c78 is a pure translation (identity 3x3) with |x| >= 400 and |z| >= 400 (the placed slots 390-397: (+-561, 0, -560), (+-532.9, 0, 560))."""
    r = [tuple(fl(x) for x in vb[4 * k:4 * k + 4]) for k in (76, 77, 78)]
    ident = r[0][:3] == (1.0, 0.0, 0.0) and r[1][:3] == (0.0, 1.0, 0.0) and r[2][:3] == (0.0, 0.0, 1.0)
    return ident and abs(r[0][3]) >= 400 and abs(r[2][3]) >= 400


def moving_world(vb):
    r = [tuple(fl(x) for x in vb[4 * k:4 * k + 3]) for k in (76, 77, 78)]
    return r != [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)]


def make_classify(plants):
    def classify(vn, o, vb, rindex):
        site = o['pass_site']
        if site == SITE_SCREEN:
            return 'arena' if vn and vn.startswith('yMovieDefault') else None
        base = L.classify_draw(vn, o, vb, rindex)
        if base == 'ring' and 'placed_off' not in plants and site == L.SITE_A and vn and vn.startswith('yProp_DefaultLM_skinned') and placed_world(vb):
            return 'arena'
        return base
    return classify


def make_family(plants):
    def family(o, vn, vb, rindex):
        site = o['pass_site']
        if site == SITE_SCREEN:
            return 'screens'
        if site == L.SITE_B:
            return 'mirror_B'
        if vn and 'UVScrol' in vn:
            return 'animated'
        if vn and vn.startswith('yProp_DefaultLM_skinned') and 'placed_off' not in plants and placed_world(vb):
            return 'placed'
        if vn and vn.startswith('yBG_'):
            return 'moving_A' if moving_world(vb) else 'static_A'
        return 'prop_static'
    return family


def analyse(path, frames='', plants=(), world=None):  # lane 130: world = {(frame, draw): {regs}} from arena_world_oracle (the kScArenaWorlds field as if recorded)
    lp = tuple(p for p in plants if p in ('no_mirror_rule', 'offset0'))
    out = L.run(path, frames, 'auto', lp, quiet=True, extra_sites=(SITE_SCREEN,), classify=make_classify(plants), family_fn=make_family(plants), world=world)
    R = out['res']['arena']
    fams = {}
    for name in FAMILIES:
        F = R['fam'].get(name)
        if not F or not F['draws']:
            fams[name] = dict(draws=0)
            continue
        tot = sum(F['regs'].values())
        prov = sum(F['regs'][s] for s in ('record', 'pass', 'mirror', 'role', 'light_state', 'derived'))  # lane 130: the fitted 'world' column is NOT provable (rule 4)
        un = {}
        for (stage, r), c in F['sbr'].items():
            if c['unsourced']:
                un[(stage, r)] = c['unsourced']
        onevalue = {k for k, v in F['uvals'].items() if len(v) == 1}  # an unsourced register that holds ONE value in every unsourced read: an our-constant, not a stream value
        proj_ok = sum(c for s, c in F['unsrc_sets_nw'].items() if set(s) <= LIGHT_STATE_REGS)
        proj3_ok = sum(c for s, c in F['unsrc_sets_nw'].items() if set(s) <= (LIGHT_STATE_REGS | onevalue | MATERIAL_REGS))
        proj2_ok = sum(c for s, c in F['unsrc_sets_nw'].items() if set(s) <= (LIGHT_STATE_REGS | onevalue))
        projf_ok = sum(c for s, c in F['unsrc_sets'].items() if set(s) <= LIGHT_STATE_REGS)  # lane 130: WITH the fitted world field (pending J1; never the headline)
        remain = {}
        for (stage, r), n in un.items():
            if (stage, r) not in LIGHT_STATE_REGS and (stage, r) not in onevalue and (stage, r) not in MATERIAL_REGS:
                remain[(stage, r)] = n
        fams[name] = dict(
            draws=F['draws'], full=F['full'], objects=len(F['objects']), reads=tot,
            world_pct={r: 100.0 * F['sbr'][('v', r)]['world'] / F['draws'] for r in (76, 77, 78, 186) if ('v', r) in F['sbr']},  # lane 130: draws whose register the kScArenaWorlds field sources
            by_source={s: F['regs'][s] for s in L.SOURCES},
            sourced_pct=100.0 * (tot - F['regs']['unsourced']) / tot if tot else 0.0,
            provable_pct=100.0 * prov / tot if tot else 0.0,
            full_pct=100.0 * F['full'] / F['draws'],
            projected_full_pct=100.0 * proj_ok / F['draws'],
            projected_fitted_full_pct=100.0 * projf_ok / F['draws'],
            world_reads=F['regs']['world'],
            projected_const_full_pct=100.0 * proj2_ok / F['draws'],
            projected_material_full_pct=100.0 * proj3_ok / F['draws'],
            onevalue_regs=sorted([s, r, L.reg_name(s, r), F['sbr'][(s, r)]['unsourced'], list(F['uvals'][(s, r)])[0]] for (s, r) in onevalue),
            by_reg={src_: sorted(([s, r, L.reg_name(s, r), c[src_]] for (s, r), c in F['sbr'].items() if c[src_]), key=lambda x: -x[3])[:14] for src_ in ('record', 'pass', 'mirror', 'template', 'role')},
            unsourced=sorted(([s, r, L.reg_name(s, r), n] for (s, r), n in un.items()), key=lambda x: -x[3]),
            remaining_after_light=sorted(([s, r, L.reg_name(s, r), n] for (s, r), n in remain.items()), key=lambda x: -x[3]),
        )
    return dict(path=out['path'], frames=out['frames'], rec_off=out['rec_off'], records_read=out['records_read'], light_records=out['light_records'],
                arena_draws=R['draws'], arena_full=R['full'], no_variant=R['no_variant'], families=fams,
                mirror_reads=sum(c['mirror'] for c in R['sources_by_reg'].values()))


def report(a):
    lines = ['arena D3 ledger: %s  %d frames  record offset %+d  records %d  light-state records %d' % (os.path.basename(os.path.dirname(a['path'])) or a['path'], a['frames'], a['rec_off'], a['records_read'], a['light_records']),
             'arena-wide: %d draws, %d 100%% sourced (%.2f%%), no-variant %d' % (a['arena_draws'], a['arena_full'], 100.0 * a['arena_full'] / max(1, a['arena_draws']), a['no_variant'])]
    for name in FAMILIES:
        f = a['families'][name]
        if not f['draws']:
            lines.append('  %-11s no draws' % name)
            continue
        src = ' '.join('%s %.1f%%' % (s, 100.0 * n / f['reads']) for s, n in f['by_source'].items() if n)
        lines.append('  %-11s draws %6d (%d objects)  100%%-sourced %5.2f%%  reads sourced %5.1f%% (provable, no template: %5.1f%%)  projected with light states: %5.1f%%, + one-value constants: %5.1f%%, + material constants: %5.1f%% of draws' % (
            name, f['draws'], f['objects'], f['full_pct'], f['sourced_pct'], f['provable_pct'], f['projected_full_pct'], f['projected_const_full_pct'], f['projected_material_full_pct']))
        lines.append('              reads: %s' % src)
        if f.get('world_pct') and any(f['world_pct'].values()):  # lane 130
            lines.append('              kScArenaWorlds field (FITTED on this capture, pending J1; not provable, not in the projected share; projected with it: %.1f%%) sources: %s' % (f['projected_fitted_full_pct'],', '.join('c%d %.1f%%' % (r, v) for r, v in sorted(f['world_pct'].items()) if v)))
        lines.append('              unsourced: %s' % ', '.join('%s c%d %s x%d' % ('VS' if s == 'v' else 'PS', r, n_, c) for s, r, n_, c in f['unsourced'][:10]))
        if f['onevalue_regs']:
            lines.append('              one-value (our constant) unsourced: %s' % ', '.join('%s c%d %s x%d' % ('VS' if s == 'v' else 'PS', r, n_, c) for s, r, n_, c, _v in f['onevalue_regs'][:8]))
        if f['remaining_after_light']:
            lines.append('              left after light states: %s' % ', '.join('%s c%d %s x%d' % ('VS' if s == 'v' else 'PS', r, n_, c) for s, r, n_, c in f['remaining_after_light'][:8]))
    return lines


def expect(a, names):
    res = []
    for e in names:
        F = a['families']
        if e == 'placed':
            n = F['placed']['draws'] / max(1, a['frames'] - 1)
            res.append((10 <= n <= 20, 'placed family: %.1f draws a frame (ARENA_OURS.md counts 16 on arena 4; want 10..20)' % n))
        elif e == 'mirror':
            f = F['mirror_B']
            cam = sum(1 for s, r, n_, c in f.get('unsourced', []) if s == 'v' and 56 <= r <= 59)
            res.append((f['draws'] > 0 and f['by_source']['mirror'] > 0 and cam == 0, 'mirror_B: mirror-rule reads %d, unsourced VS c56-c59 registers %d' % (f.get('by_source', {}).get('mirror', 0), cam)))
        elif e in ('world_placed', 'world_moving', 'world_clock'):  # lane 130: the kScArenaWorlds field (arena_world_oracle) sources the family's register
            fam, regs, floor = {'world_placed': ('placed', (76, 77, 78), 99.0), 'world_moving': ('moving_A', (76, 77, 78), 14.0), 'world_clock': ('animated', (186,), 60.0)}[e]
            wp = F[fam].get('world_pct', {})
            got = min([wp.get(r, 0.0) for r in regs])
            res.append((got >= floor, '%s: %s sourced by the world field in %.1f%% of draws (want >= %.0f%%)' % (fam, '/'.join('c%d' % r for r in regs), got, floor)))
        elif e == 'families':
            tot = sum(F[n]['draws'] for n in FAMILIES)
            res.append((tot == a['arena_draws'], 'family draws sum %d == arena-wide draws %d' % (tot, a['arena_draws'])))
        else:
            res.append((False, 'unknown --expect ' + e))
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    ap.add_argument('--json', default='')
    ap.add_argument('--worlds', default='', help='lane 130: the SOURCED file of arena_world_oracle.exe or arena_world_records.py (the kScArenaWorlds field as if recorded)')
    ap.add_argument('--expect', action='append', default=[])
    ap.add_argument('-q', action='store_true')
    x = ap.parse_args(argv)
    try:
        world = None
        if x.worlds:
            world = {}
            for ln in open(x.worlds, encoding='utf-8'):
                if ln.startswith('S '):
                    _, fi_, i_, regs_ = ln.split()
                    world[(int(fi_), int(i_))] = {int(r) for r in regs_.split(',')}
        a = analyse(x.capture, x.frames, (x.plant,) if x.plant else (), world)
    except (L.LedgerError, OSError, KeyError, struct.error, ValueError) as e:
        print('FAIL  %s: %s' % (x.capture, e))
        return 1
    for l in report(a):
        print(l)
    bad = 0
    for ok, msg in expect(a, x.expect):
        print('%s  expect %s' % ('PASS' if ok else 'FAIL', msg))
        bad += 0 if ok else 1
    if x.json:
        os.makedirs(os.path.dirname(os.path.abspath(x.json)), exist_ok=True)
        json.dump(a, open(x.json, 'w'), indent=1, default=str)
    print('arena_ready: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
