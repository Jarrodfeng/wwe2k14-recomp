#!/usr/bin/env python3
"""Lane 217 (Caster D3 E2b, the seam; docs/renderer/SKIP_GAME_WORK_MAP.md 17.9): the caster class's D3 chain through the REAL binder on a kept capture that carries caster pass draws, 0 game runs.

  py -I tools\\d3\\d3_caster_check.py [--root DIR] [--capture CAPTURE_DIR | --zip CASE_ZIP] [--no-build] [--frames N] [--plants a,b]

fview --d3 casters (binder.h D3C* functions, d3_binder.h route Casters): the guest hook is EMULATED on the capture (D3CEmulateReader -> EmulateHook runs the shipped DecideCall with the caster class's own
switch for every caster pass entry of the frame and turns the skipped runs into caster markers), the binder draws each marker from its learned sequence and the packet through BindDraw. Two passes: pass 1
writes the offline ledger (drift registers), pass 2 reads it. Mode stream: the packet carries the stream's own banks (the emulated record fields), so a marker's composed banks are compared with the stream's
truth for every part (readiness) and the proof frames compare the OLD sequence with the full path (proof compare).

Bars (each line PASS / FAIL; a missing input FAILS):
  stream    pass 2 (ledger in): markers > 0; part draws > 0; holes 0; no sequence 0; ordinal mismatch 0; unconsumed 0; sequence length differs 0; proof mismatches 0; calls = runs differ 0 (the census guard of the
            run grouping); NOT-LEARNABLE caster runs 0; readiness: every marker part's banks byte-equal to the stream's (differing 0 of composed N); validation errors 0.
  groups    (python, from the capture's own records) per frame and (pass, model): the number of runs the binder's rule gives equals the number of CasterCall entries of the record (the [L] of 17.8 item 5).
  picture   INFO only: the picture of every frame equals the arm without D3. It is insensitive to the caster draws on a capture (the shadow images arrive as captured contents), which the plants show
            (several corrupt the markers and leave the picture equal), so it is not a bar.
  plants    each is run in the stream arm and must change the census as named: hole_no_packet / join_by_index / offset0 / marker_count0 -> holes > 0; ordinal_off -> ordinal mismatch > 0;
            skip_in_proof_frame -> a skip in a proof frame (> 0); ordinal_merge / key_by_pass_wrong -> the sequence census moves (part draws differ from the honest arm);
            moving_static / placed_identity (the world from the template, not the packet) -> proof mismatches > 0 and fewer markers.
  extras        (lane 221, 17.10) --d3-mode record with D3_EMULATE_EXTRAS=1 (fview fills the CasterExtra table from the STREAM: the join, the transposition, the collapsed-slot set and the plants are tested, NOT
              the live reader's values, which hook.extra_per_entry / hook.pose_point (caster_rows_selftest.py) and the next verify run's census prove): holes 0, record missing 0, composed banks
              differing from the stream 0, proof mismatches of MARKED objects 0, and marked: rim > 0, map ordinal 0 (map A + the props) and ordinal 1 (map B) > 0. Plants rim_camera_prev_frame, rim_camera_from_r
              (the rim not marked), collapse_off, collapse_shift, collapse_point_stale (marked objects drop below the honest arm's), each run in the same arm.
              Without the table the same arm marks no rim (the 595 rim parts mismatch c56-59): INFO line.
  record    INFO: the same capture in --d3-mode record (the scene records as the source): which objects the caster registers would mark and the named reasons for the rest (E3 / E4 / E5 status)."""
import argparse
import collections
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))

LINE = re.compile(r'\[rdr\]\[d3\]\[casters\] mode (\w+): markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), learned (\d+), objects complete \(marked (\d+) of (\d+)\), proof objects (\d+) '
                  r'\(parts (\d+), mismatching (\d+), of marked objects (\d+)\), ordinal mismatch (\d+), unconsumed (\d+), gate resets (\d+), calls = runs checked (\d+) differ (\d+), record missing (\d+), hook: '
                  r'markers (\d+) dropped (\d+) .*?readiness: marker parts composed (\d+), differing from the stream (\d+), register reads (\d+), withheld (\d+), epoch drops (\d+), sequence length differs (\d+), '
                  r'never proved (\d+), marked with an unsourced register (\d+), latched (\d+), sink-skipped frames (\d+), skipped in proof frames (\d+)')
