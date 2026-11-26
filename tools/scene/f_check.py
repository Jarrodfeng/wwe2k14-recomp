r"""S103 lane F: the lane's one proof, offline. Runs from the repo root (or --root).
  py -I tools\scene\f_check.py [--root DIR]
1. out\tools\f_test.exe clean: exit 0, "0 failed" (the order rule, the record, the contributor, the hook drawn against the CPU model).
2. Each planted fault must make f_test exit 1 AND name the check it is meant to break (a twin that fails for another reason does not count).
3. The stream census: the see-through order is not a distance sort (f_order_stability.py on brock_losswin: 0 pairs flipped), and its planted twin (reverse_odd) flips pairs.
Exit 0 only when all three hold. Numbers only; nothing from the game is written."""
import argparse
import os
import subprocess
import sys

FAULTS = {
    'order_distance': 'order: game mode',
    'wrong_frame': 'draw: the hook',
    'additive_off': 'draw: the additive quad adds',
    'soft_off': 'soft: the soft batch is drawn with the soft program',   # S103 PRT: a soft batch drawn hard
    'stale_snapshot': 'contributor: a snapshot of the hook clock',
    'bad_index': 'record: the particle tables round-trip',
    'no_mirror': 'draw (viewer space): the same picture',
    'reader_scale': 'reader: the vertices are',
    'reader_gate': 'reader: an entry whose gate word',
    'reader_depth': 'reader: an entry whose +220 word is 0',   # S111 PARTZ: the entry's depth-ALWAYS word ignored
    'source_sview': 'source: the GAME-space view reaches BasisFromView',
    'reader_pivot': 'reader: the 3x3 pivot grid',   # S103 F3: the pivot grid ignored (every item centred)
    'reader_quad': 'reader: a quad takes its own matrix',   # S103 F3: a quad placed with the camera's axes
    'reader_blend': 'reader: blend_unproven counts',   # S103 F4: every blend code counted as proven
    'old_pairing': 'source: only the exact pairing',   # S103 F5: the node noted at the writer's frame is refused (the live runs: taken 0)
    'namer_diag_uncapped': 'namer diag: 12 misses',   # S103 PF: the miss diagnostic ignores its 8-line cap
    'namer_cached': 'namer: the log',   # S103 F4: the namer trusts the cached pointer instead of the handle id
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--capture', default=None)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    exe = os.path.join(root, 'out', 'tools', 'f_test.exe')
    if not os.path.isfile(exe):
        print('FAIL: %s is not built (tools\\scene\\build_f_test.ps1)' % exe)
        return 1
    bad = 0

    def run(args, timeout=300):
        return subprocess.run([exe] + args, capture_output=True, text=True, timeout=timeout, cwd=root)

    r = run([])
    out = r.stdout + r.stderr
    ok = r.returncode == 0 and ' 0 failed' in out and 'FAIL' not in out
    print('%s clean run: exit %d, %s' % ('PASS' if ok else 'FAIL', r.returncode, [l for l in out.splitlines() if 'checks,' in l][-1:] or '(no summary line)'))
    bad += not ok
    for fault, must_name in FAULTS.items():
        r = run(['--fault', fault])
        out = r.stdout + r.stderr
        named = any('FAIL' in l and must_name in l for l in out.splitlines())
        ok = r.returncode == 1 and named
        print('%s planted %-15s exit %d, the intended check %s' % ('PASS' if ok else 'FAIL', fault, r.returncode, 'failed' if named else 'DID NOT FAIL'))
        bad += not ok
    main_checkout = os.environ.get('RDR_MAIN_CHECKOUT')
    if not main_checkout:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main_checkout = os.path.dirname(common) if common else root
    cap = a.capture or os.path.join(main_checkout, 'out', 'captures', 'brock_losswin')
    tool = os.path.join(root, 'tools', 'scene', 'f_order_stability.py')
    r = subprocess.run([sys.executable, '-I', tool, cap, '--step', '3'], capture_output=True, text=True, timeout=900)
    ok = r.returncode == 0 and 'flipped 0' in r.stdout
    print('%s stream: see-through order is not a distance sort: %s' % ('PASS' if ok else 'FAIL', r.stdout.splitlines()[0] if r.stdout else r.stderr[-200:]))
    bad += not ok
    r = subprocess.run([sys.executable, '-I', tool, cap, '--step', '3', '--plant', 'reverse_odd'], capture_output=True, text=True, timeout=900)
    ok = r.returncode == 1
    print('%s stream twin (reverse_odd) must fail: exit %d' % ('PASS' if ok else 'FAIL', r.returncode))
    bad += not ok
    # The rig probe: a synthetic particle record drawn through rf::BuildFrame's viewer-space frame, landing on the middle of the picture; its twin (no mirror) must fail.
    rig = os.path.join(root, 'tools', 'scene', 'frame_rig.py')
    if not os.path.isfile(os.path.join(root, 'out', 'tools', 'frame_rig.exe')) or not os.path.isdir(cap):
        print('FAIL rig probe: out\\tools\\frame_rig.exe (tools\\scene\\build_rig.ps1) or the capture is missing')
        bad += 1
    else:
        env = dict(os.environ, WWE_FVIEW_ALLOW_STALE='1')  # the game-picture side (fview) is not used by the probe
        base = [sys.executable, '-I', rig, '--capture', cap, '--frames', '110', '--mask', '0x0', '--repeat', '1']
        r = subprocess.run(base + ['--out', os.path.join(root, 'out', 's103_f_probe'), '--particles-probe'], capture_output=True, text=True, timeout=1800, env=env)
        ok = 'particle_probe: point' in r.stdout and 'FAIL  frame' not in r.stdout.replace('FAIL  empty panel', '')
        print('%s rig probe: particles through BuildFrame land on the middle of the picture: %s' % ('PASS' if ok else 'FAIL', [l for l in r.stdout.splitlines() if 'particle_probe' in l][:1]))
        bad += not ok
        r = subprocess.run(base + ['--out', os.path.join(root, 'out', 's103_f_probe_fault'), '--particles-probe-fault', 'no_mirror', '--expect-fail', 'particle_probe FAIL'], capture_output=True, text=True, timeout=1800, env=env)
        ok = r.returncode == 0 and 'planted fault caught' in r.stdout
        print('%s rig probe twin (no mirror) must fail the probe: exit %d' % ('PASS' if ok else 'FAIL', r.returncode))
        bad += not ok
    print('s103_F: %s' %('FAIL (%d)' % bad if bad else 'all PASS'))
    return 1 if bad else 0


sys.exit(main())
