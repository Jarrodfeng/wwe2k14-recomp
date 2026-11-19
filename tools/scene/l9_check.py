#!/usr/bin/env python3
r"""S103 L9 check: the wrestlers' shadow on the ring mat (the PLANE shadow). 0 game runs, 0 Surface runs: fview headless + frame_rig.exe on a case's capture.

  py -I tools\scene\l9_check.py [--plant plane_no_reader|plane_no_chain] [--case ZIP] [--frame 500] [--probe]

What it proves (run 27, frame 500: match, arena 4, two wrestlers on the mat; the case's capture is unpacked under out\l9\run27 once):
  game  = fview --save-case of the stream's own plane chain: the mask pass's first draw (the plane constants EyePos / PlanePos / fade / fit / camera, TOLD to the rig as a labelled fixture: the live record's
          PlaneEyes carried the wrong stamp, see LIVE_MAPS.md Lane L9) and the surface the blur's second pass left (the 256 map the mat samples).
  ours  = frame_rig.exe --plane-told: the record drawn alone (the wrestlers unscaled: frame_rig --body-scale-one, a rig fixture, [U]); our plane map (mask 512 -> 256 box -> blur x -> blur z) is read by
          the ring mat's yProp_PSM_screen_shadow variant.
  Bars: (A) the map is made (`plane made N`) and the mat is bound to a *_screen_shadow fragment; (B) the rig's own checks pass (validation 0); (C) our 256 map vs the game's over the wrestlers' footprint:
        IoU of the pixels over 8/255 >= 0.55, the mean level within 20 %; (D) the picture: the mat region darkens under the wrestlers against the same frame with the reader off
        (>= 500 pixels, only darker).
  Plants (each must FAIL with the token PLANE READER): plane_no_reader (WWE_RDR_NO_PLANE_READER=1: the old behaviour, no map, the plain mat) and plane_no_chain (WWE_RDR_PLANE_NO_CHAIN=1: the raw mask is
  published, which no reader accepts).
"""
import argparse, json, os, re, subprocess, sys, zipfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or (os.path.normpath(os.path.join(ROOT, '..', '..', '..')) if os.sep + '.claude' + os.sep in ROOT else ROOT)
FVIEW = os.path.join(MAIN, 'out', 'renderer', 'dev', 'fview.exe')
ENV = dict(os.environ, CLAUDECODE='1', WWE_FVIEW_ALLOW_STALE='1', RDR_MAIN_CHECKOUT=MAIN)
IOU_BAR, LEVEL_BAR, DARK_PIXELS = 0.55, 0.20, 500
FEET_PX, FEET_FRACTION = 45, 0.85
LOW_DY = 400.0   # the low-camera twin: the ground moves this far up in the camera's view
CLEAR = (31, 36, 46)   # the front-end's clear colour


def run(cmd, env=None, timeout=1500):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env or ENV, errors='replace', cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def ops_of(capture, frame, out):
    js = os.path.join(out, 'ops%d.json' % frame)
    if not os.path.exists(js):
        rc, text = run([FVIEW, '--capture', capture, '--frame', str(frame), '--ops', '--json', js, '--root', ROOT])
        if rc != 0 or not os.path.exists(js):
            raise SystemExit('FAIL  fview --ops failed: ' + text[-400:])
    return json.load(open(js))['ops']


def passes_of(ops):
    passes, cur = [], None
    for o in ops:
        if o['kind'] == 'targets':
            cur = {'op': o['op'], 'surface': o['colors'][0] if o['colors'] else 0, 'tech': {}, 'draws': [], 'last': o['op']}
            passes.append(cur)
        elif cur is not None:
            cur['last'] = o['op']
            if o['kind'] == 'draw':
                cur['tech'][o.get('technique', '')] = cur['tech'].get(o.get('technique', ''), 0) + 1
                cur['draws'].append(o)
    return passes


def case_of(capture, frame, surface, through, op):
    rc, text = run([FVIEW, '--capture', capture, '--frame', str(frame), '--surface', str(surface), '--through-op', str(through), '--save-case', '--op', str(op), '--root', ROOT])
    line = [l for l in text.splitlines() if 'case saved:' in l]
    if not line:
        raise SystemExit('FAIL  fview --save-case failed: ' + text[-300:])
    return line[-1].split('case saved:')[1].strip()


def consts(case_dir):
    d = json.load(open(os.path.join(case_dir, 'case.json')))['draw']
    return {c['field']: c['values'] for c in d.get('constants', [])}


def f4(v):
    return ' '.join(repr(float(x)) for x in v)


