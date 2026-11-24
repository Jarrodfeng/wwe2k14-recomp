#!/usr/bin/env python3
r"""S103 lane B2 check: our scene draws take the game's own per-pass render state (src\renderer_scene\pass_state.h, docs\research\s103\T2_render_states.md). 0 game runs; offline.

  py -I tools\scene\b2_check.py [--capture DIR ...] [--frames 100] [--plant old_ref|cutout_noblend] [--game-only CASE_CAPTURE_DIR] [--crop-compare NAME --vs PLANT --crop X0,Y0,X1,Y1]

Game side: the capture's own stream (tools\solver\capread.py): per draw the DrawFixed (alpha test / func / reference, depth test / write / compare, cull, blend) joined to the draw's variant names.
Our side: the rig (out\tools\frame_rig.exe, WWE_RDR_FEATURE_DUMP) writes one "state" line per draw next to the "draw" line (our rdr::DrawState, the alpha constants the draw is fed, its pass class).
Our dump holds the frame's Record twice (the rig's seed frame, then the real one): the SECOND block is taken.
Per-draw compare: the draws of the two sides are grouped by the EXACT key (family, vertex variant, pixel variant); families are mapped from the effect name the same way on both sides. A group of ours with no
game group of that key is a failure (NO GAME GROUP), never a fallback to the family. Then, per draw of ours, against the game's draws of the same key:
  ALPHA ENABLE   ours tests alpha and the game never does in that group (ours test / game off), or ours does not and the game always does (ours off / game test).
  ALPHA REF      a tested draw's (function, reference) must be one the game's tested draws of the group carry (reference within 1/255). The old 0.5 (128) against a game 1 / 192 = FAIL `OLD ALPHA REF`.
  CUTOUT BLEND   a tested draw of ours that is not blended, with a reference < 128, where the game's group blends = FAIL (haze cards / mip-faded fences become solid without the blend).
  DEPTH          our compare must equal the game's mapped to forward Z (pass_state.h ForwardZ: GEQUAL -> LEQUAL, GREATER -> LESS; the shadow caster's LEQUAL stays).
  Reported, not asserted (T2 section 4): depth write, cull, blend on untested draws (the game blends every lit draw at alpha 1: not copied).
Coverage: per pass class (and wrestler vs other) the number of compared draws; a class the code can return with 0 compared draws over all the captures FAILs, except the rows named in UNCOVERED_OK
(the standard captures draw no wrestler: no record names the models): those are printed UNCOVERED, not passed silently.
--plant old_ref / cutout_noblend run the rig with the old behaviour (WWE_RDR_PLANT_OLD_ALPHA_REF / WWE_RDR_PLANT_CUTOUT_NOBLEND): the check must FAIL with OLD ALPHA REF / CUTOUT BLEND. A crash does not pass.
--crop-compare NAME --vs PLANT: the rig picture of a plain run and of a --plant run beside each other, the changed-pixel count and a side-by-side PNG under out\s103\B2\.
--game-only: the game census of a capture / case folder that has no scene records.
"""
import argparse, collections, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import feature_diff as F  # noqa: E402

FORWARD_Z = {6: 3, 4: 1}   # pass_state.h ForwardZ: only the reversed main-view functions (everything else unchanged)
CLASS_NAMES = ['lit', 'arena', 'hair', 'crowd', 'particles', 'see_through', 'see_through_cut', 'rim', 'plane_shadow', 'sweat']   # PassClass order
RETURNED = {0, 1, 2, 3, 5}   # the classes ClassOf can return
UNCOVERED_OK = {'lit:wrestler', 'hair', 'see_through'}   # no capture under out\captures draws a wrestler (the rig names no models; checked a00 / a04 / a41 matches + entrances, brock_losswin f110): UNCOVERED, printed every run


def fixeds(fr):
    buf = fr.buf
    magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
    sec = {}
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i)
        sec[sid] = (es, cnt, off)
    es, cnt, off = sec[7]
    assert es == 72, 'DrawFixed size changed: %d' % es
    out = []
    for i in range(cnt):
        b = buf[off + es * i:off + es * (i + 1)]
        out.append(dict(cull=b[5], dt=b[8], dw=b[9], dc=b[10], ben=b[24], bsrc=b[25], bdst=b[26], at=b[56], af=b[57], ar=int(round(struct.unpack_from('<f', b, 64)[0] * 255))))
    return out


