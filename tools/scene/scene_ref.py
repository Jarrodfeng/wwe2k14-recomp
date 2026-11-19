#!/usr/bin/env python3
r"""Session 103 lane LSR: the game's SCENE target before post and 2D overlays, the reference of the stage look (FL_floor_brightness.md finding [C]).

  from scene_ref import scene_picture      (sys.path.insert(0, tools\scene))
  ok, why = scene_picture(fview, capture, frame, ops_json, out_png, root, game)

The presented frame has the game's 2D movie overlay (op 1124, ySprite_yuv_alpha) over the floor (about 0.55x); a lighting bar must not be measured against it. The scene
target is found from fview --ops: the main scene pass is the colour target of the `targets` op before the first T_FillColorDepth draw (the post chain's first draw); its
picture is read through the op before the post chain's `targets` op (brock_losswin: frame 20 surface 1194 through op 1078, 110: 918, 200: 1227).
  py -I tools\scene\scene_ref.py CAPTURE FRAME OUT.png [--ops OPS.json]
"""
import glob, json, os, shutil, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CASE_LOCK = threading.Lock()   # S103 PS1: fview --save-case callers find their own case folder by a before / after listing of out\cases; threads of one process take turns (frame_rig.prefetch)


def pick_ops(ops):
    """(surface, last op) of the scene pass from an fview --ops list, or (None, why)"""
    post = next((x['op'] for x in ops if x['kind'] == 'draw' and x.get('technique') == 'T_FillColorDepth'), None)
    if post is None:
        return None, 'no T_FillColorDepth draw (no post chain) in the op list'
    tg = [x for x in ops if x['kind'] == 'targets' and x['op'] < post]
    if len(tg) < 2:
        return None, 'fewer than two targets ops before the post chain (op %d)' % post
    surf = tg[-2]['colors'][0]
    return (surf, tg[-1]['op'] - 1), 'surface %d through op %d (post chain first draw op %d)' % (surf, tg[-1]['op'] - 1, post)


def scene_picture(fview, capture, frame, ops_json, out_png, root=ROOT, game=None):
    if os.path.exists(out_png):
        return True, 'cached'
    if not os.path.exists(ops_json):
        subprocess.run([fview, '--capture', capture, '--frame', str(frame), '--ops', '--json', ops_json, '--root', root] + (['--game', game] if game else []), capture_output=True)
    if not os.path.exists(ops_json):
        return False, 'fview --ops wrote no op list'
    sel, why = pick_ops(json.load(open(ops_json))['ops'])
    if sel is None:
        return False, why
    with CASE_LOCK:
        before = set(d for d in glob.glob(os.path.join(root, 'out', 'cases', 'fview_*')) if not os.path.basename(d).startswith('fview_unpack_'))
        r = subprocess.run([fview, '--capture', capture, '--frame', str(frame), '--surface', str(sel[0]), '--op', str(sel[1]), '--save-case', '--root', root] + (['--game', game] if game else []), capture_output=True, text=True, errors='replace')
        new = sorted(set(d for d in glob.glob(os.path.join(root, 'out', 'cases', 'fview_*')) if not os.path.basename(d).startswith('fview_unpack_')) - before)
    pic = os.path.join(new[-1], 'picture.png') if new else None
    if not pic or not os.path.exists(pic):
        return False, 'fview wrote no scene picture: ' + (r.stdout or '')[-200:]
    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    shutil.copy(pic, out_png)
    for d in new:  # the case folders are scratch of this call only
        shutil.rmtree(d, ignore_errors=True)
    return True, why


if __name__ == '__main__':
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
    from fview_exe import find_fview
    cap, fr, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    ops = sys.argv[sys.argv.index('--ops') + 1] if '--ops' in sys.argv else out + '.ops.json'
    ok, why = scene_picture(find_fview(root=ROOT), os.path.abspath(cap), fr, ops, out, ROOT, None)
    print(('OK  ' if ok else 'FAIL  ') + why)
    sys.exit(0 if ok else 1)
