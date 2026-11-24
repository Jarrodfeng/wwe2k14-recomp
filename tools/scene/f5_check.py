r"""S103 F5 check: particles drawn by OUR frame from the record alone, offline. 0 game runs.
  py -I tools\scene\f5_check.py [--root .]
1. blend codes from their writer (f5_blend_codes.py --check; plant mode_field must fail);
2. f5_unit.exe: the sprite library / NameIndex / guest hasher + namer on the player's own prtcls.pac (plants tiled, endian must fail);
3. the rig on run 28 frame 500 (a gameplay window: the stream holds NO particle draw, so there is no game flare to compare with; the record gets a labelled FIXTURE flare):
   feature_diff row 'particles' RAN with no_texture 0; the flare's corners stay unchanged (the named texture is soft) and its centre brightens; additive >= alpha pixelwise and the two differ
   (the blend is chosen by the code); twins: unnamed texture (white square: corners change) and wrong blend (code 1 drawn as alpha) must FAIL their checks.
"""
import os, re, subprocess, sys
import numpy as np
from PIL import Image

ROOT = os.path.abspath(sys.argv[sys.argv.index('--root') + 1]) if '--root' in sys.argv else os.getcwd()
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or r'C:\Users\Jarrod\wwe2k14-recomp'
CASE = os.path.join(MAIN, 'out', 's103', 'run28_case.zip')
ENTRY = 'p0100/110/1/Light0001'
OUT = os.path.join(ROOT, 'out', 's103', 'F5', 'gate')
bad = 0


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, **(env or {})), cwd=ROOT, timeout=1800)


def check(ok, what, detail=''):
    global bad
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '  : ' + detail if detail else ''))
    bad += 0 if ok else 1
    return ok


def fixture(tag, envs):
    d = os.path.join(OUT, tag)
    e = ['--env=%s=%s' % kv for kv in envs.items()]
    p = run([sys.executable, '-I', 'tools/scene/feature_diff.py', CASE, '--frames', '500', '--out', d] + e, {'RDR_MAIN_CHECKOUT': MAIN})
    img = np.asarray(Image.open(os.path.join(d, 'ours_500_whole.png')).convert('RGB')).astype(int)
    return img, p.stdout