def game_states(case, fx, n):
    fr, draws, _ = F.game_frame(case, fx, n)
    fx_ = fixeds(fr)
    for i, d in enumerate(draws):
        d.update(fx_[int(fr.draws[i]['fixed'])])
        d['family'] = F.family_of(d['effect'])   # the same mapping as ours (game_frame leaves yCh_* as 'unnamed')
    return draws


def our_states(dump):
    """the draws of the frame's SECOND Record (the first is the rig's seed frame); the first when only one block exists."""
    blocks, begins, last = collections.defaultdict(list), 0, None
    for line in open(dump, encoding='utf-8'):
        try:
            j = json.loads(line)
        except Exception:
            continue
        if j.get('k') == 'pass' and j.get('name') == 'frame_begin':
            begins += 1
        elif j.get('k') == 'draw':
            last = j
        elif j.get('k') == 'state' and last is not None:
            last.update({k: v for k, v in j.items() if k != 'k'})
            last['vs'] = (last.get('vs') or '').replace('yAudienceInst_', 'yAudience_')   # the instanced twin of the crowd's vertex variant is the same variant
            last['effect'] = last['ps'].rsplit('_', 2)[0] if last.get('ps') else ''
            last['family'] = F.family_of(last['effect'])
            blocks[begins].append(last)
            last = None
    if not blocks:
        return []
    return blocks[2] if 2 in blocks else blocks[min(blocks)]


def key_of(d):
    return d['family'], d['vs'] or '', d['ps'] or ''