def stream_plane(capture, frame, out):
    """The game's plane chain: -> dict(told=path, blur=np HxW float (red, 0..1), c34, blur_consts)."""
    ps = passes_of(ops_of(capture, frame, out))
    mask = [p for p in ps if 'T_PlaneShadowMap' in p['tech']]
    blur = [p for p in ps if 'T_PlaneShadowBlur' in p['tech']]
    if not mask or not blur:
        raise SystemExit('FAIL  frame %d: no plane chain in the stream' % frame)
    first = next(d for d in mask[0]['draws'] if d.get('technique') == 'T_PlaneShadowMap')
    c = consts(case_of(capture, frame, mask[0]['surface'], first['op'], first['op']))
    fade = [c['ShadowDistance'][0], c['ShadowDistance'][1], c['ShadowDistance'][2], c['ShadowDistance'][3], c['ShadowRevision'][3], c['ShadowMiddleRev'][3], c['ShadowLeastRev'][3], c['ShadowDeep'][3],
            c['ShadowMiddleDeep'][3], c['ShadowLeastDeep'][3]]
    told = os.path.join(out, 'told_plane_f%d.txt' % frame)
    with open(told, 'w') as f:
        f.write('plane_eye ' + f4(c['EyePos']) + '\n' + 'plane_pos ' + f4(c['PlanePos']) + '\n' + 'plane_fade ' + f4(fade) + '\n')
        f.write('plane_fit ' + f4(c['PlaneInv']) + '\n' + 'plane_camera ' + f4(c['CameraViewProj']) + '\n')
    last_blur = [d for d in blur[-1]['draws'] if d.get('technique') == 'T_PlaneShadowBlur'][-1]   # the z pass: its surface after the pass is the 256 map the mat reads
    bdir = case_of(capture, frame, blur[-1]['surface'], blur[-1]['last'], blur[-1]['last'])
    bimg = np.asarray(Image.open(os.path.join(bdir, 'picture.png')).convert('RGB')).astype(np.float32) / 255.0
    bc = consts(case_of(capture, frame, blur[-1]['surface'], last_blur['op'], last_blur['op']))
    return dict(told=told, blur=bimg[..., 0], blur_consts=bc, c34=float(bc['ShadowThreshold'][0]) if 'ShadowThreshold' in bc else None)


def unpack(case_zip, dest):
    if os.path.isdir(os.path.join(dest, 'capture', 'scene')):
        return os.path.join(dest, 'capture')
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(case_zip) as z:
        for n in z.namelist():
            if n.startswith('capture/'):
                z.extract(n, dest)
    return os.path.join(dest, 'capture')


def rig(capture, frame, out, told, env_extra=None):
    """frame_rig.py once; -> (rc, text, report text, our whole png, our plane map png)."""
    env = dict(ENV, **(env_extra or {}))
    os.makedirs(out, exist_ok=True)
    rc, text = run([sys.executable, '-I', os.path.join('tools', 'scene', 'frame_rig.py'), '--capture', capture, '--frames', str(frame), '--out', out, '--plane-told', told, '--body-scale-one', '--link', 'none'], env=env)
    rep = os.path.join(out, 'frame_%d_report.txt' % frame)
    text_rep = open(rep, errors='replace').read() if os.path.exists(rep) else ''
    w = os.path.join(out, 'work_capture')
    return rc, text, text_rep, os.path.join(w, 'ours_%d_whole.png' % frame), os.path.join(w, 'ours_%d_map_plane.png' % frame)


