#!/usr/bin/env python3
r"""Lane L4 (Session 103): the crowd is lit with the SAME light set the game's own crowd draws were fed (0 game runs; offline, frame_rig.exe + lm_extract).

  py -I tools\scene\l4_check.py --capture C:\...\out\s103\E7\case\capture --frame 400 [--plant file_sets] [--expect-fail]

What it answers: the crowd bodies were built with the arena FILE's light set 20 (bg04: four lights 0.4706, other directions); the game's crowd draws (yAudience / yAudDefault, the stream's
constants of the same frame, read by tools\lightmatch\lm_extract.py) carry the EVALUATED set 20 of the record (0.396, dirs (+-0.664, 0.342, +-0.664), sky 0.278, ground 0.121). The check draws the
record's crowd through frame_rig.exe with WWE_RDR_DUMP_CROWD_LIGHT=1 (one "[crowd-lit]" line per model: the set the instanced draw is lit with) and prints, per field, the max abs distance of
ours to the stream group (colours x4 rgb, sky, ground, dir 0 / 3). PASS = every LIT crowd model matches (1e-3) its closest stream audience group (stands, floor, the card bake bank); the far-card quad is the one unlit model when the record lists cards. Plants: unlit, file_sets, lit_cards must FAIL.
Also prints the crowd panel's mean luminance and colour of this arm (ours) so before / after can be compared. Numbers only; nothing from the game is written next to the repo.
"""
import argparse, glob, json, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig  # noqa: E402


def stream_crowd_tuples(capture, n, work):
    """EVERY audience group of the stream frame with light constants (the stands, the floor people, the card bake: the bake's bank differs in c8-c10, CROWD_CARDS_PROMOTION_MAP.md 9): [(draws, effect, tuple)]."""
    out = os.path.join(work, 'lm')
    os.makedirs(out, exist_ok=True)
    rc, text = frame_rig.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'lightmatch', 'lm_extract.py'), capture, '--frames', str(n), '--out', out])
    groups = json.load(open(os.path.join(out, 'frame_%d.json' % n)))['groups']
    res = []
    for g in groups:
        if g.get('effect') in ('yAudience', 'yAudDefault') and g['vals'].get('DifLghCol') and g['vals'].get('DifLghVec'):
            def v(k, g=g):
                a = g['vals'][k]
                return np.array(a.get('v') or a.get('f'), dtype=np.float64)
            dirs = v('DifLghVec').reshape(4, 4)[:, :3]
            res.append((g['draws'], g['effect'], {'col': v('DifLghCol').reshape(4, 4)[:, :3], 'sky': v('SkyCol')[:3], 'gnd': v('GroundCol')[:3], 'dir0': dirs[0], 'dir3': dirs[3]}))
    return sorted(res, key=lambda r: -r[0])


