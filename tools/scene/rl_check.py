#!/usr/bin/env python3
r"""Lane RL (Session 103): the ring / props darker class (Jarrod's 5th blocker, second class), offline (0 game runs): frame_rig.exe on one match frame, the game's own picture beside it.

  py -I tools\scene\rl_check.py --case out\s103\run29_case.zip --frame 500 [--plant follow|props|both] [--expect-fail] [--out DIR]
  py -I tools\scene\rl_check.py --capture C:\...\out\captures\a08_match --frame 100 [--arena-note]

What it answers: are the arena draws that carry a camera-attached light (apron / covers / turnbuckles, set 31; the corner posts, sets 125-131) and the steps (props) lit like the game's draws of the same frame?
  1. DifLghVec, EVERY set our draws feed with a camera-attached light ([mats-vec] follow flags, WWE_RDR_DUMP_MATS): for each such set the game's yProp draws whose DifLghCol0 / SkyCol are that set's colours
     (read from OUR [mats-lit] line of the draw: no colour is typed here) give the candidate direction blocks; the error is max over our draws of the set of the best candidate's max abs difference over lights 0-3,
     after the Y mirror (ours are viewer space). PASS <= 0.01. A set with no game draw is reported and not counted; no set at all with a game draw = FAIL.
  2. Props set: our ring steps draws' DifLghCol0 / 1 / 3, SkyCol, GroundCol against the game's steps group (the yProp_BumpMap_alpha_gloss_reflect* draws). PASS <= 0.005.
  3. Luminance, per family (ring, props): the game picture's mean luminance over the family's own pixels (our per-family panel, non-clear) divided by ours over the same pixels.
     Printed (no bar: the families hold pieces this lane does not touch), so before / after can be compared.
Plants (each must FAIL check 1 or 2): follow = WWE_RDR_NO_ARENA_FOLLOW=1 (the rig sets rf::c::Plants::no_arena_follow: the stored directions unturned), props = WWE_RDR_PROPS_SET100=1
(the rig sets assets::load_detail::PropsSet100Plant: the old unlit set 100).
"""
import argparse, json, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLEAR = (31, 36, 46)

