r"""Lane 136 (SH3 slice 1) gate driver: the circle blob shadows drawn by ours from the scene record (docs\renderer\SHADOW_CIRCLES.md). 0 game runs.

  py -I tools\shadow\circles_ours_check.py unit      half (a): the capture's circle constants -> guest memory -> the SHIPPED writer's tracker -> record -> BuildRows equals the stream's constants
                                                      bit for bit on Check B, run 7079 and Bryan (tools\shadow\circles_unit.cpp); the record, guard and board checks; plants circle_bind_pose,
                                                      circle_join_by_index, circle_no_fade each must FAIL that check
  py -I tools\shadow\circles_ours_check.py clean     half (b): fview --circles-check plays each capture in lockstep (A = the stream, B = the circle hook fed from the oracle record):
                                                      every circle draw replaced (kept 0), the stamp -> frame offset learned, the mesh equal to the stream's vertex bytes, the picture byte-equal on
                                                      every frame, validation 0; then once more with SH2's passes on in both arms (B's circle draws land in our shadow map)
  py -I tools\shadow\circles_ours_check.py sizes     the clean check again with SH2's maps at 1024 and 256 px in both arms
  py -I tools\shadow\circles_ours_check.py plants    each planted fault, run without the verify check so it reaches the picture, must change the picture while its control (the same arguments, no
                                                      fault) is byte-equal: circle_bind_pose, circle_join_by_index, circle_no_fade, and SH2's floor_reads_wrestler_map re-planted with the circles on;
                                                      the verify check itself must refuse the planted join (replaced < claimed)
  py -I tools\shadow\circles_ours_check.py all

Needs this checkout's out\renderer\dev\fview.exe (tools\fview\build.ps1) and out\shadow\circles_unit.exe (tools\shadow\build_circles_unit.ps1) and the three captures under the main checkout's out\
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
UNIT = ROOT / 'out' / 'shadow' / 'circles_unit.exe'
OUT = ROOT / 'out' / 'l136'
CAPS = {'brock': MAIN / 'out' / 'lane86' / 'brock' / 'capture',
        'bryan': MAIN / 'out' / 'lane86' / 'bryan' / 'capture',
        'r7079': MAIN / 'out' / 's101' / 'lane92' / 'case_20261112_122658_040_0_pc_run_exit' / 'capture'}
VICTORY_ZIP = MAIN / 'out' / 's101' / 'runs' / '20261113_063415_s101_sweep_victory' / 'case_20261113_063616_724_0_pc_run_exit.zip'   # lane 215: a LIVE case zip (run 7132, the victory scene): a bit-equal NaN ViewProj on 4 draws, 3 read-back splits
JOIN_UNIT = ROOT / 'out' / 'shadow' / 'circles_join_unit.exe'
K3_ZIP = MAIN / 'out' / 's101' / 'runs' / '20261113_035022_s100_owner_s89_brock' / 'case_20261113_035137_389_0_pc_run_exit.zip'   # lane 205: a LIVE case zip (run 7094) whose record the OLD writer wrote
fails = []
NFRAMES = {}   # capture -> the frames the oracle holds (set by oracle())


def fail(msg):
    fails.append(msg)
    print('FAIL  ' + msg)


def ok(msg):
    print('ok    ' + msg)


def oracle(name):
    """Dumps the capture's circle constants (tools\\shadow\\circles_oracle.py); returns the file path and the draw count."""
    cap = CAPS[name]
    if not (cap / 'images.bin').exists():
        fail('cannot read %s (a check that reads nothing must fail)' % cap)
        return None, 0
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / (name + '.circ')
    r = subprocess.run([sys.executable, '-I', str(HERE / 'circles_oracle.py'), 'dump', str(cap), str(path)], capture_output=True, text=True, timeout=1200)
    print('      ' + (r.stdout.strip() or r.stderr.strip()).splitlines()[-1])
    if r.returncode != 0:
        fail('%s: the oracle dump failed / the ViewProj cross-check failed: %s' % (name, (r.stdout + r.stderr).strip()[-300:]))
        return None, 0
    m = re.search(r'(\d+) circle draws', r.stdout)
    mf = re.search(r': (\d+) frames', r.stdout)
    NFRAMES[name] = int(mf.group(1)) if mf else 0
    return path, int(m.group(1)) if m else 0


