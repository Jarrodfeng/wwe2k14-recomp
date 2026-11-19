r"""Lane B (Session 103): the wrestlers applier's own offline check, 0 game runs.

  py -I tools\scene\rf_b_check.py [--capture DIR] [--frames 20 110 200] [--out DIR] [--link actor:slot:0=343:0] [--plants] [--exe rf_b_draw.exe]

For each frame of a capture that carries scene records (default out\captures\brock_losswin of the MAIN checkout, read only):
  1. the GAME's picture of the frame (the capture replayed by fview, headless, --image-png of the presented surface 1) and the same frame with every
     character draw dropped (--draw-drop @effect=yCh_): the difference is the wrestler's silhouette in the game's own picture;
  2. the camera: the live records of these captures carry none, so the game's ViewProj + EyePos of a character draw of the frame are read through
     fview --save-case (TEST ONLY: in the real chain lane A's inputs carry it) and the main wrestler's missing model link is patched in memory
     (--link, TEST ONLY, LISTED in the report: the live contributor did not name that skeleton's model in these captures);
  3. OURS: rf_b_draw (the wrestlers applier alone on a green ground = our silhouette; and with the record's arena as ground = the whole-ish picture);
  4. the numbers: silhouette IoU, centre offset (px), height ratio, luminance structure correlation inside both silhouettes, magenta pixels, validation
     errors; a sheet per frame (game / ours / difference heat, then ours alone / zoomed game|ours / silhouette overlay), labelled, and the report.
With --plants the three planted faults run on the first frame and each must FAIL its check (palette dropped: IoU; light set id ignored: the patched
dark set must darken the wrestler; mesh mask ignored: the patched mask must hide pieces). Exit 0 = every check passed (and every plant failed).
The output curves: the game's picture is fview's (it carries the display curve of the present path); ours is the front-end's colour with NO display
curve: the structure correlation is the curve-independent number, the luminance ratio is printed, never judged.
"""
import argparse, glob, json, os, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'fview'))
from fview_exe import find_fview  # noqa: E402

MAIN = Path(os.environ.get('RDR_MAIN_CHECKOUT', r'C:\Users\Jarrod\wwe2k14-recomp'))
ENV = dict(os.environ, CLAUDECODE='1', WWE_RDR_VALIDATION='1')


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=ENV, timeout=kw.get('timeout', 600))


def game_picture(fv, cap, frame, out, drop=None):
    cmd = [fv, '--capture', cap, '--frame', frame, '--image-png', f'1,{out}']
    if drop:
        cmd += ['--draw-drop', drop]
    r = run(cmd)
    if not os.path.exists(out):
        raise SystemExit(f'FAIL  fview wrote no picture: {r.stdout[-300:]} {r.stderr[-300:]}')


def grab_camera(fv, cap, frame, out_txt, workdir):
    oj = os.path.join(workdir, f'ops{frame}.json')
    run([fv, '--capture', cap, '--frame', frame, '--ops', '--json', oj])
    ops = json.load(open(oj))['ops']
    op = next((o['op'] for o in ops if o['kind'] == 'draw' and str(o.get('effect', '')).startswith('yCh_') and o.get('outcome') == 'drawn'), None)
    if op is None:
        raise SystemExit(f'FAIL  frame {frame} of {cap} has no drawn character draw (yCh_*): no camera can be read from the stream')
    before = set(glob.glob(str(HERE / 'out' / 'cases' / 'fview_*')))
    run([fv, '--capture', cap, '--frame', frame, '--save-case', '--op', op])
    fresh = sorted(set(glob.glob(str(HERE / 'out' / 'cases' / 'fview_*'))) - before, key=os.path.getmtime)
    if not fresh:
        raise SystemExit(f'FAIL  fview --save-case wrote no case for frame {frame} op {op}')
    new = fresh[-1]
    d = json.load(open(os.path.join(new, 'case.json')))['draw']
    vp = next(c['values'] for c in d['constants'] if c['stage'] == 'vs' and c['field'] == 'ViewProj')
    eye = next(c['values'] for c in d['constants'] if c['stage'] == 'vs' and c['field'] == 'EyePos')
    open(out_txt, 'w').write(' '.join(repr(x) for x in vp + eye[:3]) + '\n')


BODY_SCALE = ''   # S103 RGF: the TOLD body scale (ID=S[,ID=S]) every rf_b_draw run is given; set in main from --body-scale / frame_rig.py's DEFAULT_SCALES


