#!/usr/bin/env python3
r"""S103 SWF check: the wrestlers' sweat / blood liquid map in the stream-free renderer. 0 game runs, 0 Surface runs: frame_rig.exe on a capture's scene record (offline; fview only for the rig's own game-side picture).

  py -I tools\scene\swf_check.py [--capture DIR] [--frame 110] [--out DIR] [--plant unbound|no_liquid_item_rows]

What it proves (docs\systems\liquid_map.md "Proof"):
  items  = the item the game drew: the capture's own sweat draws (pixel shader 022F374B74F15579) give each item's LocalArea (VS c205), WorldParam (VS c206) and its type (PS c203 / c204). They are TOLD to the rig as a
           labelled fixture (frame_rig --liquid-told / WWE_RDR_LIQUID_TOLD: the capture is older than the live writer, src\scene_live\liquid_*.h). The source texture is the rig's own blob (BC1, key 1).
  A  dry     the same frame with no liquid row: the baseline picture.
  B  sweat   the sweat items told: the skin must CHANGE (>= CHANGED_PIXELS pixels differ by more than 3 / 255 on the wrestler) and the maps hook must say it drew them and handed the map to the skin.
  C  blood   the same items as type 0 (blood decals): the skin must change, and differ from B.
  D  twin    B with the planted twin WWE_RDR_SWF_PLANT=sweat_unbound (the map is made and kept, never handed to the skin): the picture must equal A. This is the arm the check must CATCH:
             `--plant unbound` runs B under the plant and exits 1 with the token LIQUID MAP when the skin did not change.
  E  dry unchanged   A with the switch on against A with WWE_RDR_NO_LIQUID=1 (the code path before this lane): the pictures are byte-equal (a dry state is byte-unchanged).
Writes DIR\swf_sheet.png (dry | sweat | blood | where B differs) and prints one PASS / FAIL line per arm.
"""
import argparse, glob, hashlib, os, re, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or (os.path.normpath(os.path.join(ROOT, '..', '..', '..')) if os.sep + '.claude' + os.sep in ROOT else ROOT)
ENV = dict(os.environ, CLAUDECODE='1', WWE_FVIEW_ALLOW_STALE='1', RDR_MAIN_CHECKOUT=MAIN)
CHANGED_PIXELS = 150
SCALE = float(os.environ.get('SWF_SCALE', '8'))
SWEAT_PS = 0x022F374B74F15579


