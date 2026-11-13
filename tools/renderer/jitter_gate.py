"""lane 185: the DLSS jitter window offline (docs\\renderer\\WIDE_OURS.md section 7.5), 0 game runs, no DLSS runtime.
rplay --jitter PHASE applies jitter_rule.h (the live reader's rule) to a capture and counts / draws it:
  count     per capture, draws the jitter moves by class: legacy window (--jitter-legacy, the S10b rule, the plant) against the new one (the post chain's last pass).
            New: 0 moved after the marker; Bryan's wrestlers / props / particles moved (legacy 0); 'checkb' = s100_s89k_a (a match: about +18 draws a frame, the particles), Brock = brock_s15_300 (+598 a frame); Brock's wrestlers
            (one layer) equal in both.
  shift     Bryan frame 50, phase 3: the sub-pixel shift of the picture against the unjittered render, measured by a gradient fit (Lucas-Kanade) in boxes on the
            wrestler and on the background. New window: both move together (difference under 0.05 px). Legacy (the past bug): the wrestler stays put and the
            background moves by the jitter. The legacy plant must fail the 'together' line.
  velocity  binder.h velocity_draws_ (the wrestlers' motion draws DLSS gets): new window greater than legacy on Bryan (two layers); equal on Brock.
  hud       a marker-late plant (--jitter-marker-late: the window never closes at the marker) must move HUD draws after the marker; the real rule moves none.
  identity  no --jitter byte-equal to the baseline rplay (--base-rplay) at 16:9 scale 1 and 2; --jitter-zero (a jitter of 0 through the whole path) equal to no jitter at 16:9 and 21:9, scale 1 and 2.
usage: py -I tools\\renderer\\jitter_gate.py [--rplay EXE] [--base-rplay EXE] [--root DIR] [--out DIR]
Exit 0 when every line passes. Captures are read from the main checkout's out\\captures and out\\cases (the same ones wide_gate.py uses)."""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image


def run(cmd, timeout=1200):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, p.stdout + p.stderr


def gray(path):
    return np.asarray(Image.open(path).convert('L'), dtype=np.float64)


def blur(a):
    k = np.array([1, 4, 6, 4, 1], float) / 16
    a = np.apply_along_axis(lambda r: np.convolve(r, k, 'same'), 1, a)
    return np.apply_along_axis(lambda r: np.convolve(r, k, 'same'), 0, a)


def shift_of(a, b, box):
    """The sub-pixel displacement (dx, dy) of picture b against picture a inside box (x0, y0, x1, y1): a global gradient fit, one step (shifts under 1 px)."""
    x0, y0, x1, y1 = box
    A, B = blur(a)[y0:y1, x0:x1], blur(b)[y0:y1, x0:x1]
    gy, gx = np.gradient((A + B) / 2)
    d = B - A
    m = np.array([[(gx * gx).sum(), (gx * gy).sum()], [(gx * gy).sum(), (gy * gy).sum()]])
    v = np.array([(gx * d).sum(), (gy * d).sum()])
    return -np.linalg.solve(m, v)