def dilate(mask, n):
    m = mask.copy()
    for _ in range(n):
        p = np.pad(m, 1)
        m = p[:-2, 1:-1] | p[2:, 1:-1] | p[1:-1, :-2] | p[1:-1, 2:] | m
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', default=None)
    ap.add_argument('--frame', type=int, default=500)
    ap.add_argument('--out', default=None)
    ap.add_argument('--probe', action='store_true')
    ap.add_argument('--low-camera', action='store_true', help='the twin of the blur quad: the plane camera lowered (the ground shifted up by LOW_DY): the map must still be made and read (a quad fitted to the mask corners loses a top corner behind the camera)')
    ap.add_argument('--plant', default=None, choices=[None, 'plane_no_reader', 'plane_no_chain', 'plane_mirror'])
    a = ap.parse_args()
    case_zip = a.case or os.path.join(MAIN, 'out', 's103', 'run27_case.zip')
    base = a.out or os.path.join(ROOT, 'out', 'l9')
    if not os.path.exists(case_zip):
        print('FAIL  PLANE READER: the case %s is not on disk' % case_zip)
        return 1
    cap = unpack(case_zip, os.path.join(base, 'run27'))
    sp = stream_plane(cap, a.frame, os.path.join(base, 'p27'))
    if a.probe:
        print('c34', sp['c34'], 'blur consts', sp['blur_consts'])
        return 0
    if a.low_camera:   # world y -> y + LOW_DY in the plane camera: cam' = cam . T(0, dy, 0)
        lines = open(sp['told']).read().splitlines()
        out_l = []
        for ln in lines:
            if ln.startswith('plane_camera '):
                m = np.array([float(v) for v in ln.split()[1:]]).reshape(4, 4)
                m[:, 3] += m[:, 1] * LOW_DY
                ln = 'plane_camera ' + f4(m.reshape(-1))
            out_l.append(ln)
        sp['told'] = os.path.join(base, 'p27', 'told_plane_low.txt')
        open(sp['told'], 'w').write('\n'.join(out_l) + '\n')
    env_extra = {'plane_no_reader': {'WWE_RDR_NO_PLANE_READER': '1'}, 'plane_no_chain': {'WWE_RDR_PLANE_NO_CHAIN': '1'}, 'plane_mirror': {'WWE_RDR_PLANT_PLANE_NOFLIP': '1'}}.get(a.plant, {})
    rc, text, rep, whole, mapp = rig(cap, a.frame, os.path.join(base, 'plant_' + a.plant if a.plant else 'check'), sp['told'], env_extra)
    fails = []
    if rc != 0 or not rep:
        fails.append('the rig failed (exit %d): %s' % (rc, text[-200:].replace('\n', ' ')))
    made = re.search(r'plane made (\d+) \(draws (\d+); ([^)]*)\)', rep)
    n_made = int(made.group(1)) if made else 0
    if n_made == 0:
        fails.append('the plane map is not made (%s)' % (made.group(3) if made else 'no census line'))
    psm = [l for l in rep.splitlines() if re.match(r'\s*yProp_PSM\s+bound to our effect', l)]
    if not psm or 'screen_shadow' not in psm[0]:
        fails.append('the ring mat is not bound to the screen-shadow variant: ' + (psm[0].strip()[:120] if psm else 'no yProp_PSM coverage line'))
    stats = {}
    if not fails and not a.low_camera and os.path.exists(mapp):
        g = sp['blur']
        o = np.asarray(Image.open(mapp).convert('RGBA')).astype(np.float32)[..., 0] / 255.0
        core = dilate(o > 0.2, 10)   # the wrestlers' footprint (the ropes' proxies are in no record: the game's map has them, ours does not; they are outside this region except where they cross)
        fg, fo = (g > 8 / 255) & core, (o > 8 / 255) & core
        iou = float((fg & fo).sum() / max(1, (fg | fo).sum()))
        un = core & ((g > 8 / 255) | (o > 8 / 255))
        gm, om = float(g[un].mean()) if un.any() else 0.0, float(o[un].mean()) if un.any() else 0.0
        stats.update(iou=iou, game_mean=gm * 255, our_mean=om * 255, mean_d=float(np.abs(g - o)[un].mean() * 255) if un.any() else 255.0)
        if iou < IOU_BAR:
            fails.append('our map vs the game\'s over the wrestlers: IoU %.3f < %.2f' % (iou, IOU_BAR))
        if abs(om - gm) > LEVEL_BAR * max(gm, 1e-6):
            fails.append('our map level %.1f vs the game\'s %.1f (more than %d %% apart)' % (om * 255, gm * 255, int(LEVEL_BAR * 100)))
    if not fails and a.low_camera:
        print('PASS  PLANE READER (low camera): the map is still made (%s) and the mat reads it' % (made.group(0) if made else ''))
        return 0
    if not fails and a.plant in (None, 'plane_mirror'):   # the picture: the same frame with the reader off (the plant's arm)
        rc2, t2, rep2, whole2, _ = rig(cap, a.frame, os.path.join(base, 'plant_off'), sp['told'], {'WWE_RDR_NO_PLANE_READER': '1'})
        if os.path.exists(whole) and os.path.exists(whole2):
            x = np.asarray(Image.open(whole).convert('RGB')).astype(int)
            y = np.asarray(Image.open(whole2).convert('RGB')).astype(int)
            d = np.abs(x - y).max(2)
            changed = int((d > 2).sum())
            darker = float((y.sum(2) - x.sum(2))[d > 2].mean() / 3.0) if changed else 0.0
            stats.update(mat_pixels_darkened=changed, mat_mean_darkening=darker)
            if changed < DARK_PIXELS or darker <= 0:
                fails.append('the mat does not darken under the wrestlers (%d pixels, mean %.1f)' % (changed, darker))
            # WHERE it darkens: the darkened pixels must lie at the wrestlers on screen (their silhouette, the wrestlers-only panel of the same rig run, grown by FEET_PX): a mirrored or offset lookup moves them away
            wp = os.path.join(os.path.dirname(whole), 'ours_%d_wrestlers.png' % a.frame)
            if os.path.exists(wp) and changed:
                wsil = np.abs(np.asarray(Image.open(wp).convert('RGB')).astype(int) - np.array(CLEAR)).max(2) > 6
                near = dilate(wsil, FEET_PX)
                inside = float((near & (d > 2)).sum() / max(1, changed))
                stats['darkened_inside_feet_zone'] = inside
                if not wsil.any() or inside < FEET_FRACTION:
                    fails.append('the darkening is not at the wrestlers: %.2f of it inside their silhouette grown by %d px (bar %.2f)' % (inside, FEET_PX, FEET_FRACTION))
            else:
                fails.append('no wrestlers-only panel to place the darkening against')
        else:
            fails.append('no picture from one of the arms')
    sline = ', '.join('%s %.3g' % (k, v) for k, v in stats.items())
    if fails:
        print('FAIL  PLANE READER: ' + '; '.join(fails))
        return 0 if a.plant else 1
    print('PASS  PLANE READER: plane map made (%s), the mat bound to the screen-shadow variant, %s' % (made.group(0) if made else '', sline))
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())