def told_body_scale(cap):
    """The body scale frame_rig.py tells for this capture (its DEFAULT_SCALES: the same source), read as text so frame_rig's imports are not needed (this runs under -I)."""
    import ast, re
    src = (HERE / 'tools' / 'scene' / 'frame_rig.py').read_text(encoding='utf-8')
    m = re.search(r'^DEFAULT_SCALES\s*=\s*(\{.*\})', src, re.M)
    scales = ast.literal_eval(m.group(1)) if m else {}
    return ','.join(f'{k}={v:.7f}' for k, v in scales.get(os.path.basename(os.path.normpath(cap)), {}).items())


def ours(exe, cap, frame, game_dir, out_png, cam, link, extra=()):
    cmd = [exe, cap, '--frame', frame, '--game', game_dir, '--out', out_png, '--cam', cam, '--link', link] + (['--body-scale', BODY_SCALE] if BODY_SCALE else []) + list(extra)
    r = run(cmd)
    log = r.stderr
    if r.returncode != 0 or not os.path.exists(out_png):
        raise SystemExit(f'FAIL  rf_b_draw: {log[-600:]}')
    val = [l for l in log.splitlines() if 'validation errors' in l]
    stats = [l for l in log.splitlines() if l.startswith('[rf][wrestlers]')]
    return log, int(val[-1].split('errors:')[1]) if val else -1, stats[-1] if stats else ''


def rgb(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int32)


def mask_from_green(a):
    g = (a[:, :, 0] < 40) & (a[:, :, 1] > 200) & (a[:, :, 2] < 40)
    m = Image.fromarray((~g * 255).astype('uint8')).filter(ImageFilter.MedianFilter(3))
    return np.asarray(m) > 127


def mask_from_diff(a, b):
    m = Image.fromarray(((np.abs(a - b).sum(2) > 30) * 255).astype('uint8')).filter(ImageFilter.MedianFilter(3))
    return np.asarray(m) > 127


def lum(a):
    return a[:, :, 0] * 0.299 + a[:, :, 1] * 0.587 + a[:, :, 2] * 0.114


def metrics(gm, om, game, ours_whole, ours_alone):
    inter, union = (gm & om).sum(), (gm | om).sum()
    iou = inter / union if union else 0.0

    def bb(m):
        ys, xs = np.nonzero(m)
        return (xs.min(), xs.max(), ys.min(), ys.max()) if len(xs) else (0, 0, 0, 0)
    gb, ob = bb(gm), bb(om)
    off = float(np.hypot((gb[0] + gb[1]) / 2 - (ob[0] + ob[1]) / 2, (gb[2] + gb[3]) / 2 - (ob[2] + ob[3]) / 2))
    hr = (ob[3] - ob[2] + 1) / max(1, gb[3] - gb[2] + 1)
    both = Image.fromarray(((gm & om) * 255).astype("uint8")).filter(ImageFilter.MinFilter(5))
    core = np.asarray(both) > 127
    corr = 0.0
    ratio = 0.0
    if core.sum() > 50:
        a, b = lum(game)[core], lum(ours_whole)[core]
        corr = float(np.corrcoef(a, b)[0, 1]) if a.std() > 1e-6 and b.std() > 1e-6 else 0.0
        ratio = float(b.mean() / max(1e-6, a.mean()))
    magenta = int(((ours_alone[:, :, 0] > 200) & (ours_alone[:, :, 1] < 40) & (ours_alone[:, :, 2] > 200)).sum())
    return dict(iou=float(iou), centre_off_px=off, height_ratio=float(hr), corr=corr, lum_ratio=ratio, magenta=magenta, game_px=int(gm.sum()), ours_px=int(om.sum()))


def label(img, text):
    im = img.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 16], fill=(0, 0, 0))
    d.text((3, 2), text, fill=(255, 255, 255))
    return im