LIT = re.compile(r'\[mats-lit\] (\S+) part=(.+?) set_idx=\S+ id=(-?\d+) col=\[(\S+) \S+ \S+\|(\S+) \S+ \S+\|\S+ \S+ \S+\|(\S+) \S+ \S+\] sky=(\S+) \S+ \S+ gnd=(\S+)')
VEC = re.compile(r'\[mats-vec\] part=(.+?) id=(-?\d+) follow=(\d{4})/(\d) amb=\S+ dif=\S+ v0=(\S+) (\S+) (\S+) v1=(\S+) (\S+) (\S+) v2=(\S+) (\S+) (\S+) v3=(\S+) (\S+) (\S+)')


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def parse_ours(text):
    """One record per draw: the [mats-lit] line (colours) and the [mats-vec] line the front end prints right after it (directions)."""
    draws, pend = [], None
    for line in text.splitlines():
        m = LIT.match(line)
        if m:
            pend = dict(effect=m.group(1), part=m.group(2), id=int(m.group(3)), col=[float(m.group(i)) for i in (4, 5, 6)], sky=float(m.group(7)), gnd=float(m.group(8)))
            continue
        m = VEC.match(line)
        if m and pend and pend['part'] == m.group(1) and pend['id'] == int(m.group(2)):
            pend['follow'] = m.group(3)
            pend['follow_view'] = int(m.group(4))
            pend['vec'] = np.array([float(x) for x in m.groups()[4:]]).reshape(4, 3) * np.array([1, -1, 1])   # viewer space -> the game's Y
            draws.append(pend)
            pend = None
    return draws


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None, help='an unpacked capture folder')
    ap.add_argument('--case', default=None, help='a case zip (out/s103/run29_case.zip): frames 0..FRAME and the record of FRAME are unpacked once into out/rl/<zip name> (scratch)')
    ap.add_argument('--frame', type=int, default=500)
    ap.add_argument('--plant', choices=['follow', 'props', 'both'], default=None)
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    if a.case:   # unpack what the rig reads (fview replays frames 0..N; the rig draws record N)
        import zipfile
        base = os.path.join(ROOT, 'out', 'rl', os.path.splitext(os.path.basename(a.case))[0] + '_unpack')
        zp = a.case if os.path.isabs(a.case) else os.path.join(os.environ.get('RDR_MAIN_CHECKOUT', ROOT), a.case)
        with zipfile.ZipFile(zp) as z:
            for e in z.infolist():
                m = re.search(r'_(\d{6})\.(bin|rsc)$', e.filename)
                if m and not ((m.group(2) == 'bin' and int(m.group(1)) <= a.frame) or int(m.group(1)) == a.frame):
                    continue
                if not os.path.exists(os.path.join(base, e.filename)):
                    z.extract(e, base)
        a.capture = os.path.join(base, 'capture')
    if not a.capture:
        ap.error('--capture or --case')
    out = a.out or os.path.join(ROOT, 'out', 'rl', 'check_%s' % (a.plant or 'fix'))
    env = dict(os.environ)
    env.update({'WWE_RDR_DUMP_MATS': '1', 'WWE_RDR_L2_LOG': '1', 'WWE_FVIEW_ALLOW_STALE': '1'})
    if a.plant in ('follow', 'both'):
        env['WWE_RDR_NO_ARENA_FOLLOW'] = '1'   # read by frame_rig.cpp (the rig), which sets the planted flag; the game exe has no such read
    if a.plant in ('props', 'both'):
        env['WWE_RDR_PROPS_SET100'] = '1'
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', a.capture, '--frames', str(a.frame), '--repeat', '1', '--out', out],
                       capture_output=True, text=True, errors='replace', env=env, timeout=1800)
    work = os.path.join(out, 'work_' + os.path.basename(os.path.normpath(a.capture)))
    errf = os.path.join(work, 'ours_%d_stderr.txt' % a.frame)
    if not os.path.exists(errf):
        print('FAIL  rig run gave no stderr file (%s): %s' % (errf, (p.stdout or '')[-500:]))
        return 1
    text = open(errf, encoding='utf8', errors='replace').read()
    lm = os.path.join(work, 'lm')
    if not os.path.exists(os.path.join(lm, 'frame_%d.json' % a.frame)):   # the game's draw constants of the frame (frame_rig.py writes them only with --look-from-stream)
        os.makedirs(lm, exist_ok=True)
        subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'lightmatch', 'lm_extract.py'), a.capture, '--frames', str(a.frame), '--out', lm],
                       capture_output=True, text=True, errors='replace', env=env, timeout=900)
    groups = json.load(open(os.path.join(lm, 'frame_%d.json' % a.frame)))['groups']

    def gv(g, k):
        v = g['vals'].get(k)
        return None if v is None else np.array(v.get('v') or v.get('f'), dtype=np.float64)
    fails = []
    ours = parse_ours(text)
    gprops = [g for g in groups if g.get('effect', '').startswith(('yProp', 'yBG')) and gv(g, 'DifLghVec') is not None and gv(g, 'DifLghCol') is not None and gv(g, 'SkyCol') is not None]
    # 1. every set that carries a camera-attached light
    sets = {}
    for d in ours:
        if d['id'] >= 0 and d['follow'] != '0000' and d['part'].startswith('slot'):   # (not on follow_view: the planted twin leaves it off and the directions must then FAIL against the game's, not vanish)
            sets.setdefault(d['id'], []).append(d)
    checked = 0
    for sid in sorted(sets):
        errs = []
        for d in sets[sid]:
            fl = [i for i in range(4) if d['follow'][i] == '1']
            fx = [i for i in range(4) if d['follow'][i] != '1']
            cand = []
            for g in gprops:   # the twin: the set's colours AND the world-fixed lights (neighbouring sets share colours: 125-128) ...
                if abs(gv(g, 'DifLghCol')[0] - d['col'][0]) >= 0.003 or abs(gv(g, 'SkyCol')[0] - d['sky']) >= 0.003:
                    continue
                gd = gv(g, 'DifLghVec').reshape(4, 4)[:, :3]
                if all(float(np.abs(d['vec'][i] - gd[i]).max()) <= 0.01 for i in fx):
                    cand.append(gd)
            if cand:   # ... and the error is taken over the FLAGGED lights, the ones this lane turns
                errs.append(min(max(float(np.abs(d['vec'][i] - c[i]).max()) for i in fl) for c in cand))
        if not errs:
            print('  set %d (follow %s): %d of our draws, no game draw with its colours and world-fixed lights in this frame (not counted)' % (sid, sets[sid][0]['follow'], len(sets[sid])))
            continue
        e = max(errs)
        checked += 1
        print('%s  set %d follow %s flagged-light DifLghVec (%d of our draws, %d with a game twin; slots %s): max |ours - game| %.4f (bar 0.01)' % ('PASS' if e <= 0.01 else 'FAIL', sid, sets[sid][0]['follow'], len(sets[sid]), len(errs),
              ','.join(sorted({d['part'].split()[1] for d in sets[sid]})[:8]), e))
        if e > 0.01:
            fails.append('follow set %d' % sid)
    if not checked:
        fails.append('follow: no set with a camera-attached light has a game draw (ours: sets %s, game yProp groups %d)' % (sorted(sets), len(gprops)))
    # 2. the props set (the ring steps)
    st = [d for d in ours if d['part'].startswith('ring steps')]
    gs = [g for g in groups if g.get('effect', '').startswith('yProp_BumpMap')]   # the steps' draws (yProp_BumpMap_alpha_gloss_reflect*)
    gs = [g for g in gs if gv(g, 'DifLghCol') is not None and gv(g, 'SkyCol') is not None and gv(g, 'GroundCol') is not None]
    if not st or not gs:
        fails.append('props: our steps draws %d, game steps groups %d' % (len(st), len(gs)))
    else:
        d = st[0]
        # LIT keeps col0 / col1 / col3 of the set (rows 0, 1, 3): the game's DifLghCol rows are 0, 4, 12
        o = np.array(d['col'] + [d['sky'], d['gnd']])
        best = None
        for g0 in gs:
            g = np.array([gv(g0, 'DifLghCol')[0], gv(g0, 'DifLghCol')[4], gv(g0, 'DifLghCol')[12], gv(g0, 'SkyCol')[0], gv(g0, 'GroundCol')[0]])
            err = float(np.abs(o - g).max())
            if best is None or err < best[0]:
                best = (err, g)
        print('%s  props (ring steps) set id %d: ours col0/col1/col3/sky/gnd %s vs game %s: max %.4f (bar 0.005; %d game steps groups)' % ('PASS' if best[0] <= 0.005 else 'FAIL', d['id'], np.round(o, 3).tolist(), np.round(best[1], 3).tolist(), best[0], len(gs)))
        if best[0] > 0.005:
            fails.append('props set')
    # 3. luminance per family
    gimg = os.path.join(work, 'game', 'frame_%05d.png' % a.frame)
    whole = os.path.join(work, 'ours_%d_whole.png' % a.frame)
    if os.path.exists(gimg) and os.path.exists(whole):
        G = np.asarray(Image.open(gimg).convert('RGB'), dtype=np.float64) / 255.0
        W = np.asarray(Image.open(whole).convert('RGB'), dtype=np.float64) / 255.0
        for fam in ('ring', 'props'):
            fp = os.path.join(work, 'ours_%d_%s.png' % (a.frame, fam))
            if not os.path.exists(fp):
                continue
            F = np.asarray(Image.open(fp).convert('RGB'))
            if F.shape != W.shape or G.shape != W.shape:
                print('  luminance %s: size mismatch' % fam)
                continue
            mask = np.any(F != np.array(CLEAR), axis=-1)
            mask &= np.all(np.abs(F.astype(np.float64) / 255.0 - W) < 1e-6, axis=-1)   # the family's pixels that survive in the whole picture
            n = int(mask.sum())
            if n < 50:
                print('  luminance %s: %d pixels' % (fam, n))
                continue
            lg, lo = float(lum(G)[mask].mean()), float(lum(W)[mask].mean())
            print('  luminance %s: %d px, game %.4f ours %.4f, game / ours %.3f' % (fam, n, lg, lo, lg / max(lo, 1e-6)))
    ok = not fails
    if a.expect_fail:
        print('%s  planted %s: %s' % ('PASS' if not ok else 'FAIL', a.plant, 'failed as it must (%s)' % ', '.join(fails) if fails else 'did NOT fail'))
        return 0 if not ok else 1
    if fails:
        print('FAIL  ' + '; '.join(fails))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
