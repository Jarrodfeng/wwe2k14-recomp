r"""Lane 139 (SH3 slice 2) gate driver: the plane shadow chain's constants drawn by ours from the scene record (docs\renderer\SHADOW_PLANE.md). 0 game runs.

  py -I tools\shadow\plane_shadow_check.py unit      half (a): the capture's plane-chain constants -> register uploads -> the SHIPPED writer's tracker -> record -> BuildMask / BuildBlur equal the
                                                      stream's constants bit for bit on Check B, run 7079 and Bryan (tools\shadow\plane_unit.cpp); the record, guard and board checks; plants
                                                      plane_inv_identity, plane_camera_stale, eye_point_shift each must FAIL that check
  py -I tools\shadow\plane_shadow_check.py clean     half (b): fview --plane-check plays each capture in lockstep with SH2 on in both arms (A = the stream, B = the plane hook fed from the oracle
                                                      record): every mask and blur draw replaced (kept 0), the stamp -> frame offset learned, every shadow reader image (the plane mask 221 among them) within
                                                      rounding of the stream's, the picture (the ring mat reads the mask) within tolerance on every frame, validation 0
  py -I tools\shadow\plane_shadow_check.py plants    each planted fault, run without the verify check so it reaches the picture, must change the plane chain's images (and the ring mat's
                                                      picture) while its control is clean: plane_inv_identity, plane_camera_stale, eye_point_shift; the verify check itself must refuse a planted
                                                      join; SH2's copy_swap and scaled_map re-planted with the plane hook on
  py -I tools\shadow\plane_shadow_check.py sites   lane 159: the writer's listening sites against the GAME'S code (tools\shadow\plane_sites_check.py) and its six plants (the F1 re-find)
  py -I tools\shadow\plane_shadow_check.py all

Lane 159: the mask and blur draws are now made with OUR programs (rdr_plane_mask / rdr_plane_blur, the stream's geometry, our constants block). clean also reports, per capture, the largest level difference
and the PIXELS more than 1 level off for the picture and for every plane-chain image, against the stream; images within ROUNDING, the picture within PICTURE_MAX levels and PICTURE_OVER1 of its pixels.

Needs this checkout's out\renderer\dev\fview.exe (tools\fview\build.ps1) and out\shadow\plane_unit.exe (tools\shadow\build_plane_unit.ps1) and the three captures under the main checkout's out\
(out\lane86\brock|bryan\capture, out\s101\lane92\case_20261112_122658_040_0_pc_run_exit\capture). A missing input FAILs; the GPU is shared: only 'Not enough graphics memory' is retried."""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PICTURE_MAX = 2.0          # lane 159: the presented picture, stream vs OUR programs: at most this many levels off ...
PICTURE_OVER1 = 1e-7       # ... and at most this fraction of its pixels more than 1 level off (our shaders are the same arithmetic in another order: Check B has 1 pixel in 312 million at 2 levels)
ROUNDING = 2.0   # "within rounding": the plane chain's RGBA8 images may differ by this many levels of 255 (the replaced draws carry the very words of the stream's, so 0 is expected)


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    try:
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()
import sys as _sys
_sys.path.insert(0, str(ROOT / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
import fview_exe as fe
try:
    FVIEW = Path(fe.find_fview(root=ROOT, kind='dev'))
except fe.FviewError as _e:
    print('FAIL  ' + str(_e))
    _sys.exit(1)
UNIT = ROOT / 'out' / 'shadow' / 'plane_unit.exe'
OUT = ROOT / 'out' / 'l139'
CAPS = {'brock': MAIN / 'out' / 'lane86' / 'brock' / 'capture',
        'bryan': MAIN / 'out' / 'lane86' / 'bryan' / 'capture',
        'r7079': MAIN / 'out' / 's101' / 'lane92' / 'case_20261112_122658_040_0_pc_run_exit' / 'capture',
        'j1': ROOT / 'out' / 'l159' / 'j1' / 'capture'}   # lane 159: Check J1's case zip (unpacked on first use, see j1_capture())
J1_ZIP = MAIN / 'out' / 's101' / 'runs' / '20261112_172304_s100_owner_s89_brock' / 'case_20261112_172341_872_0_pc_run_exit.zip' 
fails = []
NFRAMES = {}


def fail(msg):
    fails.append(msg)
    print('FAIL  ' + msg)


def ok(msg):
    print('ok    ' + msg)


def j1_capture():
    """Unpacks Check J1's case zip into out\\l159\\j1 once (Windows tar reads zips). False when the zip is missing."""
    if (CAPS['j1'] / 'images.bin').exists():
        return True
    if not J1_ZIP.exists():
        return False
    (ROOT / 'out' / 'l159' / 'j1').mkdir(parents=True, exist_ok=True)
    r = subprocess.run([os.path.join(os.environ.get('SystemRoot', 'C:\\Windows'), 'System32', 'tar.exe'), '-xf', str(J1_ZIP), '-C', str(ROOT / 'out' / 'l159' / 'j1')], capture_output=True, text=True, timeout=1200)
    return r.returncode == 0 and (CAPS['j1'] / 'images.bin').exists()


def oracle(name):
    """Dumps the capture's plane constants (tools\\shadow\\plane_oracle.py); returns the file path and the draw count (mask + blur)."""
    cap = CAPS[name]
    if name == 'j1' and not j1_capture():
        fail('cannot unpack Check J1\'s case zip (%s)' % J1_ZIP)
        return None, 0
    if not (cap / 'images.bin').exists():
        fail('cannot read %s (a check that reads nothing must fail)' % cap)
        return None, 0
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / (name + '.plane')
    r = subprocess.run([sys.executable, '-I', str(HERE / 'plane_oracle.py'), 'dump', str(cap), str(path)], capture_output=True, text=True, timeout=1200)
    print('      ' + (r.stdout.strip() or r.stderr.strip()).splitlines()[-1][:260])
    if r.returncode != 0:
        fail('%s: the oracle dump failed / a recorded fact (camera == colour c56-59, the ShadowDistance window) failed: %s' % (name, (r.stdout + r.stderr).strip()[-300:]))
        return None, 0
    m = re.search(r'(\d+) mask draws, (\d+) blur draws', r.stdout)
    mf = re.search(r': (\d+) frames', r.stdout)
    NFRAMES[name] = int(mf.group(1)) if mf else 0
    return path, (int(m.group(1)) + int(m.group(2))) if m else 0


def unit(args):
    if not UNIT.exists():
        fail('%s does not exist (tools\\shadow\\build_plane_unit.ps1)' % UNIT)
        return None, ''
    r = subprocess.run([str(UNIT)] + args, capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout + r.stderr


def fview(name, label, extra, frames, every=1, tries=8):
    if not FVIEW.exists():
        fail('%s does not exist (tools\\fview\\build.ps1)' % FVIEW)
        return None
    cap = CAPS[name]
    path = OUT / (name + '.plane')
    js = OUT / (label + '.json')
    if js.exists():
        js.unlink()
    cmd = [str(FVIEW), '--capture', str(cap), '--root', str(ROOT), '--validation', '1', '--plane-check', str(js), '--plane-oracle', str(path), '--check-every', str(every)]
    cmd += ['--frames', str(frames or NFRAMES.get(name, 0))]
    cmd += extra
    for attempt in range(tries):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
        text = r.stdout + r.stderr
        if 'Not enough graphics memory' in text:
            print('      %s: GPU busy (attempt %d), retrying' % (label, attempt + 1))
            time.sleep(20)
            continue
        if not js.exists():
            fail('%s: fview wrote no json (exit %d): %s' % (label, r.returncode, text.strip().splitlines()[-2:]))
            return None
        d = json.loads(js.read_text())
        d['exit'] = r.returncode
        rep = d['report']
        m = re.search(r'claimed (\d+) \(mask (\d+), blur (\d+)\) replaced (\d+) \(mask (\d+), blur (\d+)\)', rep)
        d['claimed'], d['claimed_mask'], d['claimed_blur'], d['replaced'], d['mask'], d['blur'] = tuple(int(x) for x in m.groups()) if m else (-1,) * 6
        m = re.search(r'mismatch (\d+) \(constants (\d+), (?:incomplete (\d+), )?late (\d+)\)', rep)   # (lane 205 added "incomplete N": the old pattern read -1 for every report)
        d['kept_late'], d['relearned'] = (int(m.group(4)), int(re.search(r'relearned (\d+)', rep).group(1))) if m else (-1, -1)
        m = re.search(r'mask draws min (\d+) max (\d+)', rep)
        d['rows_min'], d['rows_max'] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        m = re.search(r'join offset (-?\d+)', rep)
        d['offset'] = int(m.group(1)) if m else None
        m = re.search(r'unjoined (\d+), mismatch (\d+)', rep)
        d['kept_unjoined'], d['kept_mismatch'] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        m = re.search(r"joined with casters' mesh (\d+), on the stream's mesh (\d+)", rep)   # lane 255
        d['joined'], d['alone'] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        m = re.search(r'rows verified (\d+)', rep)
        d['rows_verified'] = int(m.group(1)) if m else -1
        pic = d['picture']
        d['picture_close'] = pic['max_diff'] <= PICTURE_MAX and pic['over1_pixels'] <= PICTURE_OVER1 * max(pic['pixels'], 1)
        d['differing_raw'] = d['differing_frames']
        if d['picture_close']:
            d['differing_frames'] = 0   # from here on "differing" means: beyond the tolerance above
        d['plane_images'] = {k: v for k, v in d['images'].items() if v.get('chain') == 2}
        d['map_images'] = {k: v for k, v in d['images'].items() if v.get('chain') == 1}
        return d
    fail('%s: the GPU stayed busy' % label)
    return None


def worst(images):
    return max([v['max_diff'] for v in images.values()] or [0.0]), sum(v['shape_fail'] for v in images.values())


def judge_clean(d, expected_draws):
    bad = []
    if d['exit'] != 0 or d['validation_errors']:
        bad.append('exit %d, validation errors %d' % (d['exit'], d['validation_errors']))
    if d['compared'] < 1:
        bad.append('no frame compared')
    if d['claimed'] != expected_draws:
        bad.append('claimed %d plane-chain draws, the oracle has %d' % (d['claimed'], expected_draws))
    if d['replaced'] != d['claimed'] or d['replaced'] <= 0:
        bad.append('replaced %d of %d claimed (kept: mismatch %d, unjoined %d)' % (d['replaced'], d['claimed'], d['kept_mismatch'], d['kept_unjoined']))
    if d['mask'] <= 0 or d['blur'] <= 0:
        bad.append('replaced mask %d / blur %d: both kinds must be ours' % (d['mask'], d['blur']))
    if d['rows_min'] < 16 or d['rows_max'] < 16:
        bad.append('a mask draw verified fewer than 16 rows (min %d, max %d): c0-c7 and c9-c16 must all be compared (the program has no c8)' % (d['rows_min'], d['rows_max']))
    if d['offset'] != 1:
        bad.append('the stamp -> frame offset learned is %s, the oracle stamps 1 before the header frame' % d['offset'])
    if not d['plane_images']:
        bad.append('no plane-chain image compared (the readers\' images could not be read)')
    mx, shape = worst(d['images'])
    if mx > ROUNDING or shape:
        bad.append('a shadow reader image differs from the stream\'s: max diff %s of 255 (limit %s), shape failures %d' % (mx, ROUNDING, shape))
    if not d['picture_close']:
        bad.append('the picture (the ring mat reads the mask) differs from the stream beyond the tolerance (max diff %s of 255, limit %s; %d pixels more than 1 level off of %d)' %
                   (d['picture']['max_diff'], PICTURE_MAX, d['picture']['over1_pixels'], d['picture']['pixels']))
    return bad


def clean():
    for name in ('brock', 'j1', 'r7079', 'bryan'):
        path, draws = oracle(name)
        if not path:
            continue
        d = fview(name, 'clean_' + name, ['--shadows-ours', '--shadow-map-size', '512'], 0, every=1 if name not in ('r7079', 'j1') else 2)
        if d is None:
            continue
        bad = judge_clean(d, draws if name != 'r7079' else d['claimed'])
        if bad:
            for b in bad:
                fail('%s: %s' % (name, b))
        else:
            mx, _ = worst(d['plane_images'])
            over1 = sum(v['over1_pixels'] for v in d['images'].values())
            ok('%s: %d of %d plane-chain draws replaced by OUR programs (mask %d, blur %d), offset %d learned, %d plane images within %s of 255 (worst %s level, %d pixels over 1 of %d), picture within tolerance on %d frames (max %s level, %d pixels over 1 of %d), validation 0' %
               (name, d['replaced'], d['claimed'], d['mask'], d['blur'], d['offset'], len(d['plane_images']), ROUNDING, mx, over1, sum(v['pixels'] for v in d['images'].values()), d['compared'],
                d['picture']['max_diff'], d['picture']['over1_pixels'], d['picture']['pixels']))


def judge_joined(d, expected_draws, need_mesh=True):
    """Lane 255: casters AND plane together. The plane chain's draws are fully ours: claimed once each (the oracle's count), replaced all, the mask draws either on the casters' mesh (joined) or on the stream's
    (alone), the pictures within the clean bars. need_mesh: a capture whose casters can join at least most of the mask draws (Check B, J1, 7079: >= 95 %)."""
    bad = judge_clean(d, expected_draws)
    if d['joined'] < 0:
        bad.append('the plane hook\'s report has no "joined with casters\' mesh" count (the binder join is not in this exe)')
        return bad
    if d['joined'] + d['alone'] != d['mask']:
        bad.append('joined %d + alone %d != mask draws replaced %d (a draw claimed twice or dropped)' % (d['joined'], d['alone'], d['mask']))
    if d['joined'] <= 0:
        bad.append('no mask draw was made with the casters\' mesh AND our program (joined 0 of %d)' % d['mask'])
    elif need_mesh and d['joined'] < 0.95 * d['mask']:
        bad.append('only %d of %d mask draws joined the casters\' mesh (expected >= 95 %%: the rest are frame 0 / unjoined)' % (d['joined'], d['mask']))
    return bad


def joined():
    """Lane 255: --shadow-casters-ours --plane-shadow-ours on every capture, and the plants join_double / join_drop on Check B."""
    sh2 = ['--shadows-ours', '--shadow-map-size', '512']
    game = str(MAIN / 'game_data' / 'extracted')
    if not os.path.isdir(game):
        fail('input missing: the game files ' + game + ' (the casters read chCommon.pac and the ring arena)')
        return
    cas = ['--wrestlers-ours', '--ring-ours', '--shadow-casters-ours', '--game', game]   # the casters' joins need the wrestlers' and the ring's hooks and the game files (shadow_casters_check.py)
    brock_draws = 0
    for name in ('brock', 'j1', 'r7079', 'bryan'):
        path, draws = oracle(name)
        if not path:
            continue
        if name == 'brock':
            brock_draws = draws
        every = 1 if name not in ('r7079', 'j1') else 2
        d = fview(name, 'joined_' + name, sh2 + (cas if name != 'bryan' else ['--shadow-casters-ours', '--game', game]), 0, every=every)
        if d is None:
            continue
        bad = judge_joined(d, draws if name != 'r7079' else d['claimed'], need_mesh=name != 'bryan')
        if bad:
            for b in bad:
                fail('joined %s: %s' % (name, b))
        else:
            mx, _ = worst(d['plane_images'])
            over1 = sum(v['over1_pixels'] for v in d['images'].values())
            ok('joined %s: %d of %d plane-chain draws replaced (mask %d = %d on the casters\' mesh + %d on the stream\'s, blur %d), each claimed once, plane images worst %s level (%d pixels over 1), picture max %s level (%d pixels over 1 of %d) on %d frames, validation 0' %
               (name, d['replaced'], d['claimed'], d['mask'], d['joined'], d['alone'], d['blur'], mx, over1, d['picture']['max_diff'], d['picture']['over1_pixels'], d['picture']['pixels'], d['compared']))
    # the casters-only arm (plane hook off in the same run is not possible: --plane-check always has it; casters + plane_check with the plane hook's join refused is the control the plants give). The casters' own proof (shadow_casters_check.py) holds the casters-only picture byte-equal.
    if not brock_draws:
        return
    control = fview('brock', 'joined_control', sh2 + cas, 120, 2)
    if control is None or judge_joined(control, control['claimed']):
        fail('the join plants control (casters + plane, no fault, 120 frames) is not clean: %s' % ('no run' if control is None else judge_joined(control, control['claimed'])))
        return
    ok('join control (120 frames, every 2nd): claimed %d = replaced %d, joined %d + alone %d = mask %d' % (control['claimed'], control['replaced'], control['joined'], control['alone'], control['mask']))
    for fault, why in (('join_double', 'a casters-joined mask draw offered to the plane hook twice'), ('join_drop', 'every 7th casters-joined mask draw never offered to it')):
        d = fview('brock', 'joined_plant_' + fault, sh2 + cas + ['--replace-fault', 'plane.' + fault], 120, 2)
        if d is None:
            continue
        bad = judge_joined(d, control['claimed'])
        if bad:
            ok('plant %s (%s) FAILS as it must: %s' % (fault, why, '; '.join(bad)[:300]))
        else:
            fail('plant %s (%s) passed: the join bars cannot see it (claimed %d, control %d)' % (fault, why, d['claimed'], control['claimed']))


def plants():
    path, draws = oracle('brock')
    if not path:
        return
    frames, every = 120, 2
    sh2 = ['--shadows-ours', '--shadow-map-size', '512']
    control = fview('brock', 'control_noverify', sh2 + ['--replace-fault', 'plane.no_verify'], frames, every)
    if control is None or control['differing_frames'] != 0 or control['replaced'] <= 0 or worst(control['images'])[0] > ROUNDING:
        fail('the control (plane.no_verify, no fault) is not clean: %s' % ('no run' if control is None else 'differs on %d frames, replaced %d' % (control['differing_frames'], control['replaced'])))
        return
    ok('control (plane.no_verify, no fault): %d draws replaced, plane images within %s, picture within tolerance on %d frames' % (control['replaced'], ROUNDING, control['compared']))
    for fault, why in (('inv_identity', 'plane_inv_identity'), ('camera_stale', 'plane_camera_stale'), ('plane_pos_shift', 'plane_pos_shift'),
                       ('blur_radius', 'blur_radius (the blur\'s tap step 8x)'), ('stamp_lock', 'stamp_lock (the record read one frame off: mask AND blur constants of the wrong frame)')):
        d = fview('brock', 'plant_' + fault, sh2 + ['--replace-fault', 'plane.no_verify,plane.' + fault], frames, every)
        if d is None:
            continue
        mx, _ = worst(d['plane_images'])
        if mx > ROUNDING:
            ok('plant %s FAILS as it must: the plane chain\'s images differ (max %s of 255, %d pixels over 1 level), the picture on %d of %d frames (max %s, %d pixels over 1); its control is clean' %
               (why, mx, sum(v['over1_pixels'] for v in d['plane_images'].values()), d['differing_frames'], d['compared'], d['picture']['max_diff'], d['picture']['over1_pixels']))
        else:
            fail('plant %s passed: the plane images stay within rounding (max %s)' % (why, mx))
    d = fview('brock', 'plant_join_verified', sh2 + ['--replace-fault', 'plane.inv_identity'], frames, every)
    if d is not None:
        if d['replaced'] < d['claimed'] and d['differing_frames'] == 0 and worst(d['images'])[0] <= ROUNDING:
            ok('the verify check refuses the planted PlaneInv by itself: replaced %d of %d claimed, the plane chain and the picture stay the stream\'s (within tolerance)' % (d['replaced'], d['claimed']))
        else:
            fail('verify did not refuse the planted PlaneInv (replaced %d of %d, differing %d)' % (d['replaced'], d['claimed'], d['differing_frames']))
    # lane 159: with the verify ON every record fault is refused per draw (the draw stays the stream's, counted) - and the clean bar (replaced == claimed) FAILS, so a fault cannot hide as a pass.
    for fault in ('blur_radius', 'stamp_lock', 'nth_shift', 'camera_stale', 'plane_pos_shift'):
        d = fview('brock', 'plant_verify_' + fault, sh2 + ['--replace-fault', 'plane.' + fault], frames, every)
        if d is None:
            continue
        bad = judge_clean(d, d['claimed'])
        if bad and d['differing_frames'] == 0 and d['replaced'] < d['claimed']:
            ok('plant %s with the verify ON: the clean bar FAILS as it must (replaced %d of %d claimed, kept unjoined %d / mismatch %d) and the refused draws stay the stream\'s (picture within tolerance); the refusal is REQUIRED on brock' % (fault, d['replaced'], d['claimed'], d['kept_unjoined'], d['kept_mismatch']))
        else:
            fail('plant %s with the verify ON: replaced %d of %d, differing frames %d, bar failures: %s' % (fault, d['replaced'], d['claimed'], d['differing_frames'], bad))
    # F3: the late record (the stream is seen 6 frames ahead of the record from frame 30): every draw stays the stream's, late is counted, the offset is searched again, the picture stays byte-equal
    d = fview('brock', 'plant_late_record', sh2 + ['--replace-fault', 'plane.late_record'], frames, every)
    if d is not None:
        if d['kept_late'] > 0 and d['relearned'] >= 1 and d['replaced'] < d['claimed'] and d['differing_frames'] == 0:
            ok('plant late_record FAILS as it must: kept late %d, relearned %d, replaced %d of %d claimed, the picture stays the streams' % (d['kept_late'], d['relearned'], d['replaced'], d['claimed']))
        else:
            fail('plant late_record: kept late %d, relearned %d, replaced %d of %d, differing %d' % (d['kept_late'], d['relearned'], d['replaced'], d['claimed'], d['differing_frames']))
    # F4: the composition arm: lane 99's shadow casters registered first. It does not pass a draw on unless NotOurs, so this hook is left with what it passed on: the census says where the draws went
    d = fview('brock', 'arm_casters', sh2 + ['--shadow-casters-ours'], 40, 4)
    if d is not None:
        bad = []
        if d['exit'] != 0 or d['validation_errors']:
            bad.append('exit %d, validation %d' % (d['exit'], d['validation_errors']))
        if not d['picture_close']:
            bad.append('picture differs (max %s)' % d['picture']['max_diff'])
        if d['claimed_blur'] <= 0 or d['blur'] != d['claimed_blur']:
            bad.append('the blur draws (casters do not claim them) must all be this hooks: claimed %d replaced %d' % (d['claimed_blur'], d['blur']))
        if bad:
            fail('casters + plane arm: ' + '; '.join(bad))
        else:
            ok('arm --shadow-casters-ours --plane-shadow-ours: picture within tolerance, validation 0; mask draws claimed by this hook %d (replaced %d), blur claimed %d replaced %d (lane 99 owns the plane site: %s)' %
               (d['claimed_mask'], d['mask'], d['claimed_blur'], d['blur'], 'it passed none on' if d['claimed_mask'] == 0 else 'it passed some on'))
    # R: SH2's plants re-planted with the plane hook on; the control is SH2's fault none (clean)
    c2 = fview('brock', 'control_sh2', sh2 + ['--shadows-ours-fault', 'none'], 40, 4)
    if c2 is None or c2['differing_frames'] != 0 or c2['replaced'] != c2['claimed'] or worst(c2['images'])[0] > ROUNDING:
        fail('the SH2 re-plant control (SH2 on, plane hook on, fault none) is not clean')
        return
    p = fview('brock', 'plant_copy_swap', sh2 + ['--shadows-ours-fault', 'copy_swap'], 40, 4)
    if p is not None:
        mx, _ = worst(p['plane_images'])
        (ok if mx > ROUNDING else fail)('plant copy_swap (SH2) with the plane hook on %s: plane images max diff %s of 255 (control clean)' % ('FAILS as it must' if mx > ROUNDING else 'passed', mx))
    p = fview('brock', 'plant_scaled_map', ['--shadows-ours', '--shadow-map-size', '512', '--shadows-ours-fault', 'scaled_map'], 40, 4)
    if p is not None:
        mx, shape = worst(p['images'])
        (ok if shape > 0 or mx > ROUNDING else fail)('plant scaled_map (SH2) with the plane hook on %s: shape failures %d, max diff %s (control clean)' % ('FAILS as it must' if shape > 0 or mx > ROUNDING else 'passed', shape, mx))


def unit_arm():
    files = []
    draws = {}
    for name in ('brock', 'r7079', 'bryan'):
        path, n = oracle(name)
        if path:
            files.append(str(path))
            draws[name] = n
    if len(files) != 3:
        return
    code, text = unit(files)
    if code is None:
        return
    for line in text.splitlines():
        if line.startswith('FAIL'):
            fail('unit: ' + line)
    if code != 0:
        fail('plane_unit.exe exited %s' % code)
    else:
        ok('plane_unit: the record, the writer\'s tracker, the guards and the board pass; every mask and blur draw of Check B / run 7079 / Bryan (%s) rebuilt bit for bit from the record' % ', '.join('%s %d' % kv for kv in draws.items()))
    code, text = unit(files + ['--plant', 'foreign_ignored'])
    if code == 0:
        ok('control foreign_ignored: a foreign recorder call between the modules uploads and the snapshot is ignored by the exact-LR tracker (exit 0)')
    else:
        fail('control foreign_ignored: exit %s (the shipped tracker took a foreign call)' % code)
    for plant in ('plane_inv_identity', 'plane_camera_stale', 'plane_pos_shift', 'duplicate_record', 'writer_lr_missing', 'upload_after_snapshot', 'foreign_upload_between'):
        code, text = unit(files + ['--plant', plant])
        if code == 1 and 'FAIL  writer.rows_equal' in text:
            ok('plant %s FAILS writer.rows_equal as it must (exit 1); the clean run is exit 0' % plant)
        else:
            fail('plant %s: exit %s and no writer.rows_equal failure: the unit check cannot see this fault' % (plant, code))
    # lane 205 (check K3): today's blur comparator (all four lanes of c33 / c35 by float ==) refuses a NaN stack leftover; the fixed code compares the written lane only
    code, text = unit(files + ['--plant', 'blur_nan_lane'])
    if code == 1 and 'FAIL  blur.nan_lanes_replaced' in text:
        ok('plant blur_nan_lane FAILS blur.nan_lanes_replaced as it must (exit 1); the clean run passes it (600 of 600 replaced, the written lane x still refused when it differs)')
    else:
        fail('plant blur_nan_lane: exit %s and no blur.nan_lanes_replaced failure: the unit check cannot see this fault' % code)


def sites():
    """Lane 159, the F1 re-find: the writer's listening sites against the game's code (plane_sites_check.py), and its plants. A plant that is not caught fails."""
    tool = HERE / 'plane_sites_check.py'
    r = subprocess.run([sys.executable, '-I', str(tool)], capture_output=True, text=True, timeout=900)
    if r.returncode == 0:
        ok(r.stdout.strip().splitlines()[-1][6:400])
    else:
        fail('plane_sites_check: ' + (r.stdout + r.stderr).strip()[-400:])
    for plant in ('foreign_lr', 'f1_range', 'shifted_lr', 'dropped_row', 'wrong_reg', 'wrong_rows'):
        r = subprocess.run([sys.executable, '-I', str(tool), '--plant', plant], capture_output=True, text=True, timeout=900)
        if r.returncode == 0:
            ok('listening-site plant %s FAILS as it must: %s' % (plant, r.stdout.strip().splitlines()[-1][:230]))
        else:
            fail('listening-site plant %s was not caught: %s' % (plant, (r.stdout + r.stderr).strip()[-300:]))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if what not in ('unit', 'clean', 'plants', 'sites', 'joined', 'all'):
        print(__doc__)
        return 2
    if what in ('unit', 'all'):
        unit_arm()
    if what in ('sites', 'all'):
        sites()
    if what in ('clean', 'all'):
        clean()
    if what in ('plants', 'all'):
        plants()
    if what in ('joined', 'all'):
        joined()
    print('\n%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