def unit(args, label):
    if not UNIT.exists():
        fail('%s does not exist (tools\\shadow\\build_circles_unit.ps1)' % UNIT)
        return None, ''
    r = subprocess.run([str(UNIT)] + args, capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout + r.stderr


def fview(name, label, extra, frames, every=1, tries=8):
    if not FVIEW.exists():
        fail('%s does not exist (tools\\fview\\build.ps1)' % FVIEW)
        return None
    cap = CAPS[name]
    path = OUT / (name + '.circ')
    js = OUT / (label + '.json')
    if js.exists():
        js.unlink()
    cmd = [str(FVIEW), '--capture', str(cap), '--root', str(ROOT), '--validation', '1', '--circles-check', str(js), '--circles-oracle', str(path), '--check-every', str(every)]
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
        m = re.search(r'claimed (\d+) replaced (\d+)', rep)
        d['claimed'], d['replaced'] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        m = re.search(r'join offset (-?\d+)', rep)
        d['offset'] = int(m.group(1)) if m else None
        m = re.search(r'mesh checked (\d+) bad (\d+)', rep)
        d['mesh_checked'], d['mesh_bad'] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        m = re.search(r'mismatch (\d+) \(shape (\d+), constants (\d+), late (\d+), mesh wait (\d+)\)', rep)
        d['kept_mismatch'] = int(m.group(1)) if m else -1
        d['kept_late'] = int(m.group(4)) if m else -1
        d['kept_rows'] = int(m.group(3)) if m else -1
        m = re.search(r'relearned (\d+)', rep)
        d['relearned'] = int(m.group(1)) if m else -1
        m = re.search(r'mesh checked (\d+) bad (\d+) \(index (\d+)\) unreadable (\d+)', rep)
        d['index_bad'], d['mesh_unreadable'] = (int(m.group(3)), int(m.group(4))) if m else (-1, -1)
        m = re.search(r'unjoined (\d+)', rep)
        d['kept_unjoined'] = int(m.group(1)) if m else -1
        return d
    fail('%s: the GPU stayed busy' % label)
    return None


def judge_clean(d, expected_draws, label):
    bad = []
    if d['exit'] != 0 or d['validation_errors']:
        bad.append('exit %d, validation errors %d' % (d['exit'], d['validation_errors']))
    if d['compared'] < 1:
        bad.append('no frame compared')
    if d['claimed'] != expected_draws:
        bad.append('claimed %d circle draws, the oracle has %d' % (d['claimed'], expected_draws))
    if d['replaced'] != d['claimed'] or d['replaced'] <= 0:
        bad.append('replaced %d of %d claimed (kept: mismatch %d, unjoined %d)' % (d['replaced'], d['claimed'], d['kept_mismatch'], d['kept_unjoined']))
    if d['offset'] != 1:
        bad.append('the stamp -> frame offset learned is %s, the oracle stamps 1 before the header frame' % d['offset'])
    if d['mesh_checked'] != 1 or d['mesh_bad'] != 0:
        bad.append('mesh checked %d bad %d (the record\'s mesh must equal the stream draw\'s vertex bytes once)' % (d['mesh_checked'], d['mesh_bad']))
    if d['differing_frames'] != 0 or d['picture']['max_diff'] != 0:
        bad.append('the picture differs from the stream on %d frames (max diff %s of 255)' % (d['differing_frames'], d['picture']['max_diff']))
    return bad


def clean():
    for name in ('brock', 'r7079', 'bryan'):
        path, draws = oracle(name)
        if not path:
            continue
        d = fview(name, 'clean_' + name, [], 0)
        if d is None:
            continue
        bad = judge_clean(d, draws, name)
        if bad:
            for b in bad:
                fail('%s: %s' % (name, b))
        else:
            ok('%s: %d of %d circle draws replaced, offset %d learned, mesh equal to the stream\'s bytes, picture byte-equal on %d frames, validation 0' % (name, d['replaced'], d['claimed'], d['offset'], d['compared']))
    # with SH2's passes on in both arms: the replaced draws land in our shadow maps
    path, draws = oracle('brock')
    if path:
        d = fview('brock', 'clean_brock_sh2', ['--shadows-ours', '--shadow-map-size', '512'], 0, every=2)
        if d is not None:
            bad = judge_clean(d, draws, 'brock+sh2')
            if bad:
                for b in bad:
                    fail('brock with SH2: %s' % b)
            else:
                ok('brock with SH2 on in both arms: %d circle draws replaced into our shadow map, picture byte-equal on %d frames' % (d['replaced'], d['compared']))


def sizes():
    path, draws = oracle('brock')
    if not path:
        return
    for size in (1024, 256):
        d = fview('brock', 'clean_brock_sh2_%d' % size, ['--shadows-ours', '--shadow-map-size', str(size)], 0, every=4)
        if d is None:
            continue
        # A = the stream with SH2 at this size, B = the same plus the circle hook: the circles must not change the picture at any map size
        bad = judge_clean(d, draws, 'brock sh2 %d' % size)
        if bad:
            for b in bad:
                fail('brock with SH2 at %d: %s' % (size, b))
        else:
            ok('brock with SH2 maps at %d px in both arms: %d circle draws replaced into our shadow map, picture byte-equal on %d frames' % (size, d['replaced'], d['compared']))


def plants():
    path, draws = oracle('brock')
    if not path:
        return
    frames, every = 120, 2
    control = fview('brock', 'control_noverify', ['--replace-fault', 'circles.no_verify'], frames, every)
    if control is None or control['differing_frames'] != 0 or control['replaced'] <= 0:
        fail('the control (no_verify, no fault) is not clean: %s' % ('no run' if control is None else 'differs on %d frames, replaced %d' % (control['differing_frames'], control['replaced'])))
        return
    ok('control (circles.no_verify, no fault): %d draws replaced, picture byte-equal on %d frames' % (control['replaced'], control['compared']))
    for fault, why in (('bind_pose', 'circle_bind_pose'), ('join_by_index', 'circle_join_by_index'), ('no_fade', 'circle_no_fade')):
        d = fview('brock', 'plant_' + fault, ['--replace-fault', 'circles.no_verify,circles.' + fault], frames, every)
        if d is None:
            continue
        if d['differing_frames'] > 0 and d['picture']['max_diff'] > 0:
            ok('plant %s FAILS as it must: the picture differs on %d of %d frames (max diff %s of 255); its control is byte-equal' % (why, d['differing_frames'], d['compared'], d['picture']['max_diff']))
        else:
            fail('plant %s passed: the picture check cannot see this fault' % why)
    # the verify check refuses the planted join on its own
    d = fview('brock', 'plant_join_verified', ['--replace-fault', 'circles.join_by_index'], frames, every)
    if d is not None:
        if d['replaced'] < d['claimed'] and d['differing_frames'] == 0:
            ok('the verify check refuses the planted join by itself: replaced %d of %d claimed, the picture stays the stream\'s' % (d['replaced'], d['claimed']))
        else:
            fail('verify did not refuse the planted join (replaced %d of %d, differing %d)' % (d['replaced'], d['claimed'], d['differing_frames']))
    # F2: the mesh check refuses a wrong index and an unreadable stream; F3: a late record is counted, the offset is relearned; a locked offset is never found again
    for fault, key in (('index_corrupt', 'index_bad'), ('mesh_unreadable', 'mesh_unreadable')):
        d = fview('brock', 'plant_' + fault, ['--replace-fault', 'circles.' + fault], 20, 1)
        if d is None:
            continue
        if d['replaced'] == 0 and d['claimed'] > 0 and d[key] > 0 and d['differing_frames'] == 0:
            ok('plant %s FAILS as it must: 0 of %d draws replaced (%s %d), the picture stays the stream\'s' % (fault, d['claimed'], key, d[key]))
        else:
            fail('plant %s passed: replaced %d of %d, %s %d' % (fault, d['replaced'], d['claimed'], key, d[key]))
    d = fview('brock', 'plant_late_record', ['--replace-fault', 'circles.late_record'], 120, 1)
    if d is not None:
        # a late record is a record that is not there yet: it is COUNTED as late (not as unjoined), the offset is searched again, the draws stay the stream's (picture unchanged); nothing can recover it
        if d['kept_late'] > 0 and d['relearned'] >= 1 and d['kept_unjoined'] == 0 and d['differing_frames'] == 0 and d['replaced'] < d['claimed']:
            ok('plant late_record is SEEN: late %d (unjoined %d), relearned %d, replaced %d of %d, the picture stays the stream\'s' % (d['kept_late'], d['kept_unjoined'], d['relearned'], d['replaced'], d['claimed']))
        else:
            fail('plant late_record: late %d unjoined %d relearned %d replaced %d of %d differing %d' % (d['kept_late'], d['kept_unjoined'], d['relearned'], d['replaced'], d['claimed'], d['differing_frames']))
    # the offset moves by one mid-run (record 40 on): the relearn finds it again within 9 draws; with the offset locked it never does
    d = fview('brock', 'plant_offset_shift', ['--replace-fault', 'circles.offset_shift'], 120, 1)
    if d is not None:
        if d['relearned'] >= 1 and d['replaced'] >= d['claimed'] - 12 and d['offset'] == 0 and d['differing_frames'] == 0:
            ok('offset_shift (the offset moves 1 -> 0 at record 40): relearned %d, replaced %d of %d (at most 12 draws lost), picture byte-equal' % (d['relearned'], d['replaced'], d['claimed']))
        else:
            fail('offset_shift: relearned %d offset %s replaced %d of %d differing %d' % (d['relearned'], d['offset'], d['replaced'], d['claimed'], d['differing_frames']))
    d = fview('brock', 'plant_offset_static_lock', ['--replace-fault', 'circles.offset_shift,circles.offset_static_lock'], 120, 1)
    if d is not None:
        if d['replaced'] < d['claimed'] - 100 and d['relearned'] == 0:
            ok('plant offset_static_lock FAILS as it must: the offset never relearned after it moved, replaced %d of %d' % (d['replaced'], d['claimed']))
        else:
            fail('plant offset_static_lock passed: replaced %d of %d, relearned %d' % (d['replaced'], d['claimed'], d['relearned']))
    # R: SH2's floor_reads_wrestler_map (Session 98's long stretched shadows), re-planted with the circles on: control = SH2 fault none
    sh2 = ['--shadows-ours', '--shadow-map-size', '512']
    c2 = fview('brock', 'control_floor', sh2 + ['--shadows-ours-fault', 'none'], 40, 4)
    p2 = fview('brock', 'plant_floor', sh2 + ['--shadows-ours-fault', 'floor_reads_wrestler_map'], 40, 4)
    if c2 is not None and p2 is not None:
        if c2['differing_frames'] != 0 or c2['replaced'] != c2['claimed'] or c2['replaced'] <= 0:
            fail('the floor_reads_wrestler_map control (SH2 on, circles on, fault none) is not clean: differs on %d frames, replaced %d of %d' % (c2['differing_frames'], c2['replaced'], c2['claimed']))
        elif p2['picture']['max_diff'] > 8:
            ok('plant floor_reads_wrestler_map (SH2, circles on) FAILS as it must: picture max diff %s of 255; its control (fault none) is byte-equal' % p2['picture']['max_diff'])
        else:
            fail('plant floor_reads_wrestler_map passed with the circles on: picture max diff %s' % p2['picture']['max_diff'])


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
    # lane 205 (check K3): the mesh against LIVE bytes, not against a fake guest built by the rule under test. The zip is the past-bug re-find: stream == record 0 of 544 draws (the old writer), the shipped
    # conversion rebuilds the stream's bytes from the record's raw guest id word.
    lm = OUT / 'k3.lmesh'
    if not K3_ZIP.exists():
        fail('cannot read %s (a check that reads nothing must fail)' % K3_ZIP)
    else:
        r = subprocess.run([sys.executable, '-I', str(HERE / 'circles_live_mesh.py'), 'dump', str(K3_ZIP), str(lm)], capture_output=True, text=True, timeout=1200)
        print('      ' + (r.stdout.strip() or r.stderr.strip()).splitlines()[-1])
        if r.returncode != 0:
            fail('the live mesh dump failed: %s' % (r.stdout + r.stderr).strip()[-300:])
        else:
            files += ['--live-mesh', str(lm)]
    code, text = unit(files, 'clean')
    if code is None:
        return
    for line in text.splitlines():
        if line.startswith('FAIL'):
            fail('unit: ' + line)
    if code != 0:
        fail('circles_unit.exe exited %s' % code)
    else:
        ok('circles_unit: the record, the writer\'s tracker, the guards and the board pass; every circle draw of Check B / run 7079 / Bryan (%s) rebuilt bit for bit from the record' % ', '.join('%s %d' % kv for kv in draws.items()))
    for plant, name, where in (('bind_pose', 'circle_bind_pose', 'FAIL  circles.rows'), ('join_by_index', 'circle_join_by_index', 'FAIL  circles.rows'), ('no_fade', 'circle_no_fade', 'FAIL  circles.rows'),
                               ('mesh_header_as_data', 'mesh_header_as_data', 'FAIL  writer.mesh_through_resource_objects'), ('id_word_kept', 'id_word_kept', 'FAIL  live_mesh.guest_to_host'), ('stale_viewproj', 'stale_viewproj', 'FAIL  writer.viewproj_not_stale')):
        code, text = unit(files + ['--plant', plant], plant)
        if code == 1 and where in text:
            ok('plant %s FAILS %s as it must (exit 1); the clean run is exit 0' % (name, where[6:]))
        else:
            fail('plant %s: exit %s and no "%s": the unit check cannot see this fault' % (name, code, where))


def join_arm():
    """Lane 215: the comparator (a bit-equal NaN is equal) and the keeper (a read-back split's first part carries the record the second part takes), then the REAL victory capture through fview."""
    if not JOIN_UNIT.exists():
        fail('%s does not exist (tools/shadow/build_circles_unit.ps1)' % JOIN_UNIT)
        return
    r = subprocess.run([str(JOIN_UNIT)], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        fail('circles_join_unit: ' + ' | '.join(l for l in r.stdout.splitlines() if l.startswith('FAIL')))
    else:
        ok('circles_join_unit: the comparator accepts a bit-equal NaN and still refuses real differences; every frame (read-back parts included) holds the stamp j-1 for the circle and the plane board')
    for plant, where in (('float_compare', 'FAIL  comparator.nan_pair_equal'), ('no_part_record', 'FAIL  keeper.every_frame_has_its_stamp')):
        r = subprocess.run([str(JOIN_UNIT), '--plant', plant], capture_output=True, text=True, timeout=600)
        if r.returncode == 1 and where in r.stdout:
            ok('plant %s FAILS %s as it must' % (plant, where[6:]))
        else:
            fail('plant %s: exit %s and no "%s"' % (plant, r.returncode, where))
    if not VICTORY_ZIP.exists():
        fail('cannot read %s (a check that reads nothing must fail)' % VICTORY_ZIP)
        return
    if not FVIEW.exists():
        fail('%s does not exist (tools/fview/build.ps1)' % FVIEW)
        return
    arms = {}
    for label, extra in (('new', []), ('old_comparator', ['--replace-fault', 'circles.float_compare'])):
        cmd = [str(FVIEW), '--case', str(VICTORY_ZIP), '--root', str(ROOT), '--validation', '1', '--frames', '1793', '--circles-ours'] + extra
        text = ''
        for attempt in range(8):
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
            text = r.stdout + r.stderr
            if 'Not enough graphics memory' in text:
                time.sleep(20)
                continue
            break
        lines = [l for l in text.splitlines() if 'circles: frames' in l]
        # (stdout and stderr are concatenated, so the last line is not the last window: take the line of the highest frame count)
        lines.sort(key=lambda l: int(re.search(r'circles: frames (\d+)', l).group(1)))
        arms[label] = (lines[-1] if lines else '', r.returncode, 'validation errors 0' in text)
    for label, (line, code, valid) in arms.items():
        m = re.search(r'claimed (\d+) replaced (\d+) \| kept: unjoined (\d+), mismatch (\d+) \(shape (\d+), constants (\d+), late (\d+)', line)
        arms[label] = (m.groups() if m else None, code, valid)
    n, o = arms['new'], arms['old_comparator']
    if not n[0] or not o[0]:
        fail('victory capture: no circles line from fview (new %s, old %s)' % (n, o))
        return
    claimed, replaced, unjoined, mismatch, shape, consts, late = (int(x) for x in n[0])
    # the first replayed frame has no previous record (not ready, 1 frame of 5 draws), everything else must be ours
    if consts == 0 and late == 0 and unjoined == 0 and n[2] and n[1] == 0:
        ok('victory capture (run 7132): constants refusals 0, late 0 (replaced %d of %d; the first frame has no record before it); validation 0' % (replaced, claimed))
    else:
        fail('victory capture: constants %d late %d unjoined %d validation %s exit %s' % (consts, late, unjoined, n[2], n[1]))
    oc = int(o[0][5])
    if oc >= 4:
        ok('the old comparator (plant float_compare) refuses %d constants on the same capture: the check sees the fault' % oc)
    else:
        fail('plant float_compare did not refuse the NaN draws (constants %d): the capture cannot see the fault' % oc)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if what not in ('unit', 'clean', 'sizes', 'plants', 'join', 'all'):
        print(__doc__)
        return 2
    if what in ('unit', 'all'):
        unit_arm()
    if what in ('clean', 'all'):
        clean()
    if what in ('sizes', 'all'):
        sizes()
    if what in ('plants', 'all'):
        plants()
    if what in ('join', 'all'):
        join_arm()
    print('\n%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