def parse_lit(text):
    res = []
    for m in re.finditer(r'\[crowd-lit\] model (\d+) set_idx (-?\d+) id (-?\d+) col=([^ ]+ [^ ]+ [^ ]+)\|([^ ]+ [^ ]+ [^ ]+)\|([^ ]+ [^ ]+ [^ ]+)\|([^ ]+ [^ ]+ [^ ]+) sky=(\S+ \S+ \S+) gnd=(\S+ \S+ \S+) dir0=(\S+ \S+ \S+) dir3=(\S+ \S+ \S+)', text):
        f = lambda s: np.array([float(x) for x in s.split()])
        res.append({'model': int(m.group(1)), 'id': int(m.group(3)), 'col': np.stack([f(m.group(i)) for i in (4, 5, 6, 7)]), 'sky': f(m.group(8)), 'gnd': f(m.group(9)), 'dir0': f(m.group(10)), 'dir3': f(m.group(11))})
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frame', type=int, default=400)
    ap.add_argument('--plant', default=None, help='file_sets = the old behaviour (the file\'s baseline sets): must FAIL')
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--camera-json', default=None, help='a camera_N.json of a frame_rig work folder (else read from the stream with fview)')
    ap.add_argument('--look-from-stream', action='store_true', help='an ENTRANCE capture: inject the stage look rows read from the stream (the crowd is then lit by the rig\'s slot 2, not the match key)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--tol', type=float, default=1e-3)
    a = ap.parse_args()
    main_root = frame_rig.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(a.capture)
    if not os.path.exists(capture) and not os.path.isabs(a.capture):
        capture = os.path.join(main_root, a.capture)   # a lane names out\... relative to the tree: the main checkout owns the captures
    work = a.out or os.path.join(ROOT, 'out', 's103', 'L4')
    os.makedirs(work, exist_ok=True)
    rig = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    rsc = os.path.join(capture, 'scene', 'frame_%06d.rsc' % a.frame)
    for need in (rig, game, capture, rsc):
        if not os.path.exists(need):
            print('FAIL  cannot read its input: ' + need)
            return 1
    if a.camera_json:
        vp = np.array(json.load(open(a.camera_json))['vp'])
        eye = None
    else:
        from fview_exe import find_fview
        fview = find_fview(root=ROOT)
        cam, msg = frame_rig.stream_camera(fview, capture, a.frame, game, work)
        if cam is None:
            print('FAIL  camera: ' + msg)
            return 1
        vp, eye = cam[0], cam[1]
    wants = stream_crowd_tuples(capture, a.frame, work)
    want = wants[0][2] if wants else None
    if want is None:
        print('FAIL  the stream frame has no crowd draw with light constants')
        return 1
    prefix = os.path.join(work, 'ours_%d%s' % (a.frame, '_' + a.plant if a.plant else ''))
    cmd = [rig, '--record', rsc, '--game', game, '--size', '1280x720', '--prefix', prefix, '--per-family', '--stream-viewproj', ','.join('%.9g' % x for x in vp), '--camera', ','.join('%.9g' % x for x in vp)]
    if eye is not None:
        cmd += ['--eye', ','.join('%.9g' % x for x in eye)]
    if a.look_from_stream:   # an entrance capture whose record predates the live look rows: the rows are fitted from the stream's own constants (frame_rig.stream_look)
        largs, lmsg = frame_rig.stream_look(capture, a.frame, work)
        print('look rows: ' + lmsg)
        cmd += largs
    env = dict(os.environ, WWE_RDR_DUMP_CROWD_LIGHT='1', RDR_MAIN_CHECKOUT=main_root)
    if a.plant == 'unlit':
        env['WWE_RDR_PLANT_UNLIT_CROWD'] = '1'   # the old variant choice: no FX_LIT bias at all = the simplest pair
    elif a.plant == 'lit_cards':
        env['WWE_RDR_PLANT_LIT_CARDS'] = '1'     # the far-card quad chosen lit like the bodies (double lighting)
    elif a.plant:
        cmd += ['--plant-crowd', a.plant]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=1800)
    out_text = (p.stdout or '') + (p.stderr or '')
    lit = parse_lit(out_text)
    print('frame_rig exit %d, crowd models lit: %d' % (p.returncode, len(lit)))
    fails = []
    variants = re.findall(r'\[crowd-var\] model (\d+) vs (\S+) ps (\S+)', out_text)
    var_of = {int(m): v for m, v, _ in variants}
    print('  vertex variants: ' + ', '.join(sorted(set(var_of.values()))))
    # the far-card quad is the ONE unlit model (the game draws cards with yAudience_lm_uv + lm_pixel_color_alpha; its picture is baked lit): it exists in the frame only when the record lists cards
    rc2 = subprocess.run([os.path.join(ROOT, 'out', 'tools', 'rsc_dump.exe'), rsc], capture_output=True, text=True, errors='replace')
    has_cards = 'card:' in (rc2.stdout or '')
    unlit = sorted(m for m, v in var_of.items() if '_lit_' not in v)
    want_unlit = 1 if has_cards else 0
    print('  record lists cards: %s; unlit models: %d (want %d: the card quad)' % (has_cards, len(unlit), want_unlit))
    if len(unlit) != want_unlit:
        fails.append('%d crowd model(s) drawn with an UNLIT vertex variant, %d expected (%s): bodies must be lit, the card quad must not be' % (len(unlit), want_unlit, ', '.join(var_of[m] for m in unlit[:2])))
    if not lit:
        fails.append('no [crowd-lit] line (no crowd model drawn)')
    worst = 0.0
    for L in lit:
        if var_of.get(L['model']) is not None and '_lit_' not in var_of[L['model']]:
            continue   # the card quad reads no light
        best = None   # the closest audience group of the stream (stands, floor, the card bake's own bank)
        for draws, eff, w in wants:
            d = {k: float(np.abs(L[k] - w[k]).max()) for k in ('col', 'sky', 'gnd', 'dir0', 'dir3')}
            if best is None or max(d.values()) < max(best[1].values()):
                best = (draws, d)
        d = best[1]
        worst = max(worst, max(d.values()))
        print('  model %d (set id %d): max |ours - stream group of %d draws|  col %.4f  sky %.4f  gnd %.4f  dir0 %.4f  dir3 %.4f' % (L['model'], L['id'], best[0], d['col'], d['sky'], d['gnd'], d['dir0'], d['dir3']))
    print('  stream audience groups: %s; stands tuple: col %.4f sky %.4f gnd %.4f dir0 %s' % ([(w[0], w[1]) for w in wants], want['col'][0, 0], want['sky'][0], want['gnd'][0], np.round(want['dir0'], 3)))
    cp = prefix + '_crowd.png'
    if os.path.exists(cp):
        im = np.asarray(Image.open(cp).convert('RGB'), dtype=np.float64)
        bg = im[0, 0]
        m = np.abs(im - bg).max(axis=2) > 6
        if m.any():
            px = im[m]
            lum = px @ np.array([0.2126, 0.7152, 0.0722])
            print('  crowd panel (ours): %d px, mean luminance %.2f, mean rgb %s' % (m.sum(), lum.mean(), np.round(px.mean(axis=0), 1)))
    if worst > a.tol:
        fails.append('crowd light set differs from the stream by %.4f (> %.4f)' % (worst, a.tol))
    ok = not fails
    print(('PASS' if ok else 'FAIL') + '  l4_check: ' + ('crowd lit with the stream\'s own set' if ok else '; '.join(fails)))
    if a.expect_fail:
        print(('PASS' if not ok else 'FAIL') + '  planted fault ' + ('caught' if not ok else 'NOT caught'))
        return 0 if not ok else 1
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