def main():
    # 1
    r = run([sys.executable, '-I', 'tools/scene/f5_blend_codes.py', '--check'])
    check(r.returncode == 0 and 'CHECK PASS' in r.stdout, 'blend codes: only 0 / 1 / 2 occur in the writer\'s data; Kane smoke 0, fire 1 (block+12 of the emitter header)')
    r = run([sys.executable, '-I', 'tools/scene/f5_blend_codes.py', '--check', '--plant', 'mode_field'])
    check(r.returncode != 0, 'plant mode_field (the wrong header word) FAILS the blend-code check')
    # 2
    unit = os.path.join(ROOT, 'out', 'tools', 'f5_unit.exe')
    g = os.path.join(MAIN, 'game_data', 'extracted')
    r = run([unit, g])
    check(r.returncode == 0, 'f5_unit: sprite library, index and guest-memory naming on the player\'s own prtcls.pac', r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-200:])
    for pl in ('tiled', 'endian'):
        r = run([unit, g, '--plant', pl])
        check(r.returncode != 0, 'f5_unit plant %s FAILS the naming check' % pl)
    # CAWH: the PCMN / PCCH sections are in the index; the past-bug re-find = the c2d_menu capture's 10 particle images, 10 of 10 named (0 of 10 with the PCE6-only walk)
    cap = os.path.join(MAIN, 'out', 'captures', 'c2d_menu')
    menu = os.path.join(OUT, 'menu_imgs')
    mm = []
    if os.path.isdir(cap):
        rd = run([sys.executable, '-I', 'tools/particles/menu_particle_dump.py', cap, menu], {'RDR_MAIN_CHECKOUT': MAIN})
        check(rd.returncode == 0, 'menu_particle_dump: the c2d_menu capture\'s particle images', rd.stdout.strip()[-120:] or rd.stderr[-200:])
        mm = ['--menu', menu]
    r = run([unit, g] + mm)
    check(r.returncode == 0 and 'CAWH past-bug re-find' in r.stdout if mm else r.returncode == 0, 'f5_unit CAWH: PCE6 + PCMN + PCCH indexed, PCMN Smoke0004 loads' + (', c2d_menu 10 of 10 named' if mm else ' (capture absent: re-find skipped)'))
    r = run([unit, g, '--plant', 'pce6only'] + mm)
    check(r.returncode != 0 and (not mm or 'FAIL  CAWH past-bug re-find' in r.stdout), 'f5_unit plant pce6only (the old walk) FAILS the index and the 10-of-10 re-find')
    r = run([unit, g])
    check(r.returncode == 0 and 'INN: gINFE/110/1/Light0001' in r.stdout, 'f5_unit INN: the PCGE banks (INFE / GAME / RYAL / ECW_) are indexed, gINFE/110/1/Light0001 loads')
    r = run([unit, g, '--plant', 'nopcge'])
    check(r.returncode != 0 and 'FAIL  INN' in r.stdout, 'f5_unit plant nopcge (the old walk without PCGE) FAILS the INN checks')
    # 3
    base, _ = fixture('base', {})
    named, out_named = fixture('named', {'WWE_RDR_PARTICLE_FIXTURE': ENTRY})
    alpha, _ = fixture('alpha', {'WWE_RDR_PARTICLE_FIXTURE': ENTRY, 'WWE_RDR_PARTICLE_FIXTURE_BLEND': 'alpha'})
    sub, _ = fixture('sub', {'WWE_RDR_PARTICLE_FIXTURE': ENTRY, 'WWE_RDR_PARTICLE_FIXTURE_BLEND': 'subtract'})
    white, _ = fixture('white', {'WWE_RDR_PARTICLE_FIXTURE': 'none'})
    row = re.search(r'particles \(sprites / flares\)\s+game\s+0 draws\s+ours RAN\s+\(particle hook: batches (\d+) particles (\d+) draws (\d+) no_texture (\d+)', out_named)
    check(bool(row) and int(row.group(4)) == 0 and int(row.group(3)) > 0, 'feature_diff: the particle pass RAN in our frame (fixture flare), texture resolved by name (no_texture 0)', row.group(0)[:120] if row else out_named[-300:])

    def region(a, b):
        d = np.abs(a - b).sum(axis=2)
        ys, xs = np.nonzero(d > 12)
        return d, ys, xs
    d, ys, xs = region(named, base)
    n = len(ys)
    check(n > 200, 'the flare changes pixels (named, additive)', '%d pixels' % n)
    if n:
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
        bright = (named - base).sum(axis=2)
        check(bright[cy - 3:cy + 4, cx - 3:cx + 4].mean() > 60, 'the centre brightens (additive glow)', '%.0f' % bright[cy - 3:cy + 4, cx - 3:cx + 4].mean())
        # corners of the quad's box (the flare spans ~12 % of the picture height): the named texture is a soft glow, its corners stay put
        h = int(0.05 * 720 / 2)   # the flare's half-size in pixels (fixture default 5 % of the picture height)
        ring = np.zeros_like(d, bool)
        for (py, px) in ((cy - 0.9 * h, cx - 0.9 * h), (cy - 0.9 * h, cx + 0.9 * h), (cy + 0.9 * h, cx - 0.9 * h), (cy + 0.9 * h, cx + 0.9 * h)):
            py, px = int(py), int(px)
            if 0 <= py < d.shape[0] - 4 and 0 <= px < d.shape[1] - 4: ring[py:py + 4, px:px + 4] = True
        corner_named = d[ring].mean() if ring.any() else 0
        dw, _, _ = region(white, base)
        corner_white = dw[ring].mean() if ring.any() else 0
        check(corner_named < 0.3 * corner_white and corner_white > 100, 'named texture is soft at the corners (%.1f), the unnamed (white) twin is a square (%.1f)' % (corner_named, corner_white))
        check(not (corner_white < 100), 'twin unnamed texture FAILS the soft-corner check (a white stand-in changes the corners)', '%.1f' % corner_white)
    diff_ab = np.abs(named - alpha).sum()
    # S103 F5S (docs\research\s103\F5S.md): compared on the 3x3 box mean of sum(named - alpha), not pixelwise. The blend itself (SRC_ALPHA / ONE, ADD) can
    # never darken, but the picture is read AFTER the post combine (FXAA + DOF, post_ours.h), whose edge choices do not keep "brighter stays brighter"
    # per pixel: 10 edge pixels were already darker before MIP (min -2), and MIP's game-correct LOD bias (daf91b9fe) moved one to -21. Box mean: -0.1
    # before MIP, -5.1 after; a wrong blend (alpha drawn for additive) is caught by diff_ab and the twin below.
    s = (named.astype(np.int32) - alpha.astype(np.int32)).sum(axis=2) if named.ndim == 3 else (named.astype(np.int32) - alpha.astype(np.int32))
    sp = np.pad(s, 1, mode='edge')
    box = sum(sp[dy:dy + s.shape[0], dx:dx + s.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
    check(diff_ab > 5000 and box.min() >= -12, 'additive >= alpha (3x3 box mean, after the post combine) and the blends differ (the code selects the blend)', 'sum |diff| %d, box min %.1f, pixel min %d' % (diff_ab, box.min(), (named - alpha).min()))
    check(np.abs(alpha - named).sum() > 5000, 'twin wrong blend (code 1 drawn as alpha) FAILS: it is not the additive picture', '%d' % np.abs(alpha - named).sum())
    dsub = (sub - base).sum(axis=2)
    check(dsub.min() < -40 and dsub.max() < 40, 'code 2 (reverse-subtract, Vulkan op 2) DARKENS the flare area (a MAX op, the review bug, would brighten)', 'min %d max %d' % (dsub.min(), dsub.max()))
    print('f5_check: %s' % ('OK' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
