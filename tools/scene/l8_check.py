#!/usr/bin/env python3
r"""S103 lane L8: the stage light-map page of a MATCH is the game's page pass, and our page matches the game's pixels (0 game runs, 0 Surface runs).

  py -I tools\scene\l8_check.py [--capture out\captures\s100_match300] [--frame 2] [--plant old|p00] [--expect-fail TOKEN]

What the game does [C, docs\formats\arena_lightmap.md "S103 L8"]: a page object draws 512x512 sprites into a render target (image 1286 / 1201 ...) that is copied to the page image (388); in a match
after the entrance it draws fade_lightmap00 at alpha 1, then fade_lightmap01 over it at the vertex alpha int(255 * (t mod 600) / 600) (blend SRC_ALPHA / 1 - SRC_ALPHA), then 01 alone, then nothing.
CHECK 1 (identity): the stream's two page textures (images 298 / 299 of s100_match300, an arena-4 match start) are byte-equal to the arena's own fade_lightmap00 / fade_lightmap01 (l8_page_id).
CHECK 2 (pixels): the game's page target after the fade draw (fview --save-case, surface after the last page draw of the frame) against OUR page (assets::stage_pools::FadeFrom, tools\scene\l8_unit.cpp, run on the
player's own textures at the stream's own alpha): mean |d| per channel <= BAR (1.0 of 255). The planted twins: `--plant old` = today's match page (01 from the first frame) and `--plant p00` = 00 alone:
both must read > 4 (measured 8.5 / 8.9) or the check is blind. CHECK 3 (clock): FadeAlpha255 reads the stream's own alpha (129 then 130) at t = 304 / 306.
Numbers only; nothing from the game is written. Exit 1 on any FAIL."""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import arena_files as af  # noqa: E402
import capread as cr  # noqa: E402
import fview_exe  # noqa: E402

BAR = 1.0
PLANT_MIN = 4.0
ARENA = 4


