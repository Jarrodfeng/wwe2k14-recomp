r"""S102 lane L2: the ring-plane shadow bake (src\renderer_adapter\plane_bake.h), proved offline with fview headless. 0 game runs, 0 Surface runs.

  py -I tools\shadow\plane_bake_check.py [--root DIR] [--jobs 3] [--keep] [--calibrate]

Inputs (the MAIN checkout's out\, read in place; Check B's zip is unpacked into this tree's out\l2 and its plane oracle dumped there):
  Check B   out\s101\runs\20261111_215201_s100_owner_s89_brock\case_*.zip (341 frames; the only standard input with a live plane mask and its scene records)
  Bryan     out\lane86\bryan\capture (91 frames; the plane chain is BLANK: the lever must take 0 frames there and say blank). It has no scene records, so its arms use the record-free part of the bundle.
Every Check B arm is the SHIPPED DEFAULT BUNDLE (wwe2k14_app.h: wrestlers / shadow casters / shadows / rim / circles / rim chain / small passes / plane shadow ours); `half_*` arms add `--shadow-half` (the half rate).
Arms: off | nochain (every mask draw dropped: where the shadow chain matters) | dropropes / dropwrestler (the game's chain without one of its two proxies) | on (1024) | on2048 | nobake (the composite
ignores the bake: the lever's wrestler path alone) | the planted twins | wobble arms (the rope proxy's palette moved 8 units on replayed frames 110..160: the captures never move the ropes, so this is the only
way to drive the key offline) | half-rate off / on | sweeps for the alias and Bryan checks.
Checks (PASS / FAIL, numbers only):
  census      the lever takes frames, hits dominate, the bake's border holds no lit texel (read from the dumped raw bake), reads pointed == taken
  equal       the picture is byte-equal to off everywhere the shadow chain and the bake do not touch (outside R = dilate((off != nochain) | (nobake != on))) on every compared frame; the difference inside R is
              printed as a histogram
  footprint   the ropes sit where the game's do: the correlation (gaussian sigma 4 px) between the game's rope darkening (dropropes - off) and the bake's (nobake - on) stays over the bar, the energy ratio
              (sum bake / sum game) stays inside its bar; the wrong_arena and half_texel twins must fall under the correlation bar, ropes_kept over the energy bar
  wrestler    the mat still gets the wrestler's contact shadow: the darkening under the wrestler (dropwrestler - X over its pixels) of `on` is 0.6..1.4 of off's; no_wrestler must fall under 0.25
  key         under the wobble arm `on` re-bakes (> 1) and follows the ropes (correlation over the bar on the plateau frames); stale_key must fall under it
  order       bake_late: the composite before the bake: re-bake frames differ from `on`
  composite   composite_min differs from `on`
  half        with the half rate on, the lever's picture equals off outside R on the drawn frames and the lever declines the skipped ones (counted)
  alias       on + --copy-alias 2 byte-equal to on on every frame (hash sweep)
  bryan       the lever takes 0 frames on Bryan (blank counted) and the picture hash of every frame equals off
  validation  every arm: `validation errors 0` AND exit code 0 (fview writes its output before it exits 4)"""
import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import plane_bake_lines as pbl  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAILS = 0
REPORT = {}
BUNDLE = ['--wrestlers-ours', '--shadow-casters-ours', '--shadows-ours', '--rim-ours', '--circles-ours', '--rim-chain-ours', '--small-passes-ours', '--plane-shadow-ours']
BUNDLE_BRYAN = ['--shadows-ours', '--rim-chain-ours', '--small-passes-ours', '--plane-shadow-ours']   # (the standard Bryan capture has no scene records)
ROPE, WRESTLER = '0xaaf56230', '0xad424110'   # Check B's proxy objects (the rope proxy is the one with 4 eye points)
FRAMES = list(range(0, 341, 10))
WOBBLE = '110,160,8'
WOB_FRAMES = list(range(100, 181, 5)) + [111, 113, 115, 119, 121, 159, 161, 163]
PLATEAU = [125, 135, 145, 155]
CORR_BAR = 0.92           # clean footprint correlation at least this (Check B measures 0.947 at 1024 and 2048)
KEY_BAR = 0.92            # the same under the wobble (0.978 measured)
ENERGY_BAR = (0.6, 1.0)   # the bake's integrated darkening against the game's rope darkening (0.90 / 0.91 measured; the ropes drawn twice, a union, reach 1.06)
PLANT_DROP = 0.04         # a planted twin must lose at least this much correlation against the clean one


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1800, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def unpack(zip_path, dest):
    if os.path.isfile(os.path.join(dest, 'capture', 'images.bin')):
        return os.path.join(dest, 'capture')
    os.makedirs(dest, exist_ok=True)
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    rc, out = run([tar, '-xf', zip_path, '-C', dest], 1800)
    if rc or not os.path.isfile(os.path.join(dest, 'capture', 'images.bin')):
        raise RuntimeError('could not unpack %s: %s' % (zip_path, out[:300]))
    return os.path.join(dest, 'capture')