# Bryan frame 50 at 1280 x 720 (the picture: Bryan's jacket and torso in the middle, the crowd and the screen behind, the HUD along the bottom)
WRESTLER = (560, 260, 800, 540)
BACKGROUNDS = {'crowd-right': (930, 100, 1270, 380), 'crowd-left': (10, 120, 230, 330), 'doorway': (290, 40, 470, 360)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=None)
    ap.add_argument('--rplay', default=None)
    ap.add_argument('--base-rplay', default=None)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    main_root = os.path.normpath(os.path.join(repo, '..', '..', '..'))
    root = a.root or (main_root if os.path.isdir(os.path.join(main_root, 'out', 'captures')) else repo)
    rplay = a.rplay or os.path.join(repo, 'out', 'renderer', 'release', 'rplay.exe')
    out = a.out or os.path.join(repo, 'out', 'l185', 'gate')
    os.makedirs(out, exist_ok=True)
    caps = {'bryan': os.path.join(root, 'out', 'cases', 'case_20261111_174607_799_0_pc_run_exit', 'capture'),
            'brock': os.path.join(root, 'out', 'captures', 'brock_s15_300'),
            'checkb': os.path.join(root, 'out', 'captures', 's100_s89k_a')}
    frames = {'bryan': 60, 'brock': 16, 'checkb': 6}
    fails = [0]

    def line(ok, text):
        fails[0] += 0 if ok else 1
        print(('PASS ' if ok else 'FAIL ') + text, flush=True)

    for k, p in caps.items():
        if not os.path.isdir(p):
            line(False, 'capture %s missing (%s)' % (k, p))
            return 1

    def census(name, extra):
        cmd = [rplay, '--capture', caps[name], '--frames', str(frames[name]), '--jitter', '3', '--root', root] + extra
        rc, text = run(cmd)
        if rc:
            raise RuntimeError('rplay %s %s failed (%d): %s' % (name, extra, rc, text[-400:]))
        res = {'classes': {}}
        for ln in text.splitlines():
            m = re.match(r'\[jitter\] class (\S+) draws (\d+) moved (\d+) after_copy (\d+) after_marker (\d+) left_after_copy (\d+)', ln)
            if m:
                res['classes'][m.group(1)] = dict(draws=int(m.group(2)), moved=int(m.group(3)), after_copy=int(m.group(4)), after_marker=int(m.group(5)))
            m = re.match(r'\[jitter\] mode \S+.*: (\d+) frames, candidate draws (\d+), moved (\d+) \(after the first depth copy (\d+), after the marker (\d+)\)', ln)
            if m:
                res['frames'], res['draws'], res['moved'], res['after_copy'], res['after_marker'] = map(int, m.groups())
            m = re.match(r'\[jitter\] velocity_draws_ total (\d+)', ln)
            if m:
                res['velocity'] = int(m.group(1))
        return res

    WR = ('wrestler_gear', 'wrestler_skin', 'wrestler_reflect_trans')
    res = {}
    for name in ('bryan', 'brock', 'checkb'):
        res[name] = (census(name, []), census(name, ['--jitter-legacy']))
    for name in ('bryan', 'brock', 'checkb'):
        new, leg = res[name]
        n = new['frames']
        extra = (new['moved'] - leg['moved']) / n
        ok = new['after_marker'] == 0 and leg['after_marker'] == 0
        line(ok, 'count %s: moved after the marker new %d legacy %d (must be 0); new moves %d, legacy %d, +%.1f draws a frame' % (name, new['after_marker'], leg['after_marker'], new['moved'], leg['moved'], extra))
        wn = sum(new['classes'].get(c, {}).get('moved', 0) for c in WR)
        wl = sum(leg['classes'].get(c, {}).get('moved', 0) for c in WR)
        pn = new['classes'].get('particle', {}).get('moved', 0)
        pl = leg['classes'].get('particle', {}).get('moved', 0)
        if name == 'bryan':
            line(wl == 0 and wn > 0 and pn > pl, 'count bryan wrestler draws moved: legacy %d, new %d (two layers: the wrestlers are drawn after the first depth copy); particles legacy %d new %d' % (wl, wn, pl, pn))
        elif name == 'brock':
            line(wn == wl and 500 <= extra <= 700, 'count brock wrestler draws moved: legacy %d, new %d (one layer: equal); +%.1f draws a frame (expected about +598 here: the capture particles, 9572 in 16 frames; lane 181 +282 was another, longer Brock run)' % (wl, wn, extra))
        else:
            line(wn == wl and 12 <= extra <= 26, 'count checkb wrestler draws moved: legacy %d, new %d (equal); +%.1f draws a frame (expected about +18: the particles; the capture s100_s89k_a, a match, 108 in 6 frames)' % (wl, wn, extra))
    # velocity
    nb, lb = res['bryan']
    line(nb['velocity'] > lb['velocity'], 'velocity bryan: velocity_draws_ over %d frames legacy %d, new %d (the layer-2 wrestlers join the motion pass)' % (frames['bryan'], lb['velocity'], nb['velocity']))
    nk, lk = res['brock']
    line(nk['velocity'] >= lk['velocity'], 'velocity brock: legacy %d, new %d (one layer: not fewer)' % (lk['velocity'], nk['velocity']))
    # the HUD plant
    late = census('checkb', ['--jitter-marker-late'])
    line(late['after_marker'] > 0 and res['checkb'][0]['after_marker'] == 0, 'hud plant (marker never noted) moves %d draws after the marker on checkb; the real rule moves %d' % (late['after_marker'], res['checkb'][0]['after_marker']))
    # a frame with a depth copy and no marker (--jitter-chain: the armed state carries across frames, --jitter-marker-late: the marker is never seen): nothing is jittered
    nomark = census('bryan', ['--jitter-chain', '--jitter-marker-late'])
    chain = census('bryan', ['--jitter-chain'])
    line(nomark['moved'] == 0, 'no-marker frames (depth copy, no marker, chained): moved %d draws over %d frames (must be 0: DLSS cannot run on them, their HUD-phase draws must not shimmer)' % (nomark['moved'], nomark['frames']))
    line(0 < chain['moved'] < res['bryan'][0]['moved'], 'chained frames (armed only after a frame with its marker): moved %d, fewer than the unchained %d (the first frame follows no marker)' % (chain['moved'], res['bryan'][0]['moved']))
    # the audience composite (Bryan f0 draw 1453: a pixel-placed full-view quad, PS 7ABDFEFB yAudience_color_alpha): left, and moved by the plant
    def draw_verdict(extra):
        rc, text = run([rplay, '--capture', caps['bryan'], '--frames', '1', '--jitter', '3', '--jitter-census', '--root', root] + extra)
        for ln in text.splitlines():
            if ln.startswith('[jitter] draw 1453 '):
                return ln
        return ''
    v_new, v_plant = draw_verdict([]), draw_verdict(['--jitter-no-fullview'])
    line('7ABDFEFBCADB4BF9' in v_new and v_new.endswith('-> left') and v_plant.endswith('-> MOVED'), 'full-view composite (bryan f0 draw 1453, PS 7ABDFEFB yAudience_color_alpha, quad 0,0-1280,720): new rule left, plant (--jitter-no-fullview) moved')
    # shift
    def render(name, tag, extra, scale=1, nframes=None):
        png = os.path.join(out, '%s_%s_x%g.png' % (name, tag, scale))
        cmd = [rplay, '--capture', caps[name], '--frames', str(nframes or frames[name]), '--scale', str(scale), '--out', png, '--root', root] + extra
        rc, text = run(cmd)
        if rc:
            raise RuntimeError('rplay %s %s failed (%d): %s' % (name, tag, rc, text[-400:]))
        return png
    base = gray(render('bryan', 'base', [], 1, 51))
    new = gray(render('bryan', 'new', ['--jitter', '3'], 1, 51))
    leg = gray(render('bryan', 'legacy', ['--jitter', '3', '--jitter-legacy'], 1, 51))
    sw_n, sw_l = shift_of(base, new, WRESTLER), shift_of(base, leg, WRESTLER)
    bg_n = {k: shift_of(base, new, b) for k, b in BACKGROUNDS.items()}
    bg_l = {k: shift_of(base, leg, b) for k, b in BACKGROUNDS.items()}
    for k in BACKGROUNDS:
        dn = float(np.linalg.norm(sw_n - bg_n[k]))
        dl = float(np.linalg.norm(sw_l - bg_l[k]))
        line(dn < 0.05 and dl > 0.15, 'shift bryan f50 phase 3, wrestler vs %s: new window wrestler (%.3f, %.3f) background (%.3f, %.3f) differ %.3f px (< 0.05); legacy wrestler (%.3f, %.3f) background (%.3f, %.3f) differ %.3f px (the plant, > 0.15)' %
             (k, sw_n[0], sw_n[1], bg_n[k][0], bg_n[k][1], dn, sw_l[0], sw_l[1], bg_l[k][0], bg_l[k][1], dl))
    # identity
    if a.base_rplay:
        for scale in (1, 2):
            for name in ('bryan', 'brock', 'checkb'):
                n = render(name, 'id_new', [], scale, 3)
                p0 = os.path.join(out, '%s_id_base_x%g.png' % (name, scale))
                rc, text = run([a.base_rplay, '--capture', caps[name], '--frames', '3', '--scale', str(scale), '--out', p0, '--root', root])
                if rc:
                    raise RuntimeError('base rplay failed: ' + text[-300:])
                line(open(n, 'rb').read() == open(p0, 'rb').read(), 'identity %s x%g: no --jitter equals the baseline rplay byte for byte' % (name, scale))
    for scale in (1, 2):
        for name in ('bryan', 'checkb'):
            for label, wide in (('16:9', []), ('21:9', ['--wide', '1.3125'])):
                z = render(name, 'id_zero', ['--jitter', '3', '--jitter-zero'] + wide, scale, 3)
                b = render(name, 'id_nojit', wide, scale, 3)
                line(open(z, 'rb').read() == open(b, 'rb').read(), 'identity %s %s x%g: a jitter of 0 through the whole path equals no jitter byte for byte' % (name, label, scale))
    print('jitter_gate: %s' % ('PASS' if not fails[0] else 'FAIL (%d)' % fails[0]))
    return 1 if fails[0] else 0


if __name__ == '__main__':
    sys.exit(main())