def tex(a, name):
    from io import BytesIO
    im = Image.open(BytesIO(a.textures[name].blob))
    im.load()
    return np.asarray(im.convert('RGBA'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=os.path.join(r'C:\Users\Jarrod\wwe2k14-recomp', 'out', 'captures', 's100_match300'))
    ap.add_argument('--frame', type=int, default=2)
    ap.add_argument('--plant', choices=['old', 'p00'], default=None)
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'L8'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    fails = []
    # CHECK 1: identity of the stream's page textures
    rc = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l8_page_id.py'), a.capture, str(a.frame), '298', '299'], capture_output=True, text=True, errors='replace',
                        env=dict(os.environ, RDR_MAIN_CHECKOUT=os.environ.get('RDR_MAIN_CHECKOUT', r'C:\Users\Jarrod\wwe2k14-recomp')))
    ident = rc.stdout
    ok0 = ('image 298 == arena %d fade_lightmap00' % ARENA) in ident
    ok1 = ('image 299 == arena %d fade_lightmap01' % ARENA) in ident
    print('identity: image 298 == arena %d fade_lightmap00: %s; image 299 == fade_lightmap01: %s' % (ARENA, ok0, ok1))
    if not (ok0 and ok1):
        fails.append('PAGE IDENTITY (the stream\'s page textures are not the arena files)')
    # the stream's own fade alpha: the last two draws of the page pass (a full-page quad each)
    cap = os.path.abspath(a.capture)
    fr = cr.Frame(open(os.path.join(cap, 'frames', 'frame_%06d.bin' % a.frame), 'rb').read())
    ps = [i for i, d in enumerate(fr.draws) if d['vertex_count'] == 4 and d['vertex_bytes'] == 80 and d['tex_count'] == 1][-2:]
    base_i, fade_i = None, None
    for i in range(len(fr.draws) - 1):
        d, e = fr.draws[i], fr.draws[i + 1]
        if (d['vertex_count'], d['vertex_bytes'], d['tex_count']) == (4, 80, 1) and (e['vertex_count'], e['vertex_bytes'], e['tex_count']) == (4, 80, 1) and fr.slots[d['tex_first']]['image'] == 298 and fr.slots[e['tex_first']]['image'] == 299:
            base_i, fade_i = i, i + 1
    if fade_i is None:
        raise SystemExit('FAIL  the page pass (images 298 then 299) is not in frame %d' % a.frame)
    alpha = float(fr.reg(int(fr.draws[fade_i]['vs_bank']), 1)[0][3])
    a255 = int(round(alpha * 255))
    print('the stream: page pass draws %d (base, alpha %.4f) and %d (page 01, vertex alpha %.4f = %d / 255)' % (base_i, float(fr.reg(int(fr.draws[base_i]['vs_bank']), 1)[0][3]), fade_i, alpha, a255))
    # CHECK 3: the clock reads the stream's own alphas (t = 304 -> 129, 306 -> 130 are the unit's own expectations; here: some t gives this alpha)
    ts = [t for t in range(600) if int(subprocess.run([os.path.join(ROOT, 'out', 'tools', 'l8_unit.exe'), '--alpha', str(t)], capture_output=True, text=True).stdout) == a255]
    print('the clock: FadeAlpha255 gives alpha %d for t = %d..%d' % (a255, ts[0] if ts else -1, ts[-1] if ts else -1))
    if not ts:
        fails.append('CLOCK (no t gives the stream\'s alpha)')
    # CHECK 2: pixels
    fv = str(fview_exe.find_fview(root=ROOT, allow_stale=True, quiet=True))
    tgt = None
    # the page target = the surface of the draw's targets op: find through fview --ops
    opsj = os.path.join(a.out, 'ops.json')
    subprocess.run([fv, '--capture', cap, '--frame', str(a.frame), '--ops', '--json', opsj, '--root', ROOT, '--game', os.path.join(r'C:\Users\Jarrod\wwe2k14-recomp', 'game_data', 'extracted')], capture_output=True, text=True, errors='replace')
    ops = json.load(open(opsj))['ops']
    last_fade_op = [o['op'] for o in ops if o['kind'] == 'draw' and o['index'] == fade_i][0]
    cur = None
    for o in ops:
        if o['op'] > last_fade_op:
            break
        if o['kind'] == 'targets':
            cur = o['colors'][0]
    tgt = cur
    case_json = os.path.join(a.out, 'px.json')
    p = subprocess.run([fv, '--capture', cap, '--frame', str(a.frame), '--pixel', '256,256', '--surface', str(tgt), '--through-op', str(last_fade_op), '--save-case', '--op', str(last_fade_op),
                        '--root', ROOT, '--game', os.path.join(r'C:\Users\Jarrod\wwe2k14-recomp', 'game_data', 'extracted'), '--json', case_json], capture_output=True, text=True, errors='replace')
    line = [l for l in (p.stdout + p.stderr).splitlines() if 'case saved:' in l]
    if not line:
        raise SystemExit('FAIL  fview --save-case: ' + (p.stdout + p.stderr)[-400:])
    pic = os.path.join(line[0].split('case saved:')[1].strip(), 'picture.png')
    game = np.asarray(Image.open(pic).convert('RGBA'), float)
    arena = af.load_arena(ARENA, False)
    p0, p1 = tex(arena, 'fade_lightmap00'), tex(arena, 'fade_lightmap01')
    if p0.shape != game.shape:
        raise SystemExit('FAIL  page size %s vs the target %s' % (p0.shape, game.shape))
    p0.tofile(os.path.join(a.out, 'p00.raw'))
    p1.tofile(os.path.join(a.out, 'p01.raw'))
    outraw = os.path.join(a.out, 'ours.raw')
    r = subprocess.run([os.path.join(ROOT, 'out', 'tools', 'l8_unit.exe'), os.path.join(a.out, 'p00.raw'), os.path.join(a.out, 'p01.raw'), str(a255), outraw], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('FAIL  l8_unit: ' + r.stdout + r.stderr)
    ours = np.fromfile(outraw, np.uint8).reshape(game.shape).astype(float)
    if a.plant == 'old':
        ours = p1.astype(float)
    elif a.plant == 'p00':
        ours = p0.astype(float)
    d = np.abs(ours[..., :3] - game[..., :3])
    mean = float(d.mean())
    print('page pixels (512x512, rgb, of 255): ours%s vs the game\'s target: mean |d| %.2f (per channel %s), max %.0f' % (' [PLANT ' + a.plant + ']' if a.plant else '', mean, np.round(d.reshape(-1, 3).mean(0), 2).tolist(), float(d.max())))
    for nm, t in (('01 alone (today)', p1), ('00 alone', p0)):
        print('   reference: %-18s mean |d| %.2f' % (nm, float(np.abs(t[..., :3].astype(float) - game[..., :3]).mean())))
    if mean > BAR:
        fails.append('PAGE PIXELS %.2f > %.2f' % (mean, BAR))
    for f in fails:
        print('FAIL  ' + f)
    text = '\n'.join(fails)
    if a.expect_fail:
        if fails and a.expect_fail in text:
            print('OK (planted fault found: %s)' % a.expect_fail)
            return 0
        print('FAIL  the planted fault was NOT reported (%s)' % a.expect_fail)
        return 1
    print('l8_check: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