class Arm:
    def __init__(self, name, extra, frames=None, capture='checkb', frames_n=341):
        self.name, self.extra, self.frames, self.capture, self.frames_n = name, extra, frames if frames is not None else FRAMES, capture, frames_n
        self.dir = None
        self.census = ''
        self.valid = None
        self.rc = None


def fview_arm(fv, game, caps, oracles, work, arm, sweep=False):
    d = os.path.join(work, arm.name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    arm.dir = d
    cmd = [fv, '--capture', caps[arm.capture], '--game', game] + (BUNDLE if arm.capture == 'checkb' else BUNDLE_BRYAN) + ['--plane-oracle', oracles[arm.capture], '--validation', '1', '--frames', str(arm.frames_n)]
    if sweep:
        cmd += ['--sweep', os.path.join(d, 'sweep.csv')]
    else:
        cmd += ['--dump-frames', ','.join(map(str, arm.frames)), '--dump-dir', d]
    cmd += arm.extra
    out = ''
    for _ in range(8):   # the GPU is shared: retry ONLY the exact start-up refusal
        rc, out = run(cmd)
        if 'Not enough graphics memory for the frame ring' not in out:
            break
    arm.rc = rc
    arm.valid = 'validation errors 0' in out
    for ln in out.splitlines():
        if ln.startswith('[rdr][plane_bake]'):
            arm.census = ln
    open(os.path.join(d, 'log.txt'), 'w', encoding='utf-8').write(out)
    return arm


def num(census, key):
    m = re.search(key + r' (\d+)', census)
    return int(m.group(1)) if m else -1


def load(arm, f):
    return np.asarray(Image.open(os.path.join(arm.dir, 'frame_%05d.png' % f)).convert('RGB')).astype(np.int32)


def gray(a):
    return a.sum(2) / 3.0


def dilate(mask, r):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def gauss(a, s=4):
    k = np.exp(-0.5 * (np.arange(-3 * s, 3 * s + 1) / s) ** 2)
    k /= k.sum()
    a = np.apply_along_axis(lambda r: np.convolve(r, k, 'same'), 1, a)
    return np.apply_along_axis(lambda c: np.convolve(c, k, 'same'), 0, a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--calibrate', action='store_true', help='print the numbers and judge nothing against the bars')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    fv = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    if not os.path.isfile(fv):
        raise SystemExit('no fview.exe in this tree (tools\\fview\\build.ps1)')
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, z) for z in os.listdir(runs) if z.startswith('case_') and z.endswith('.zip')]
    bryan = os.path.join(main_co, 'out', 'lane86', 'bryan', 'capture')
    if not zips or not os.path.isfile(os.path.join(bryan, 'images.bin')):
        raise SystemExit('missing input: Check B zip or the Bryan capture (read from the main checkout)')
    work = os.path.join(root, 'out', 'l2')
    os.makedirs(work, exist_ok=True)
    caps = {'checkb': unpack(zips[0], os.path.join(work, 'checkb')), 'bryan': bryan}
    oracles = {'checkb': os.path.join(work, 'checkb.plane'), 'bryan': os.path.join(work, 'bryan.plane')}
    for k in caps:
        if not os.path.isfile(oracles[k]):
            rc, out = run([sys.executable, '-I', os.path.join(root, 'tools', 'shadow', 'plane_oracle.py'), 'dump', caps[k], oracles[k]])
            if rc:
                raise SystemExit('plane_oracle failed: ' + out[-400:])
    wob = ['--plane-bake-wobble', WOBBLE]
    on = ['--plane-bake']
    arms = [Arm('off', []), Arm('nochain', ['--draw-drop', '@class=all@site=0x82BF4D38']), Arm('dropropes', ['--draw-drop', '@class=all@object=' + ROPE]),
            Arm('dropwrestler', ['--draw-drop', '@class=all@object=' + WRESTLER]), Arm('on', on), Arm('on2048', ['--plane-bake=2048']), Arm('nobake', on + ['--plane-bake-fault', 'no_bake']),
            Arm('off_w', wob, WOB_FRAMES), Arm('dropropes_w', wob + ['--draw-drop', '@class=all@object=' + ROPE], WOB_FRAMES), Arm('on_w', on + wob, WOB_FRAMES),
            Arm('nobake_w', on + wob + ['--plane-bake-fault', 'no_bake'], WOB_FRAMES)]
    plants = {'wrong_arena': [], 'half_texel': [], 'no_wrestler': [], 'composite_min': [], 'ropes_kept': [], 'scale5': [], 'bake_late': wob, 'stale_key': wob}
    c7 = ['--plane-bake-wobble', '110,160,0', '--plane-bake-wobble-c7', '30', '--replace-fault', 'plane.no_verify']
    for p, ex in plants.items():
        arms.append(Arm('p_' + p, on + ['--plane-bake-fault', p] + ex, WOB_FRAMES if ex else FRAMES))
    arms += [Arm('w_c7', on + c7, WOB_FRAMES), Arm('p_centre_now', on + c7 + ['--plane-bake-fault', 'centre_now'], WOB_FRAMES),
             Arm('w_c1', on + ['--plane-bake-wobble', '110,160,0', '--plane-bake-wobble-c1', '0.2', '--replace-fault', 'plane.no_verify'], WOB_FRAMES)]
    arms += [Arm('half_off', ['--shadow-half']), Arm('half_on', ['--shadow-half'] + on), Arm('half_nochain', ['--shadow-half', '--draw-drop', '@class=all@site=0x82BF4D38']), Arm('half_nobake', ['--shadow-half'] + on + ['--plane-bake-fault', 'no_bake'])]
    sw = [Arm('sw_on', on), Arm('sw_on_alias', on + ['--copy-alias', '2']), Arm('sw_bry_off', [], capture='bryan', frames_n=91), Arm('sw_bry_on', on, capture='bryan', frames_n=91)]
    arms.append(Arm('bry_on', on, [0], 'bryan', 91))   # (a dump arm: the sweep mode prints no census)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(lambda arm: fview_arm(fv, game, caps, oracles, work, arm), arms))
        list(ex.map(lambda arm: fview_arm(fv, game, caps, oracles, work, arm, True), sw))
    A = {arm.name: arm for arm in arms + sw}
    for arm in arms + sw:
        line(bool(arm.valid) and arm.rc == 0, 'validation %-14s: errors 0 and exit code %s' % (arm.name, arm.rc))
    # ---- census
    c = A['on'].census
    rawp = os.path.join(A['on'].dir, 'bakeraw_00200.png')
    raw = np.asarray(Image.open(rawp).convert('RGB')).astype(int)[..., 0] if os.path.isfile(rawp) else None
    edge = int(raw[0].max() + raw[-1].max() + raw[:, 0].max() + raw[:, -1].max()) if raw is not None else -1
    line(num(c, 'taken') > 300 and num(c, 'hits') > 300 and edge == 0 and num(c, 'reads pointed') == num(c, 'taken'), 'census on: taken %d hits %d rebakes %d, reads pointed %d, lit texels on the raw bake\'s border %d (frame 200): %s' % (
        num(c, 'taken'), num(c, 'hits'), num(c, 'rebakes'), num(c, 'reads pointed'), edge, c[:200]))
    line(num(A['on2048'].census, 'taken') > 300, 'census on2048: taken %d' % num(A['on2048'].census, 'taken'))
    line(num(c, 'early reads') == 0 and num(A['on_w'].census, 'early reads') == 0, 'F6: reads of the game\'s map before the composite of a taken frame: %d (wobble arm %d), must be 0' % (num(c, 'early reads'), num(A['on_w'].census, 'early reads')))
    line(num(A['half_on'].census, 'taken') > 100 and num(A['half_on'].census, 'skip') > 100, 'census half rate: taken %d, declined as skipped %d (the mat keeps the last composite: kept %d)' % (num(A['half_on'].census, 'taken'), num(A['half_on'].census, 'skip'), num(A['half_on'].census, 'half-skip kept')))
    # ---- pictures
    def region(chain, x, nob, f):
        o, n, t, nb = load(A['off'], f), load(A[x], f), load(A[chain], f), load(A[nob], f)
        return dilate((np.abs(o - t).max(2) > 0) | (np.abs(nb - n).max(2) > 0), 2)
    def equal(x, chain, nob, frames):
        worst, hist, inside = 0, np.zeros(6, int), 0.0
        for f in frames:
            o, n = load(A['off'], f), load(A[x], f)
            R = region(chain, x, nob, f)
            d = np.abs(o - n).max(2)
            worst = max(worst, int(d[~R].max()) if (~R).any() else 0)
            hist += np.histogram(d[R], bins=[0, 1, 2, 4, 8, 16, 256])[0]
            inside += float(d[R].mean()) if R.any() else 0.0
        return worst, hist, inside / max(1, len(frames))
    for nm in ('on', 'on2048'):
        worst, hist, inside = equal(nm, 'nochain', 'nobake', FRAMES)
        print('  %-8s outside R max %d, inside R mean %.3f, histogram (0,1,2-3,4-7,8-15,16+) %s' % (nm, worst, inside, hist.tolist()))
        line(worst == 0, 'equal %-7s: byte-equal to off outside the shadow chain / bake region on %d frames (max difference there %d)' % (nm, len(FRAMES), worst))
    def foot(x, frames, off='off', dr='dropropes', nb='nobake'):
        cs, g, b = [], 0.0, 0.0
        for f in frames:
            dg = gray(load(A[dr], f)) - gray(load(A[off], f))
            db = gray(load(A[nb], f)) - gray(load(A[x], f))
            R = np.abs(dg) > 0.5
            g += float(dg[R].sum())
            b += float(db[R].sum())
            ga, gb = gauss(dg), gauss(db)
            m = (np.abs(ga) > 0.02) | (np.abs(gb) > 0.02)
            if m.sum() > 10 and ga[m].std() > 0 and gb[m].std() > 0:
                cs.append(float(np.corrcoef(ga[m], gb[m])[0, 1]))
        return (float(np.mean(cs)) if cs else 0.0), (b / g if g else 0.0)
    fr_f = [f for f in FRAMES if 150 <= f <= 330]
    res = {nm: foot(nm, fr_f) for nm in ('on', 'on2048', 'p_wrong_arena', 'p_half_texel', 'p_ropes_kept')}
    for nm, (cc, en) in res.items():
        print('  footprint %-14s corr %.3f  energy ratio %.2f' % (nm, cc, en))
    REPORT.update(corr_on=res['on'][0], energy_on=res['on'][1], corr_wrong_arena=res['p_wrong_arena'][0], corr_half_texel=res['p_half_texel'][0], energy_ropes_kept=res['p_ropes_kept'][1])
    line(a.calibrate or (res['on'][0] >= CORR_BAR and res['on2048'][0] >= CORR_BAR and ENERGY_BAR[0] <= res['on'][1] <= ENERGY_BAR[1]),
         'footprint: the baked rope shadow matches the game\'s: correlation %.3f (2048: %.3f, bar %.2f), energy ratio %.2f (bar %.1f..%.1f)' % (res['on'][0], res['on2048'][0], CORR_BAR, res['on'][1], ENERGY_BAR[0], ENERGY_BAR[1]))
    for p in ('wrong_arena', 'half_texel'):
        line(a.calibrate or res['p_' + p][0] <= min(CORR_BAR, res['on'][0] - PLANT_DROP), 'footprint twin %-11s: correlation %.3f must fall under %.3f' % (p, res['p_' + p][0], min(CORR_BAR, res['on'][0] - PLANT_DROP)))
    line(a.calibrate or res['p_ropes_kept'][1] > ENERGY_BAR[1], 'footprint twin ropes_kept : energy ratio %.2f must exceed %.1f (the ropes drawn twice)' % (res['p_ropes_kept'][1], ENERGY_BAR[1]))
    # line geometry (Jarrod's bar: the same rope lines, same positions, same width): centre offset and width, off against on, along rows of the mat
    def lines_of(x, frames, off='off', dr='dropropes', nb='nobake'):
        offs, wid, peak = [], [], []
        for f in frames:
            n_ = 'frame_%05d.png' % f
            s = pbl.summary(pbl.measure(*[os.path.join(A[k].dir, n_) for k in (off, dr, nb, x)]))
            if s['n']:
                offs.append(s['off_med'])
                wid.append(s['width_ratio'])
                peak.append(s['peak_ratio'])
        return (float(np.mean(offs)) if offs else 99.0), (float(np.mean(wid)) if wid else 0.0), (float(np.mean(peak)) if peak else 0.0)
    lf = [f for f in FRAMES if 150 <= f <= 330]
    LN = {nm: lines_of(nm, lf) for nm in ('on', 'on2048', 'p_scale5', 'p_wrong_arena', 'p_half_texel')}
    for nm, (o_, w_, p_) in LN.items():
        print('  lines %-14s centre offset %.2f px, width ratio %.2f, peak ratio %.2f' % (nm, o_, w_, p_))
    REPORT.update(line_offset_on=LN['on'][0], line_width_on=LN['on'][1], line_offset_scale5=LN['p_scale5'][0])
    line(a.calibrate or (LN['on'][0] <= 1.5 and LN['on2048'][0] <= 1.5 and 0.9 <= LN['on'][1] <= 1.1 and 0.9 <= LN['on2048'][1] <= 1.1),
         'lines: the baked rope lines sit where the game\'s do and are as wide: centre offset %.2f px (2048: %.2f, bar 1.5 at 720p), width ratio %.2f (2048: %.2f, bar 0.9..1.1)' % (LN['on'][0], LN['on2048'][0], LN['on'][1], LN['on2048'][1]))
    line(a.calibrate or LN['p_scale5'][0] > 1.5 or not (0.9 <= LN['p_scale5'][1] <= 1.1), 'lines twin scale5 (5 %% scale error): centre offset %.2f px / width ratio %.2f must break a bar' % (LN['p_scale5'][0], LN['p_scale5'][1]))
    line(a.calibrate or LN['p_half_texel'][0] > 1.5, 'lines twin half_texel (2 game texels): centre offset %.2f px must exceed 1.5' % LN['p_half_texel'][0])
    # wrestler contact
    def wr(x, frames):
        n_, d_ = 0.0, 0.0
        for f in frames:
            o, n2, w_ = gray(load(A['off'], f)), gray(load(A[x], f)), gray(load(A['dropwrestler'], f))
            M_ = dilate(np.abs(o - w_) > 3, 1)
            if M_.any():
                n_ += float((w_ - n2)[M_].mean())
                d_ += float((w_ - o)[M_].mean())
        return n_ / d_ if d_ else 0.0
    wf = [f for f in FRAMES if 160 <= f <= 340]
    r_on, r_nw = wr('on', wf), wr('p_no_wrestler', wf)
    line(a.calibrate or 0.6 <= r_on <= 1.4, 'wrestler: the mat darkening under the wrestler is %.2f of off\'s with the lever on (bar 0.6..1.4)' % r_on)
    line(a.calibrate or r_nw < 0.25, 'wrestler twin no_wrestler: %.2f of off\'s (must be under 0.25)' % r_nw)
    # key
    cw = A['on_w'].census
    line(num(cw, 'rebakes') > 1, 'key: the wobble arm re-bakes %d times (hits %d)' % (num(cw, 'rebakes'), num(cw, 'hits')))
    kw = {nm: foot(nm, PLATEAU, 'off_w', 'dropropes_w', 'nobake_w') for nm in ('on_w', 'p_stale_key', 'p_bake_late')}
    for nm, (cc, en) in kw.items():
        print('  wobble %-12s corr %.3f  energy %.2f' % (nm, cc, en))
    line(a.calibrate or kw['on_w'][0] >= KEY_BAR, 'key: under the wobble the bake follows the ropes: correlation %.3f (bar %.2f)' % (kw['on_w'][0], KEY_BAR))
    line(a.calibrate or kw['p_stale_key'][0] <= kw['on_w'][0] - 0.08, 'key twin stale_key: correlation %.3f must fall under %.3f' % (kw['p_stale_key'][0], kw['on_w'][0] - 0.08))
    extra = num(cw, 'rebakes') - num(A['on'].census, 'rebakes')
    line(0 <= extra <= 10 and num(cw, 'unsteady') > 0, 'F8: a moving rope costs a handful of extra re-bakes (%d over the %d the rope\'s own sway needs, bar 0..10) and the game draws it while it moves (unsteady frames %d)' % (extra, num(A['on'].census, 'rebakes'), num(cw, 'unsteady')))
    c7f = [130, 140, 150, 170, 180]
    k7, k7n = foot('w_c7', c7f), foot('p_centre_now', c7f)
    line(a.calibrate or (k7[0] >= CORR_BAR and num(A['w_c7'].census, 'rebakes') > num(A['on'].census, 'rebakes')), 'F2: the plane position c7.x moved by 30: the bake is re-made around it and the composite reads it there: footprint correlation %.3f (bar %.2f), re-bakes %d (without the move %d)' % (k7[0], CORR_BAR, num(A['w_c7'].census, 'rebakes'), num(A['on'].census, 'rebakes')))
    line(a.calibrate or k7n[0] <= k7[0] - PLANT_DROP, 'F2 twin centre_now (the composite\'s centre 30 units off the bake\'s): correlation %.3f must fall under %.3f' % (k7n[0], k7[0] - PLANT_DROP))
    line(num(A['w_c1'].census, 'rebakes') >= 2, 'F3: c1.x (the fade array, not a translation) moved by 0.2: the key re-bakes (%d re-bakes, must be > 1)' % num(A['w_c1'].census, 'rebakes'))
    d_late = sum(int(np.abs(load(A['on_w'], f) - load(A['p_bake_late'], f)).max(2).max() > 0) for f in (111, 113, 115, 119, 121, 159, 161, 163))
    line(a.calibrate or d_late > 0, 'order twin bake_late: %d of 8 re-bake frames differ from `on` (must be > 0)' % d_late)
    dm = sum(int(np.abs(load(A['on'], f) - load(A['p_composite_min'], f)).max(2).max() > 0) for f in FRAMES)
    line(a.calibrate or dm > 0, 'composite twin composite_min: %d of %d frames differ from `on` (must be > 0)' % (dm, len(FRAMES)))
    # half rate
    hf = [f for f in FRAMES if 20 <= f <= 330]
    worst, one_level, two_plus = 0, 0, 0
    for f in hf:
        o, n, t = load(A['half_off'], f), load(A['half_on'], f), load(A['half_nochain'], f)
        R = dilate((np.abs(o - t).max(2) > 0), 6) | dilate(np.abs(load(A['half_nobake'], f) - load(A['half_on'], f)).max(2) > 0, 6)
        d = np.abs(o - n).max(2)
        worst = max(worst, int(d[~R].max()) if (~R).any() else 0)
        one_level += int(((d == 1) & R).sum())
        two_plus += int(((d > 1) & ~R).sum())
    line(a.calibrate or worst == 0, 'half rate: byte-equal (0 levels) to the half-rate off outside dilate(region, 6) on %d frames (max %d, pixels over 1 level outside %d); the 1-level pixels (%d) all lie inside it' % (len(hf), worst, two_plus, one_level))
    # alias / bryan by hashes
    def hashes(arm):
        rows = list(csv.reader(open(os.path.join(arm.dir, 'sweep.csv'))))
        return [r[:2] for r in rows[1:] if r and r[0].isdigit()]
    line(hashes(A['sw_on']) == hashes(A['sw_on_alias']) and len(hashes(A['sw_on'])) > 300, 'alias: --copy-alias 2 with the lever on equals the in-place run on every frame (%d)' % len(hashes(A['sw_on'])))
    line(hashes(A['sw_bry_off']) == hashes(A['sw_bry_on']) and len(hashes(A['sw_bry_on'])) > 80, 'bryan: pictures byte-equal off vs on on %d frames' % len(hashes(A['sw_bry_on'])))
    cb = A['bry_on'].census
    line(num(cb, 'taken') == 0 and num(cb, 'blank') > 0, 'bryan census: ' + cb[:210])
    print('REPORT', json.dumps({k: round(float(v), 4) for k, v in REPORT.items()}))
    if not a.keep:
        for d in os.listdir(work):
            if d not in ('checkb', 'checkb.plane', 'bryan.plane') and os.path.isdir(os.path.join(work, d)):
                shutil.rmtree(os.path.join(work, d), ignore_errors=True)
    print('plane_bake_check: %d failure(s)' % FAILS)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