def sheet(path, frame, cap_name, game, whole, alone, gm, om, m, stats_line):
    G, W, A = [Image.fromarray(x.astype('uint8')) for x in (game, whole, alone)]
    heat = np.clip(np.abs(game - whole).sum(2) / 3, 0, 255).astype('uint8')
    H = Image.fromarray(heat).convert('RGB')
    ys, xs = np.nonzero(gm | om)
    x0, x1, y0, y1 = max(0, xs.min() - 30), min(game.shape[1], xs.max() + 30), max(0, ys.min() - 30), min(game.shape[0], ys.max() + 30)
    zoom = lambda im: im.crop((x0, y0, x1, y1)).resize((int((x1 - x0) * 2), int((y1 - y0) * 2)))
    ov = np.zeros(game.shape, 'uint8')
    ov[gm & ~om] = (255, 60, 60)
    ov[om & ~gm] = (60, 220, 255)
    ov[gm & om] = (255, 255, 255)
    O = Image.fromarray(ov)
    tiles = [label(G, f'GAME (fview, display curve on) {cap_name} f{frame}'), label(W, 'OURS whole: wrestlers applier + arena ground (no display curve)'), label(H, 'difference |game - ours| (heat)'),
            label(A, f'OURS alone: wrestler objects drawn {stats_line}'), label(zoom(G), 'zoom: game'), label(zoom(W), 'zoom: ours'),
            label(O, f'silhouette: white both, red game only, cyan ours only: IoU {m["iou"]:.3f} off {m["centre_off_px"]:.1f}px h-ratio {m["height_ratio"]:.3f}')]
    tw, th = 640, 360
    canvas = Image.new('RGB', (tw * 3, th * 3), (20, 20, 20))
    for i, t in enumerate(tiles[:3] + [tiles[3]] + [tiles[6]]):
        canvas.paste(t.resize((tw, th)), ((i % 3) * tw, (i // 3) * th))
    z1, z2 = tiles[4], tiles[5]
    zh = th
    for i, z in enumerate((z1, z2)):
        s = min(tw / z.width, zh / z.height)
        canvas.paste(z.resize((max(1, int(z.width * s)), max(1, int(z.height * s)))), (i * tw, th * 2))
    canvas.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=str(MAIN / 'out' / 'captures' / 'brock_losswin'))
    ap.add_argument('--frames', nargs='+', default=['20', '110', '200'])
    ap.add_argument('--out', default=None)
    ap.add_argument('--link', default='actor:slot:0=343:0')
    ap.add_argument('--body-scale', default=None, help='S103 RGF: the TOLD body scale ID=S[,ID=S] given to rf_b_draw (W2: an unknown scale is a hole); default = frame_rig.py DEFAULT_SCALES for the capture')
    ap.add_argument('--plants', action='store_true')
    ap.add_argument('--exe', default=str(HERE / 'out' / 'tools' / 'rf_b_draw.exe'))
    ap.add_argument('--keys-exe', default=str(HERE / 'out' / 'tools' / 'rf_b_keys.exe'))
    ap.add_argument('--game', default=str(MAIN / 'game_data' / 'extracted'))
    ap.add_argument('--min-iou', type=float, default=0.80)
    ap.add_argument('--min-corr', type=float, default=0.50)
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    a.out = str(run_dirs.run_out('B', a.out))
    os.makedirs(a.out, exist_ok=True)
    fv = find_fview(root=str(HERE), kind='dev')
    cap_name = os.path.basename(a.capture.rstrip('\\/'))
    ok = True
    global BODY_SCALE
    BODY_SCALE = a.body_scale if a.body_scale is not None else told_body_scale(a.capture)
    print(f'TOLD (test rig, not measured): link {a.link}; body scale {BODY_SCALE or "none (a Live wrestler with no scale is a hole: W2)"} [U that it is this capture\'s body]')
    # B4 / B7: the record's model naming, file-only: every model key of every ch pac loads ITS PACH; the planted fault (variant used as the n-th PACH) must fail
    real = run([a.keys_exe, '--game', a.game])
    fault = run([a.keys_exe, '--game', a.game, '--fault'])
    kok = real.returncode == 0 and fault.returncode == 1
    print(('PASS' if kok else 'FAIL'), 'model keys resolve by the full key:', real.stdout.strip(), '|| planted fault (variant as ordinal):', fault.stdout.strip(), f'(real exit {real.returncode} must be 0, fault exit {fault.returncode} must be 1)')
    ok &= kok
    rows = []
    first = None
    for fr in a.frames:
        P = lambda n: os.path.join(a.out, f'{n}_{fr}.png')
        cam = os.path.join(a.out, f'cam_{cap_name}_{fr}.txt')
        game_picture(fv, a.capture, fr, P('game'))
        game_picture(fv, a.capture, fr, P('nowr'), drop='@effect=yCh_')
        if not os.path.exists(cam):
            grab_camera(fv, a.capture, fr, cam, a.out)
        log, val, stats = ours(a.exe, a.capture, fr, a.game, P('ours'), cam, a.link, ['--arena'])
        log2, val2, _ = ours(a.exe, a.capture, fr, a.game, P('alone'), cam, a.link, ['--green'])
        game, nowr, whole, alone = rgb(P('game')), rgb(P('nowr')), rgb(P('ours')), rgb(P('alone'))
        gm, om = mask_from_diff(game, nowr), mask_from_green(alone)
        m = metrics(gm, om, game, whole, alone)
        m['validation'] = max(val, val2)
        good = m['iou'] >= a.min_iou and m['corr'] >= a.min_corr and m['magenta'] == 0 and m['validation'] == 0 and m['height_ratio'] > 0.93 and m['height_ratio'] < 1.07
        ok &= good
        sheet(os.path.join(a.out, f'sheet_{fr}.png'), fr, cap_name, game, whole, alone, gm, om, m, f'(see stats)')
        print(('PASS' if good else 'FAIL'), f'frame {fr}: IoU {m["iou"]:.3f} (>= {a.min_iou}), centre off {m["centre_off_px"]:.1f}px, height ratio {m["height_ratio"]:.3f}, structure corr {m["corr"]:.3f} (>= {a.min_corr}), '
              f'lum ratio ours/game {m["lum_ratio"]:.2f} (not judged: no display curve, stand-in light set), magenta {m["magenta"]}, validation {m["validation"]}, silhouette px game {m["game_px"]} ours {m["ours_px"]}')
        print('     ' + stats)
        rows.append((fr, m))
        if first is None:
            first = (fr, cam, m)
    if a.plants and first:
        fr, cam, base = first
        P = lambda n: os.path.join(a.out, f'{n}_{fr}.png')
        game, nowr = rgb(P('game')), rgb(P('nowr'))
        gm = mask_from_diff(game, nowr)
        # 1. palette dropped: the silhouette must stop matching
        _, _, _ = ours(a.exe, a.capture, fr, a.game, P('plant_palette'), cam, a.link, ['--green', '--plant', 'palette'])
        pm = metrics(gm, mask_from_green(rgb(P('plant_palette'))), game, rgb(P('ours')), rgb(P('plant_palette')))
        failed = pm['iou'] < a.min_iou
        print(('PASS' if failed else 'FAIL'), f'planted fault palette dropped (bind pose): IoU {pm["iou"]:.3f} vs the check bar {a.min_iou}: the check {"fails" if failed else "DOES NOT FAIL"} it (must fail)')
        ok &= failed
        # 2. light set id: the record says set 100 (all lights zero): honoured = much darker than the default set; the plant ignores the id
        _ = ours(a.exe, a.capture, fr, a.game, P('lt_def'), cam, a.link, ['--green'])
        _ = ours(a.exe, a.capture, fr, a.game, P('lt_100'), cam, a.link, ['--green', '--light-id', '100'])
        _ = ours(a.exe, a.capture, fr, a.game, P('lt_plant'), cam, a.link, ['--green', '--light-id', '100', '--plant', 'light'])
        L = lambda n: (lum(rgb(P(n)))[mask_from_green(rgb(P('lt_def')))]).mean()
        ld, l100, lp = L('lt_def'), L('lt_100'), L('lt_plant')
        honoured = l100 < 0.8 * ld
        plant_fails = not (lp < 0.8 * ld)
        print(('PASS' if honoured else 'FAIL'), f'record light set id honoured: patched set 100 (dark) mean luminance {l100:.1f} vs default set {ld:.1f} (must be < 0.8x)')
        print(('PASS' if plant_fails else 'FAIL'), f'planted fault light id ignored: luminance {lp:.1f} vs default {ld:.1f}: the check {"fails" if plant_fails else "DOES NOT FAIL"} it (must fail the "honoured" test)')
        ok &= honoured and plant_fails
        # 3. mesh mask: the record hides the first 16 meshes: honoured = the silhouette area shrinks; the plant ignores the mask
        _ = ours(a.exe, a.capture, fr, a.game, P('mk_full'), cam, a.link, ['--green'])
        _ = ours(a.exe, a.capture, fr, a.game, P('mk_hide'), cam, a.link, ['--green', '--mask', 'FFFF0000,FFFFFFFF'])
        _ = ours(a.exe, a.capture, fr, a.game, P('mk_plant'), cam, a.link, ['--green', '--mask', 'FFFF0000,FFFFFFFF', '--plant', 'mask'])
        area = lambda n: int(mask_from_green(rgb(P(n))).sum())
        af, ah, ap_ = area('mk_full'), area('mk_hide'), area('mk_plant')
        honoured = ah < 0.9 * af
        plant_fails = not (ap_ < 0.9 * af)
        print(('PASS' if honoured else 'FAIL'), f'record mesh mask honoured: silhouette {ah} px with the first 16 meshes hidden vs {af} px (must be < 0.9x)')
        print(('PASS' if plant_fails else 'FAIL'), f'planted fault mask ignored: {ap_} px vs {af}: the check {"fails" if plant_fails else "DOES NOT FAIL"} it (must fail the "honoured" test)')
        ok &= honoured and plant_fails
    print('RESULT', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