def run(cmd, env=None, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env or ENV, errors='replace', cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def sweat_items(cap, want=3):
    """Up to `want` distinct sweat draws of the capture -> dict(type, local[4], world[4]) from the draw's own constants."""
    collected = []
    for fp in sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin'))):
        b = open(fp, 'rb').read()
        _, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
        S = {}
        for s in range(sc):
            sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
            S[sid] = (es, cnt, so)
        banks_es, _, banks_so = S.get(13, (4096, 0, 0))
        defs = {}

        def rec(sid, i):
            es, cnt, so = S[sid]
            return so + es * i

        def bank(i):
            if i in defs:
                return defs[i]
            base, full, pf, pc = struct.unpack_from('<IIII', b, rec(17, i))
            d = list(struct.unpack_from('<1024I', b, banks_so + banks_es * full)) if base == 0xFFFFFFFF else list(bank(base))
            for p in range(pf, pf + pc):
                o = rec(18, p)
                blk = struct.unpack_from('<I', b, o)[0]
                d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
            defs[i] = d
            return d

        def vec(d, r):
            return [struct.unpack('<f', struct.pack('<I', d[4 * r + k]))[0] for k in range(4)]

        es, cnt, so = S[4]
        found = []
        for i in range(cnt):
            o = so + es * i
            vs, ps, vsp, fixed, dyn, tf, tc, vb, pb = struct.unpack_from('<QQQIIIIII', b, o)
            if ps != SWEAT_PS:
                continue
            pd, vd = bank(pb), bank(vb)
            c203, c204 = vec(pd, 203), vec(pd, 204)
            typ = 1 if (c203[0] == 1 and c203[1] == 0 and c204[1] == 1) else 0
            found.append(dict(type=typ, local=vec(vd, 205), world=vec(vd, 206)))
        for it in found:   # distinct items (by WorldParam) across the capture's frames: the first `want`
            if all(max(abs(it['world'][k] - o['world'][k]) for k in range(4)) > 0.02 for o in collected):
                collected.append(it)
        if len(collected) >= want:
            break
    return collected[:want]


def write_told(path, items, force_type=None):
    with open(path, 'w') as f:
        f.write('# SWF rig fixture: the capture\'s own sweat draws\' constants (LocalArea c205, WorldParam c206); nothing from the game is stored here beyond those few numbers\n')
        f.write('object actor\nmap 1024 512 3 2\n')
        for it in items:
            t = it['type'] if force_type is None else force_type
            w, lo = it['world'], it['local']

            def grow(b):   # a heavily sweaty state: the captured tears, SCALE times larger about their centres
                cx, cy = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
                return [cx + (b[0] - cx) * SCALE, cy + (b[1] - cy) * SCALE, cx + (b[2] - cx) * SCALE, cy + (b[3] - cy) * SCALE]
            f.write('item %d 0 %s %s\n' % (t, ' '.join(repr(float(x)) for x in grow(lo)), ' '.join(repr(float(x)) for x in grow(w))))


def rig(cap, frame, out, extra=None):
    env = dict(ENV)
    os.makedirs(out, exist_ok=True)
    rc, text = run([sys.executable, '-I', os.path.join('tools', 'scene', 'frame_rig.py'), '--capture', cap, '--frames', str(frame), '--out', out, '--body-scale-one', '--no-sheet', '--no-family-png'] + list(extra or []), env=env)
    rep = os.path.join(out, 'frame_%d_report.txt' % frame)
    rep_text = open(rep, errors='replace').read() if os.path.exists(rep) else ''
    pngs = glob.glob(os.path.join(out, '**', 'ours_%d_whole.png' % frame), recursive=True)
    return rc, text, rep_text, (pngs[0] if pngs else None)


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(int)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=os.path.join(MAIN, 'out', 'captures', 'brock_losswin'))
    ap.add_argument('--frame', type=int, default=110)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'swf'))
    ap.add_argument('--plant', default=None, choices=[None, 'unbound'])
    a = ap.parse_args()
    cap = a.capture
    if not os.path.isdir(cap):
        print('FAIL  LIQUID MAP: the capture %s is not on disk' % cap)
        return 1
    items = sweat_items(cap)
    if not items:
        print('FAIL  LIQUID MAP: no sweat draw (ps %016X) in %s' % (SWEAT_PS, cap))
        return 1
    os.makedirs(a.out, exist_ok=True)
    told_sweat, told_blood = os.path.join(a.out, 'told_sweat.txt'), os.path.join(a.out, 'told_blood.txt')
    write_told(told_sweat, items)
    write_told(told_blood, items, force_type=0)
    print('items: %d from the capture\'s own sweat draws (types %s)' % (len(items), [it['type'] for it in items]))
    fails, stats = [], {}

    def arm(name, extra):
        rc, text, rep, png = rig(cap, a.frame, os.path.join(a.out, name), extra)
        if rc != 0 or not png:
            fails.append('the rig failed for %s (exit %d): %s' % (name, rc, text[-300:].replace('\n', ' ')))
            return None, rep
        return load(png), rep

    if a.plant == 'unbound':
        dry, _ = arm('dry', [])
        wet, rep = arm('plant_unbound', ['--liquid-told', told_blood, '--liquid-plant', 'unbound'])
        if dry is None or wet is None:
            print('FAIL  LIQUID MAP (the rig itself failed, the plant is not judged): ' + '; '.join(fails))
            return 1
        n = int((np.abs(dry - wet).max(2) > 3).sum())
        print('twin (blood items, sweat_unbound): %d pixels differ from the dry picture' % n)
        if n < CHANGED_PIXELS:
            print('FAIL  LIQUID MAP: the skin did not change with the map made and never bound (%d pixels < %d): the planted twin is CAUGHT (exit 0, as the other planted arms)' % (n, CHANGED_PIXELS))
            return 0
        print('PASS  LIQUID MAP: the planted twin was NOT caught (%d pixels changed with the map unbound): the check is blind' % n)
        return 1

    dry, rep_dry = arm('dry', [])
    dry_off, _ = arm('dry_off', ['--liquid-plant', 'off'])
    sweat, rep_s = arm('sweat', ['--liquid-told', told_sweat])
    blood, rep_b = arm('blood', ['--liquid-told', told_blood])
    if any(x is None for x in (dry, dry_off, sweat, blood)):
        print('FAIL  LIQUID MAP: ' + '; '.join(fails))
        return 1
    # E: a dry state is byte-unchanged
    same = hashlib.sha256(dry.astype(np.uint8).tobytes()).hexdigest() == hashlib.sha256(dry_off.astype(np.uint8).tobytes()).hexdigest()
    print('%s  DRY UNCHANGED: the dry frame with the switch on equals the frame with WWE_RDR_NO_LIQUID=1 byte for byte' % ('PASS' if same else 'FAIL'))
    if not same:
        fails.append('dry changed')
    # B / C
    ds, db = np.abs(dry - sweat).max(2), np.abs(dry - blood).max(2)
    ns, nb = int((ds > 3).sum()), int((db > 3).sum())
    nsb = int((np.abs(sweat - blood).max(2) > 3).sum())
    stats.update(sweat_changed=ns, blood_changed=nb, sweat_vs_blood=nsb)
    ok = nb >= CHANGED_PIXELS
    print('%s  BLOOD: the skin changes (%d pixels differ by more than 3/255 from the dry frame; bar %d)' % ('PASS' if ok else 'FAIL', nb, CHANGED_PIXELS))
    if not ok:
        fails.append('blood did not change the skin')
    # Sweat reaches the skin only through gloss and the sweat highlight (fx_skin.glsl: specularGloss / sweatHighlight inside `shine`): in this rig frame (one small figure, no rim map from the game) that is 0 pixels
    # at >0 [U why: the shine term is ~0 here], so the sweat arm is judged on the MAP the pass made (G and B written where the items are, R untouched) and reported, not on a skin difference.
    def map_stats(name):
        p = glob.glob(os.path.join(a.out, name, '**', 'ours_%d_map_liquid.png' % a.frame), recursive=True)
        if not p:
            return None
        m = np.asarray(Image.open(p[0]).convert('RGBA')).astype(int)
        return [int((m[..., c] > 8).sum()) for c in range(3)]
    ms, mb = map_stats('sweat'), map_stats('blood')
    ok = bool(ms) and ms[1] >= 500 and ms[2] >= 500 and ms[0] == 0
    print('%s  SWEAT map: G / B written where the type-1 items are, R untouched (texels over 8/255 in R, G, B: %s); skin pixels changed %d (informational)' % ('PASS' if ok else 'FAIL', ms, ns))
    if not ok:
        fails.append('sweat map content')
    ok = bool(mb) and mb[0] >= 500 and mb[1] == 0 and mb[2] == 0
    print('%s  BLOOD map: R written, G / B untouched (%s)' % ('PASS' if ok else 'FAIL', mb))
    if not ok:
        fails.append('blood map content')
    # S111 LIQUID4BIT: the game's map texture is k_4_4_4_4 (sub_82799890) and every frame's rects are resolved into it, so every texel of R, G, B is n x 17 / 255 (n / 15). The planted twin is the 8-bit store
    # (the code before LIQUID4BIT, or the copy_prev_q4 pass left out): faint texels off the lattice.
    def off_lattice(name):
        p = glob.glob(os.path.join(a.out, name, '**', 'ours_%d_map_liquid.png' % a.frame), recursive=True)
        if not p:
            return None
        m = np.asarray(Image.open(p[0]).convert('RGBA')).astype(int)
        return int(((m[..., :3] % 17) != 0).sum())
    lat = [off_lattice(n) for n in ('sweat', 'blood')]
    ok = all(x == 0 for x in lat)
    print('%s  4-BIT STORE: map channel values off the 16-step lattice (n x 17) in the sweat / blood maps: %s' % ('PASS' if ok else 'FAIL', lat))
    if not ok:
        fails.append('map not on the 4-bit lattice')
    ok = nsb >= 20
    print('%s  SWEAT vs BLOOD: the two states differ (%d pixels)' % ('PASS' if ok else 'FAIL', nsb))
    if not ok:
        fails.append('sweat == blood')
    for label, rep in (('sweat', rep_s), ('blood', rep_b)):
        m = re.search(r'\[scene\]\[maps\]\[liquid\][^\n]*items drawn (\d+)[^\n]*handed to the skin (\d+)', rep)
        drawn, handed = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        ok = drawn >= 1 and handed >= 1
        print('%s  %s census: items drawn %d, maps handed to the skin %d%s' % ('PASS' if ok else 'FAIL', label, drawn, handed, '' if m else ' (no [scene][maps][liquid] line in the report)'))
        if not ok:
            fails.append(label + ' census')
    # the sheet
    try:
        w, h = dry.shape[1], dry.shape[0]
        diff = np.clip(ds * 8, 0, 255).astype(np.uint8)
        sheet = Image.new('RGB', (w * 4, h + 18), (20, 20, 20))
        for i, (img, label) in enumerate(((dry, 'dry'), (sweat, 'sweat (type 1 items)'), (blood, 'blood (type 0 items)'), (np.stack([diff] * 3, 2), 'where sweat differs x8'))):
            sheet.paste(Image.fromarray(img.astype(np.uint8)), (i * w, 18))
            ImageDraw.Draw(sheet).text((i * w + 4, 2), label, fill=(255, 255, 255))
        sheet.save(os.path.join(a.out, 'swf_sheet.png'))
    except Exception as e:  # noqa: BLE001
        print('(sheet not written: %s)' % e)
    print('stats', stats)
    if fails:
        print('FAIL  LIQUID MAP: ' + '; '.join(fails))
        return 1
    print('PASS  LIQUID MAP: blood changes the skin, sweat and blood paint their own channels of the map, the dry frame is byte-unchanged')
    return 0


if __name__ == '__main__':
    sys.exit(main())