def compare(gdraws, odraws, label, cover):
    """returns (fails, lines); cover[row] += 1 per compared draw of ours."""
    groups = collections.defaultdict(list)
    for d in gdraws:
        groups[key_of(d)].append(d)
    fails, rows = [], []
    n_ok = n_ref = n_en_on = n_en_off = n_cut = n_dep = n_nogroup = 0
    diff_dw = diff_cull = diff_ben = 0
    nogroup = collections.Counter()
    n_bearing = n_habit = 0
    habit = collections.Counter()
    seen = set()  # (kind, key) of the enable / no-group differences, checked against the pinned list b2_known.txt
    for d in odraws:
        k = key_of(d)
        g = groups.get(k)
        if not g:
            n_nogroup += 1
            nogroup[k] += 1
            seen.add(('NOGROUP', k))
            continue
        row = CLASS_NAMES[d['pc']] + (':wrestler' if d['family'] == 'wrestler' else '')
        cover[row] += 1
        cover[CLASS_NAMES[d['pc']]] += 1
        gtested = {(x['af'], x['ar']) for x in g if x['at']}
        gblend = any(x['ben'] for x in g if x['at'])
        if d['at']:
            if not gtested:
                n_en_on += 1
                seen.add(('ENABLE_ON', k))
                rows.append('  ALPHA ENABLE ours test / game off  %-9s %s / %s' % (k[0], k[1][-30:], k[2][-34:]))
            elif any(af == d['af'] and abs(ar - d['ar']) <= 1 for af, ar in gtested):
                n_ok += 1
            else:
                n_ref += 1
                rows.append('  ALPHA REF %-9s %-40s ours func %d ref %3d, the game %s' % (k[0], k[2][-40:], d['af'], d['ar'], sorted(gtested)))
            if gtested and gblend and not d['ben'] and d['ar'] < 128:
                n_cut += 1
                rows.append('  CUTOUT BLEND %-9s %-40s tested ref %d, not blended, the game blends' % (k[0], k[2][-40:], d['ar']))
        elif all(x['at'] for x in g):
            n_en_off += 1
            seen.add(('ENABLE_OFF', k))
            if d.get('oa') == 2 and d.get('dal', 0) > 0:   # the output alpha carries a texture alpha < 1 and the game tests + blends this group: a look difference, not a habit
                n_bearing += 1
                rows.append('  ALPHA-BEARING UNTESTED %-9s %s / %s (diffuse alpha below 1 on %.1f %% of texels)' % (k[0], k[1][-30:], k[2][-34:], 100 * d['dal']))
            else:
                n_habit += 1
                habit[CLASS_NAMES[d['pc']]] += 1
            rows.append('  ALPHA ENABLE ours off / game test  %-9s %s / %s' % (k[0], k[1][-30:], k[2][-34:]))
        if d['dt']:
            want = {FORWARD_Z.get(x['dc'], x['dc']) for x in g if x['dt']}
            if want and d['dc'] not in want:
                n_dep += 1
                rows.append('  DEPTH %-9s %-40s ours compare %d, the game (forward Z) %s' % (k[0], k[2][-40:], d['dc'], sorted(want)))
            gw = {x['dw'] for x in g if x['dt']}
            if gw and d['dw'] not in gw:
                diff_dw += 1
        if d['cull'] not in {x['cull'] for x in g}:
            diff_cull += 1
        if (not d['at']) and any(x['ben'] for x in g) and not d['ben']:
            diff_ben += 1    # blend-on-opaque of the game on an untested draw (alpha 1): documented, not copied
    lines = ['%s: %d game draws, %d of ours' % (label, len(gdraws), len(odraws)),
             '  alpha tested draws of ours: %d match the game (exact key, function + reference within 1/255), %d REF differ' % (n_ok, n_ref),
             '  alpha ENABLE: %d ours test / game off, %d ours off / game test; CUTOUT BLEND: %d tested + unblended + ref < 128 where the game blends' % (n_en_on, n_en_off, n_cut),
             '  depth compare: %d differ from the game mapped to forward Z; NO GAME GROUP: %d draws in %d groups' % (n_dep, n_nogroup, len(nogroup)),
             '  reported only: depth write differs on %d, cull on %d, blend-on-opaque (untested, game blends, ours not) on %d' % (diff_dw, diff_cull, diff_ben)] + rows[:10]
    for k, v in nogroup.most_common(6):
        lines.append('    NO GAME GROUP x%d: %s' % (v, ' / '.join(x[-40:] for x in k)))
    if n_ref:
        fails.append('OLD ALPHA REF: %d tested draws carry a reference the game does not (%s)' % (n_ref, label))
    lines.insert(3, '  ours off / game test split: %d HABIT (alpha-free texture or a variant whose output alpha is not the texture\'s: test + blend is a no-op) %s, %d ALPHA-BEARING (output alpha carries a texture alpha < 1)' % (n_habit, dict(habit), n_bearing))
    if n_bearing:
        fails.append('ALPHA-BEARING UNTESTED: %d draws the game tests + blends carry a texture alpha < 1 and ours leaves untested (%s)' % (n_bearing, label))
    SEEN_ALL.update(seen)
    known = load_known()
    new = sorted(s for s in seen if s not in known)
    lines.append('  pinned differences (tools\\scene\\b2_known.txt): %d groups seen, %d of them NEW' % (len(seen), len(new)))
    for kind, k in new[:8]:
        lines.append('    NEW %s %s' % (kind, ' / '.join(k)))
    if new:
        fails.append('NEW GROUP DIFFERENCE (alpha enable / no game group): %d groups not in b2_known.txt (%s)' % (len(new), label))
    if n_cut:
        fails.append('CUTOUT BLEND: %d tested unblended cut-outs (%s)' % (n_cut, label))
    if n_dep:
        fails.append('DEPTH FUNC: %d draws (%s)' % (n_dep, label))
    return fails, lines