KEYS = ['mode', 'markers', 'parts', 'holes', 'no_seq', 'learned', 'marked', 'complete_of', 'proof_obj', 'proof_parts', 'proof_mism', 'proof_mism_marked', 'ord_mism', 'unconsumed', 'gate_resets', 'calls_checked',
        'calls_differ', 'record_missing', 'h_markers', 'h_dropped', 'composed', 'differ', 'reads', 'withheld', 'epoch', 'seq_len', 'never', 'unsrc', 'latched', 'sink_skipped', 'skipped_in_proof']
RUNS = re.compile(r'runs seen \(map/plane/rim\): (\d+)/(\d+)/(\d+), not learnable: (\d+)/(\d+)/(\d+)')


def run(cmd, timeout, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_csv(path):
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            f, h, d, b = line.strip().split(',')
            rows[int(f)] = h
    return rows


def groups_check(cap_dir, frames):
    """Per sampled frame and (pass, model): runs by the binder's rule (consecutive draws of one model at one pass site with increasing part indices) against the record's CasterCall entries (both pairings)."""
    import ledger as L
    cap = L.Capture(cap_dir)
    site = {0x82C04634: 1, 0x82BF4D38: 2, 0x82BFBD58: 3}
    keys = bad = n_frames = 0
    for fi in range(60, frames - 2, 20):
        f = cap.frame(fi)
        runs = collections.Counter()
        last, lastpart = None, -1
        for i in range(f.ndraws):
            o = f.origin(i)
            p = site.get(o['pass_site'])
            if not p or not o['object']:
                last = None
                continue
            if last != (o['object'], p) or o['part'] <= lastpart:
                runs[(p, o['object'])] += 1
            last, lastpart = (o['object'], p), o['part']
        ent = collections.Counter((c['pass_'], c['object']) for c in cap.record(fi).caster_calls())
        diff = [k for k in set(runs) | set(ent) if runs[k] != ent[k]]
        keys += len(set(runs) | set(ent))
        bad += len(diff)
        n_frames += 1
    return n_frames, keys, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture', default='')
    ap.add_argument('--zip', default='')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--plants', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    out = os.path.join(root, 'out', 'd3caster')
    os.makedirs(out, exist_ok=True)
    cap = args.capture
    if not cap:
        z = args.zip or os.path.join(main_co, 'out', 's101', 'runs', '20261113_081920_s100_owner_s89_brock', 'case_20261113_082000_320_0_pc_run_exit.zip')
        if not os.path.isfile(z):
            print('FAIL  input missing: the caster verify capture (run 7147; the older run 7138 zip is a fallback via --zip) ' + z)
            return 1
        d = os.path.join(out, 'z7147')
        if not os.path.isfile(os.path.join(d, 'capture', 'images.bin')):
            zipfile.ZipFile(z).extractall(d)
        cap = os.path.join(d, 'capture')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc != 0:
            print(text[-2500:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218's locator (this tool came after it: run 7149's check first ran a stale exe)
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    n = args.frames or len([f for f in os.listdir(os.path.join(cap, 'frames'))])
    cache = {}

    def arm(tag, extra, d3=True, env=None):
        if tag in cache:
            return cache[tag]
        d = os.path.join(out, 'arm_' + tag)
        cmd = [exe, '--capture', cap, '--game', game, '--frames', str(n), '--thin-run', d] + (['--d3', 'casters'] + extra if d3 else extra)
        rc, text = 1, ''
        for attempt in range(8):   # (the GPU is shared: this exact message is retried, nothing else)
            rc, text = run(cmd, 1500, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                import time
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, rc))
        m = LINE.search(text)
        res = {'rc': rc, 'rows': read_csv(csv), 'text': text, 'c': dict(zip(KEYS, [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None}
        r = RUNS.search(text)
        res['runs'] = list(map(int, r.groups())) if r else None
        cache[tag] = res
        return res

    try:
        base = arm('base', [], d3=False)
        ledger = os.path.join(out, 'ledger.txt')
        p1 = arm('p1', ['--d3-mode', 'stream', '--d3-ledger-out', ledger])
        s = arm('p2', ['--d3-mode', 'stream', '--d3-ledger-in', ledger])
        c = s['c']
        if not c:
            verdict(False, 'stream: no [rdr][d3][casters] census line')
            return 1
        notl = sum(s['runs'][3:6]) if s['runs'] else -1
        verdict(c['markers'] > 0 and c['parts'] > 0 and c['complete_of'] > 0, 'stream (ledger in): %d markers, %d part draws synthesized, %d objects marked of %d, runs seen map/plane/rim %s' % (c['markers'], c['parts'], c['marked'], c['complete_of'], '/'.join(map(str, s['runs'][:3])) if s['runs'] else '?'))
        verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and c['seq_len'] == 0 and c['epoch'] == 0 and c['record_missing'] == 0,
                'holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, sequence length differs %d, epoch drops %d' % (c['holes'], c['no_seq'], c['ord_mism'], c['unconsumed'], c['seq_len'], c['epoch']))
        verdict(c['proof_mism'] == 0 and c['proof_parts'] > 0 and c['skipped_in_proof'] == 0 and c['never'] == 0 and c['unsrc'] == 0,
                'proof compare: %d objects, %d parts, mismatching %d (marked objects %d), skipped in a proof frame %d, never proved %d, marked with an unsourced register %d' % (c['proof_obj'], c['proof_parts'], c['proof_mism'], c['proof_mism_marked'], c['skipped_in_proof'], c['never'], c['unsrc']))
        verdict(c['calls_differ'] == 0 and c['calls_checked'] > 0 and notl == 0, 'calls = runs checked %d differ %d; caster runs not learnable %d' % (c['calls_checked'], c['calls_differ'], notl))
        verdict(c['composed'] > 0 and c['differ'] == 0 and c['withheld'] == 0, 'marker banks: %d marker parts composed, %d differing from the stream (%d register reads, withheld %d)' % (c['composed'], c['differ'], c['reads'], c['withheld']))
        verdict(s['rc'] == 0 and 'validation errors 0' in s['text'], 'validation layer: errors 0 (exit %d)' % s['rc'])
        nf, keys, bad = groups_check(cap, n)
        verdict(bad == 0 and keys > 0, 'groups: %d sampled frames, %d (pass, model) keys: runs by the binder\'s rule equal the record\'s CasterCall entries on all but %d' % (nf, keys, bad))
        for tag, a in (('pass 1 (no ledger)', p1), ('pass 2 (ledger in)', s)):
            dd = [f for f in sorted(base['rows']) if a['rows'].get(f) != base['rows'][f]]
            print('INFO  picture, %s vs the arm without D3: %d of %d frames differ' % (tag, len(dd), len(base['rows'])))
        # plants
        want = {'hole_no_packet': lambda x: x['holes'] > 0, 'join_by_index': lambda x: x['holes'] > 0, 'offset0': lambda x: x['holes'] > 0, 'marker_count0': lambda x: x['holes'] > 0,
                'ordinal_off': lambda x: x['ord_mism'] > 0, 'skip_in_proof_frame': lambda x: x['skipped_in_proof'] > 0,
                'ordinal_merge': lambda x: x['parts'] != c['parts'] or x['ord_mism'] > 0 or x['calls_differ'] > 0,
                'key_by_pass_wrong': lambda x: x['parts'] != c['parts'] or x['ord_mism'] > 0 or x['proof_mism'] > 0,
                'moving_static': lambda x: x['proof_mism'] > 0 and x['markers'] < c['markers'], 'placed_identity': lambda x: x['proof_mism'] > 0 and x['markers'] < c['markers']}
        only = set(x for x in args.plants.split(',') if x)
        for pl, test in want.items():
            if only and pl not in only:
                continue
            x = arm('pl_' + pl, ['--d3-mode', 'stream', '--d3-ledger-in', ledger, '--d3-fault', pl])
            xc = x['c']
            verdict(bool(xc) and test(xc), 'plant %s detected: markers %s, part draws %s, holes %s, ordinal mismatch %s, proof mismatching %s, skipped in a proof frame %s' % (
                pl, xc and xc['markers'], xc and xc['parts'], xc and xc['holes'], xc and xc['ord_mism'], xc and xc['proof_mism'], xc and xc['skipped_in_proof']))
        r = arm('record', ['--d3-mode', 'record', '--d3-ledger-in', ledger])
        rc = r['c']
        if rc:
            print('INFO  record mode: %d markers, %d of %d objects marked, proof mismatching %d, record missing %d' % (rc['markers'], rc['marked'], rc['complete_of'], rc['proof_mism'], rc['record_missing']))
        for l in r['text'].splitlines():
            if l.startswith('[rdr][d3][casters] not marked') or l.startswith('[rdr][d3][casters] proof mismatch registers'):
                print('INFO  ' + l[:400])
        # lane 221: the record route WITH the CasterExtra table (emulated from the stream by fview), the rim / map A / map B / props map marked, and the plants
        env = dict(os.environ, D3_EMULATE_EXTRAS='1')
        e0 = arm('extras', ['--d3-mode', 'record', '--d3-ledger-in', ledger], env=env)
        ec = e0['c']
        mk = lambda text: {(m.group(2), int(m.group(3))): int(m.group(1)) for m in re.finditer(r'not marked: (\d+) objects: marked: (\w+) ordinal (\d+)', text)}  # noqa: E731
        marked = mk(e0['text'])
        total = sum(marked.values())
        if ec:
            verdict(ec['holes'] == 0 and ec['record_missing'] == 0 and ec['differ'] == 0 and ec['withheld'] == 0 and ec['unsrc'] == 0 and ec['proof_mism_marked'] == 0 and ec['markers'] > 0,
                    'extras (record route + CasterExtra): %d markers, %d part draws, holes %d, record missing %d, composed banks differing from the stream %d of %d register reads, marked with an unsourced register %d, proof mismatching of marked objects %d (%d parts mismatch in unmarked objects: moving props, named)' % (
                        ec['markers'], ec['parts'], ec['holes'], ec['record_missing'], ec['differ'], ec['reads'], ec['unsrc'], ec['proof_mism_marked'], ec['proof_mism']))
        else:
            verdict(False, 'extras: no census line')
        verdict(marked.get(('rim', 0), 0) > 0 and marked.get(('map', 0), 0) > 0 and marked.get(('map', 1), 0) > 0,
                'extras: marked objects by pass %s (rim, map A + props map [ordinal 0], map B [ordinal 1] all > 0)' % dict(sorted(marked.items())))
        # lane 226 (17.11, E5): the plane mask marked by the record route (the plane tables + the plane entries' matrices EMULATED from the stream by fview; the live values are the next verify run's)
        plane_total = sum(v for (k, _), v in marked.items() if k == 'plane')
        verdict(plane_total >= 14, 'extras: plane objects marked %d (bar 14: the ring proxy and the wrestler proxy of runs 7147 / 7149; E5)' % plane_total)
        # lane 290 (17.14): the caster ledger tracks 16 calls of a plane object (kMaxCasterPlaneCalls; map / rim keep 4): the ring's plane proxy (12 calls a frame) gets markers on ordinals 4-11
        hi = sum(v for (k, o), v in marked.items() if k == 'plane' and o >= 4)
        verdict(hi >= 8 and ec is not None and ec['holes'] == 0 and ec['calls_differ'] == 0, 'extras: plane objects marked on ordinals 4-11 (the lane 290 lever): %d (bar 8: the ring proxy calls 4-11), holes %d, calls = runs differ %d' % (hi, ec and ec['holes'], ec and ec['calls_differ']))
        dd = [f for f in sorted(base['rows']) if e0['rows'].get(f) != base['rows'][f]]
        verdict(not dd, 'extras: picture of all %d frames equal to the arm without D3 (differ %d)' % (len(base['rows']), len(dd)))
        mx4 = max([o for (k, o) in marked if k == 'map' or k == 'rim'] or [0])
        verdict(mx4 <= 3, 'extras: map / rim marked ordinals stay below 4 (highest %d: the 4-call cap of those passes is unchanged)' % mx4)
        if not only or 'caster_plane_cap4' in only:
            c4 = arm('ex_caster_plane_cap4', ['--d3-mode', 'record', '--d3-ledger-in', ledger, '--d3-fault', 'caster_plane_cap4'], env=env)
            m4 = mk(c4['text'])
            p4 = sum(v for (k, _), v in m4.items() if k == 'plane')
            verdict(c4['c'] is not None and c4['c']['markers'] < ec['markers'] and c4['c']['parts'] < ec['parts'], 'plant caster_plane_cap4 detected: the lever drops: markers %s (honest %d), marker parts %s (honest %d); the marked-object census (plane %d, honest %d) is the binder, it does not see the hook ledger' % (c4['c'] and c4['c']['markers'], ec['markers'], c4['c'] and c4['c']['parts'], ec['parts'], p4, plane_total))
        n0 = arm('noextras', ['--d3-mode', 'record', '--d3-ledger-in', ledger])
        m0 = mk(n0['text'])
        print('INFO  the same arm without the table: marked %s (rim %d), proof mismatching %s' % (dict(sorted(m0.items())), m0.get(('rim', 0), 0), n0['c'] and n0['c']['proof_mism']))
        for pl in (x for x in ('rim_camera_prev_frame', 'rim_camera_from_r', 'collapse_off', 'collapse_shift', 'collapse_point_stale', 'plane_camera_prev', 'plane_fit_identity', 'plane_pos_shift', 'plane_mats_prev_frame') if not only or x in only):   # (plane_pos_prev_frame is NOT here: c7 is the same row in every frame of 7147 / 7149, so a stale c7 is undetectable on this capture; hook.plane_pos proves the per-frame value)
            x = arm('ex_' + pl, ['--d3-mode', 'record', '--d3-ledger-in', ledger, '--d3-fault', pl], env=env)
            mx = mk(x['text'])
            tx = sum(mx.values())
            pm = sum(v for (k, _), v in mx.items() if k == 'plane')
            ok = tx < total and (pl.startswith('collapse') or (pl.startswith('plane') and pm < plane_total) or mx.get(('rim', 0), 0) < marked.get(('rim', 0), 0)) and x['c'] is not None and x['c']['proof_mism_marked'] == 0
            verdict(ok, 'extras plant %s detected: marked objects %d (honest %d), rim %d (honest %d), plane %d (honest %d), proof mismatching %s' % (pl, tx, total, mx.get(('rim', 0), 0), marked.get(('rim', 0), 0), pm, plane_total, x['c'] and x['c']['proof_mism']))
        # lane 226 follow-up (S1): the live guard. A core that is not the offline measurement (the live seam) must never mark a plane call: its sources (c0-c16, c7) are made by the plane tracker, whose
        # entry snapshot is not proven live. Arm live_guard = the extras arm behaving as a live core: plane 0 marked, everything else as the honest arm. Plant live_guard_off = the guard removed: plane marked again.
        if not only or 'live_guard' in only:
            lg = arm('ex_live_guard', ['--d3-mode', 'record', '--d3-ledger-in', ledger, '--d3-fault', 'live_guard'], env=env)
            mg = mk(lg['text'])
            pg = sum(v for (k, _), v in mg.items() if k == 'plane')
            others = {k: v for k, v in mg.items() if k[0] != 'plane'}
            verdict(pg == 0 and others == {k: v for k, v in marked.items() if k[0] != 'plane'} and lg['c'] is not None and lg['c']['proof_mism_marked'] == 0,
                    'live guard: a live core marks 0 plane objects (honest %d), the map / rim marks unchanged %s' % (plane_total, dict(sorted(others.items()))))
            lo = arm('ex_live_guard_off', ['--d3-mode', 'record', '--d3-ledger-in', ledger, '--d3-fault', 'live_guard_off'], env=env)
            po = sum(v for (k, _), v in mk(lo['text']).items() if k == 'plane')
            verdict(po == plane_total and po > pg, 'plant live_guard_off detected: the guard removed marks %d plane objects (guarded arm %d)' % (po, pg))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('PASS' if not fails else 'FAIL (%d)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