def load_known():
    """the pinned differences: KIND | family | vs | ps | reason. A group listed here is a KNOWN, characterised difference (reported every run); a group NOT listed fails."""
    out = set()
    p = os.path.join(HERE, 'b2_known.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                f = [x.strip() for x in line.split('|')]
                if len(f) >= 4:
                    out.add((f[0], (f[1], f[2], f[3])))
    return out


def census(gdraws):
    c = collections.Counter()
    for d in gdraws:
        c[(d['family'], d['effect'], d['at'], d['af'], d['ar'], d['dc'], d['dw'], d['cull'], d['ben'])] += 1
    print('  family / effect / atest / afunc / aref255 / depth cmp / dw / cull / blend : draws')
    for k, v in c.most_common(25):
        print('  ', k, v)


def pick_frames(case, want):
    if want:
        return [int(x) for x in want.split(',')]
    s = sorted(set(case.scene) & set(case.frame_names))
    return [s[len(s) // 2]]


SEEN_ALL = set()
REASON = {'NOGROUP': 'ours differs from the game shader variant: a stand-in / vertex-LM / edge-fade / UVScroll vertex variant of ours, or a pixel variant (e.g. yAudience_lm_pixel_fragment vs the game\'s ..._pixel_color_alpha_fragment) that the game draws under another name or not in this frame; follow-up [L]',
          'ENABLE_ON': 'ours tests a part the game does not test in this key: arena slots 390-393 (yProp_Default_base), BC3 diffuse with 65 % of texels below alpha 255 (a real cut-out, texture evidence); the game\'s single draw of this key is another material',
          'ENABLE_OFF': 'habit: the game tests + blends every draw of this group (GEQUAL 1/255, arena class) but no texture alpha reaches the output (alpha-free diffuse, or a variant with out_alpha != 2): test + blend is a no-op, not copied (CLAUDE.md rule 6). b2_check FAILS this group if a draw in it carries a texture alpha < 1 into the output'}
PLANTS = {'old_ref': 'WWE_RDR_PLANT_OLD_ALPHA_REF', 'cutout_noblend': 'WWE_RDR_PLANT_CUTOUT_NOBLEND', 'untested_alpha': 'WWE_RDR_PLANT_UNTESTED_ALPHA'}


def run_capture(cap, frames, plant, out, fx, rig, cover):
    case = F.Case(cap)
    allfails, alllines = [], []
    main = F.main_checkout()
    game_dir = os.path.join(main, 'game_data', 'extracted')
    for n in pick_frames(case, frames):
        gd = game_states(case, fx, n)
        fr = capread_frame(case, n)
        vp, eye = F.stream_camera(case, fx, fr)
        if vp is None:
            return ['no arena draw with a ViewProj in %s frame %d' % (cap, n)], []
        envs = {PLANTS[plant]: '1'} if plant else {}
        tag = 'b2_%s_%s_%d' % (os.path.basename(cap.rstrip('\\/')), plant or 'new', n)
        o = F.our_frame(case, fx, n, out, rig, game_dir, vp, eye, envs, tag, ['--body-scale-one'])
        if o['rc'] != 0:
            return ['the rig exit %s: %s' % (o['rc'], o['text'][-300:])], []
        dump = os.path.join(out, '%s_%d_dump.jsonl' % (tag, n))
        od = our_states(dump)
        if not od:
            return ['the rig wrote no state lines (is the rig built from this tree?)'], []
        fails, lines = compare(gd, od, '%s frame %d' % (os.path.basename(cap.rstrip('\\/')), n), cover)
        allfails += fails
        alllines += lines
    return allfails, alllines


def crop_compare(out, name, n, vs, region):
    """the rig picture of the plain run beside the picture of the --plant `vs` run: the changed-pixel count, and a side-by-side PNG of `region` (X0,Y0,X1,Y1) or of the 160 x 90 tile that changed most."""
    import numpy as np
    from PIL import Image
    new = np.asarray(Image.open(os.path.join(out, 'b2_%s_new_%d_%d_whole.png' % (name, n, n))).convert('RGB')).astype(np.int16)
    old = np.asarray(Image.open(os.path.join(out, 'b2_%s_%s_%d_%d_whole.png' % (name, vs, n, n))).convert('RGB')).astype(np.int16)
    d = np.abs(new - old).sum(axis=2)
    lines = ['picture %s f%d (%s vs new): %d of %d pixels differ (max channel sum %d)' % (name, n, vs, int((d > 0).sum()), d.size, int(d.max()))]
    if region:
        x0, y0, x1, y1 = [int(v) for v in region.split(',')]
    else:
        th, tw = 90, 160
        best, x0, y0 = -1, 0, 0
        for y in range(0, d.shape[0] - th + 1, th // 2):
            for x in range(0, d.shape[1] - tw + 1, tw // 2):
                s = int(d[y:y + th, x:x + tw].sum())
                if s > best:
                    best, x0, y0 = s, x, y
        x1, y1 = x0 + tw, y0 + th
    sc = 4
    a = Image.fromarray(old[y0:y1, x0:x1].astype(np.uint8)).resize(((x1 - x0) * sc, (y1 - y0) * sc), Image.NEAREST)
    b = Image.fromarray(new[y0:y1, x0:x1].astype(np.uint8)).resize(((x1 - x0) * sc, (y1 - y0) * sc), Image.NEAREST)
    sheet = Image.new('RGB', (a.width * 2 + 8, a.height), (255, 0, 255))
    sheet.paste(a, (0, 0))
    sheet.paste(b, (a.width + 8, 0))
    path = os.path.join(out, 'b2_%s_%d_crop_%s_vs_new.png' % (name, n, vs))
    sheet.save(path)
    lines.append('  crop (%d,%d)-(%d,%d) x%d, left = %s, right = the pass-class state: %s' % (x0, y0, x1, y1, sc, vs, path))
    return lines


def capread_frame(case, n):
    return F.capread.Frame(case.src.read(case.frame_names[n]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', action='append', default=[])
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default=None, choices=[None, 'old_ref', 'cutout_noblend', 'untested_alpha'])
    ap.add_argument('--crop', default='')
    ap.add_argument('--rig', default='')
    ap.add_argument('--crop-compare', default='', help='capture name (brock_losswin): the rig pictures a plain run and a --plant run left in --out')
    ap.add_argument('--vs', default='old_ref', choices=['old_ref', 'cutout_noblend', 'untested_alpha'])
    ap.add_argument('--pin', action='store_true', help='write tools\\scene\\b2_known.txt from this run (a reviewed act: the file is the list of known differences)')
    ap.add_argument('--game-only', default='')
    ap.add_argument('--out', default=os.path.join('out', 's103', 'B2'))
    a = ap.parse_args()
    fx = F.Fx()
    main_co = F.main_checkout()
    if a.game_only:
        case = F.Case(a.game_only)
        for n in ([int(x) for x in a.frames.split(',')] if a.frames else sorted(case.frame_names)[len(case.frame_names) // 2:][:1]):
            print('game census %s frame %d' % (a.game_only, n))
            census(game_states(case, fx, n))
        return 0
    if a.crop_compare:
        print('\n'.join(crop_compare(os.path.join(ROOT, a.out), a.crop_compare, int(a.frames or 100), a.vs, a.crop)))
        return 0
    # the standard captures: two brock_losswin frames (arena fences / haze cards, props) and a04_match (a match with the crowd cards); none draws a wrestler (no record names the models)
    caps = a.capture or [os.path.join(main_co, 'out', 'captures', 'brock_losswin') + '@100', os.path.join(main_co, 'out', 'captures', 'brock_losswin_rt') + '@100', os.path.join(main_co, 'out', 'captures', 'a04_match')]
    out = os.path.join(ROOT, a.out)
    os.makedirs(out, exist_ok=True)
    rig = a.rig or os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    if not os.path.exists(rig):
        print('FAIL  no %s (pwsh tools\\scene\\build_rig.ps1)' % rig)
        return 2
    fails, lines = [], []
    cover = collections.Counter()
    for cap in caps:
        frames = a.frames
        if '@' in cap:
            cap, frames = cap.split('@', 1)
        f, l = run_capture(cap, frames, a.plant, out, fx, rig, cover)
        fails += f
        lines += l
    if a.pin:
        with open(os.path.join(HERE, 'b2_known.txt'), 'w', encoding='utf-8') as f:
            f.write('# S103 B2: the pinned (known, characterised) group differences of tools\\scene\\b2_check.py: KIND | family | vertex variant | pixel variant | reason. A group NOT listed fails the check.\n')
            for kind, k in sorted(SEEN_ALL):
                f.write('%s | %s | %s | %s | %s\n' % (kind, k[0], k[1], k[2], REASON[kind]))
        print('pinned %d groups' % len(SEEN_ALL))
    rows = [CLASS_NAMES[c] for c in sorted(RETURNED)] + ['lit:wrestler']
    lines.append('coverage (compared draws): ' + ', '.join('%s %d' % (r, cover[r]) for r in rows))
    for r in rows:
        if cover[r] == 0:
            if r in UNCOVERED_OK:
                lines.append('  UNCOVERED: %s (no standard capture draws it: the records do not name the models)' % r)
            elif not fails:
                fails.append('COVERAGE: pass class %s has 0 compared draws' % r)
    print('\n'.join(lines))
    if fails:
        print('FAIL  ' + '; '.join(fails))
        token = {'old_ref': 'OLD ALPHA REF', 'cutout_noblend': 'CUTOUT BLEND', 'untested_alpha': 'ALPHA-BEARING UNTESTED'}.get(a.plant)
        return 0 if (token and any(token in f for f in fails)) else 1   # a crash or another failure must not pass a plant
    print('PASS  B2: exact-key compare: alpha enable + function + reference, cut-out blend, depth compare all match the game for the pass classes covered')
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())
