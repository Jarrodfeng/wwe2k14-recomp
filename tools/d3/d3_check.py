#!/usr/bin/env python3
"""Lane 138 (D3 skip the object) offline proof, 0 game runs: the arena's D3 machinery through the REAL binder (fview --thin arena --d3 arena) on Check B (run 7051) and run 7079.

  py -I tools\\d3\\d3_check.py [--root DIR] [--no-build] [--frames N] [--only a,b]

The guest hook is EMULATED on the capture (binder.h D3EmulateReader -> d3_binder.h EmulateHook runs the shipped DecideCall for every object call of the frame, turns the skipped runs into markers
and puts the stream's own banks in the packet as the emulated record fields). The binder then draws each marker from its sequence (learned from recorded-in-full frames), the frame's pass bank
and the packet. Two passes for each capture: pass 1 writes the OFFLINE LEDGER (the drift registers and the unstable objects a whole-capture comparison finds: SKIP_LEDGER's classification), pass 2 reads it.

Bars (each line PASS / FAIL; a missing input FAILS):
  ours      the arena's --arena-ours arm: the reference picture of every frame.
  stream    pass 2 (ledger in): the picture of ALL frames byte-equal to ours; markers > 0; holes 0; no sequence 0; sequence length differs 0; proof mismatches 0; marker banks equal to the stream's.
  online    pass 1 (no ledger): reported (frames that differ before a drift register is known), never a bar.
  fallback  --d3-table 20 (the side table fills): the hook records the objects in full (dropped by table full > 0), holes 0, the picture byte-equal.
  sourced   --d3-mode sourced (the record groups withheld): the picture differs (control), and the exact readiness: which marker parts would differ and why, with the projection per group.
  valid     12 frames with the validation layer: errors 0.
Planted faults (detected by the check named): hole_no_packet / join_by_index / offset0 -> holes > 0; skip_in_proof_frame -> markers in an object's proof frame > 0; template_first -> the picture;
  placed_identity / record_sets / no_pass (+ no proof) -> the picture and the proof compare; no_mirror / moving_static -> the proof compare (pass / world registers).

Lane 152 (D3-F, the record route; SKIP_GAME_WORK_MAP.md section 11), on J1 (run 7086, the capture that carries the light states and the arena worlds) and on Check B / run 7079 (no records):
  record    --d3-mode record (two passes, the offline ledger as above): the picture of ALL frames byte-equal to the D1 arm (`--thin arena`, the reference: D1 itself is NOT byte-equal to --arena-ours on J1, a finding
            reported as INFO, not a D3 fault); markers > 0; holes / no sequence / ordinal mismatch / unconsumed / light state missing / the proof mismatches of MARKED objects / calls = runs differ / marked-with-an-unsourced-
            register all 0; the objects not marked are listed by named reason.
  verify    --d3-verify: markers written, nothing dropped: the picture byte-equal to D1, the stream's draws compared with the sequence: mismatching 0.
  norecord  Check B / 7079 in record mode: no light states, so nothing is marked (0 markers, 0 marked), cleanly, and the picture is the ours arm's.
  plants    state_prev_object, record_offset0, negate_dirs, mirror_dirs -> light / world register mismatches in the proof compare; clock_origin (the past bug: c186 from the MotionSetup origin) -> clock mismatches;
            mark_unsourced -> a marked object with an unsourced register (and the picture differs).
The light-state plants (state_prev_object, slot0_always, pre_attach, negate_dirs, w_dropped) are lane 109's own selftest, run here as a sub-check; pre_attach is SKIP until a capture carries the second direction copy."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

CENSUS = re.compile(r'\[rdr\]\[d3\] arena markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), lost (\d+), learned (\d+), objects complete (\d+) \(marked (\d+) of (\d+)\), '
                    r'proof objects (\d+) \(parts (\d+), mismatching (\d+): pass (\d+) template (\d+) light (\d+) shadow (\d+) world (\d+) clock (\d+) drift (\d+)\), state differ (\d+), gate resets (\d+), ordinal mismatch (\d+), unconsumed (\d+), pass fallback (\d+), frames (\d+)(?:, marker unclaimed \d+)?'
                    r' \| hook: (?:calls \d+, )?markers (\d+), dropped (\d+) \(gate not proven (\d+), proof frame (\d+), table full (\d+), state overflow (\d+), unreadable (\d+)(?:, cache path 2 \d+, 3 \d+, 4 \d+)?\)(?:; paths: fresh1 (\d+), fresh2 (\d+), record3 (\d+), cached4 (\d+), unreadable (\d+))?')   # lane 237: calls + the path histogram are optional trailing groups
READY = re.compile(r'readiness \(mode (\w+)\): marker parts composed (\d+), differing from the stream (\d+) .*?register reads (\d+), withheld (\d+).*?skipped in proof frames (\d+); epoch drops (\d+); sequence length differs (\d+)')
PROJ = re.compile(r'light\+shadow sourced -> ([\d.]+) %; \+ world -> ([\d.]+) %; \+ clock -> ([\d.]+) %; \+ drift .*? -> ([\d.]+) %; parts right now ([\d.]+) %')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def read_csv(path):
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            f, h, d, b = line.strip().split(',')
            rows[int(f)] = h
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print("FAIL  input missing: Check B's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    caps = {}
    cap_b = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap_b, 'images.bin')):
        os.makedirs(os.path.join(main_co, 'out', 's101', 'thin', 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(main_co, 'out', 's101', 'thin', 'case_brock'))
    caps['B'] = (cap_b, 341)
    cap_7 = os.path.join(main_co, 'out', 'lane115', 'r7079', 'capture')
    if os.path.isfile(os.path.join(cap_7, 'images.bin')):
        caps['7079'] = (cap_7, 381)
    else:
        print('SKIP  run 7079 capture not found (' + cap_7 + '): Check B only')
    for k, (c, _) in caps.items():
        if not os.path.isdir(os.path.join(c, 'scene')):
            print('FAIL  the capture has no scene\\ records: ' + c)
            return 1
    j1 = None
    j1_dirs = [os.path.join(main_co, 'out', 's101', 'lane92', d, 'capture') for d in sorted(os.listdir(os.path.join(main_co, 'out', 's101', 'lane92')))] if os.path.isdir(os.path.join(main_co, 'out', 's101', 'lane92')) else []
    for d in j1_dirs:
        if os.path.isfile(os.path.join(d, 'images.bin')) and os.path.isdir(os.path.join(d, 'scene')) and 'case_20261112_172341' in d:
            j1 = d
    if j1:
        caps['J1'] = (j1, 343)
    else:
        print('FAIL  input missing: the J1 capture (run 7086) under out/s101/lane92/case_20261112_172341*/capture')
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    out = os.path.join(root, 'out', 'd3check')
    os.makedirs(out, exist_ok=True)
    only = set(x for x in args.only.split(',') if x)
    want = lambda n: not only or n in only
    cache = {}

    def arm(cap_key, tag, extra, env=None, frames=0):
        key = (cap_key, tag)
        if key in cache:
            return cache[key]
        cap, n = caps[cap_key]
        n = frames or args.frames or n
        d = os.path.join(out, 'arm_%s_%s' % (cap_key, tag))
        cmd = [exe, '--capture', cap, '--arena-ours', '--game', game, '--frames', str(n), '--thin-run', d] + extra
        text, rc = '', 0
        for attempt in range(8):   # (the GPU is shared: this exact message is retried, nothing else)
            rc, text = run(cmd, 1500, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print('  (GPU busy: frame ring refused, retry %d in 40 s)' % (attempt + 1), flush=True)
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s/%s failed (exit %d)' % (cap_key, tag, rc))
        m = CENSUS.search(text)
        r = READY.search(text)
        pj = PROJ.search(text)
        res = {'rows': read_csv(csv), 'text': text, 'n': n}
        if m:
            g = list(map(int, m.groups()))
            res['c'] = dict(zip(['markers', 'parts', 'holes', 'no_seq', 'lost', 'learned', 'complete', 'marked', 'pinned', 'proof_obj', 'proof_parts', 'mism', 'g_pass', 'g_template', 'g_light', 'g_shadow',
                                 'g_world', 'g_clock', 'g_drift', 'state_differ', 'gate_resets', 'ord_mism', 'unconsumed', 'pass_fallback', 'frames', 'h_markers', 'h_dropped', 'h_gate', 'h_proof', 'h_table', 'h_overflow', 'h_unreadable'], g))
        if r:
            res['r'] = dict(zip(['mode', 'parts', 'differ', 'reads', 'withheld', 'skipped_in_proof', 'epoch', 'seq_len'], [r.group(1)] + list(map(int, r.groups()[1:]))))
        if pj:
            res['proj'] = list(map(float, pj.groups()))
        cache[key] = res
        return res

    def diffs(a, b):
        fa, fb = a['rows'], b['rows']
        return [f for f in sorted(fa) if f not in fb or fa[f] != fb[f]]

    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    try:
        for ck in caps:
            if ck == 'J1':
                continue   # lane 152: J1 has its own section below (D1 is the reference there)
            ledger = os.path.join(out, 'ledger_%s.txt' % ck)
            ours = arm(ck, 'ours', [])
            n = ours['n']
            verdict(len(ours['rows']) == n and not all(h == '0' for h in ours['rows'].values()), '[%s] ours (the reference, --arena-ours): %d frames' % (ck, len(ours['rows'])))
            base = ['--thin', 'arena', '--d3', 'arena']
            p1 = arm(ck, 'pass1', base + ['--d3-ledger-out', ledger])
            c1 = p1.get('c')
            if not c1:
                verdict(False, '[%s] pass 1: no [rdr][d3] census line' % ck)
                continue
            d1 = diffs(p1, ours)
            print('INFO  [%s] online (pass 1, no ledger): %d of %d frames differ before a drift register / an unstable object is known%s; markers %d, proof mismatching parts %d (pass %d, template %d)' % (
                ck, len(d1), n, (' ' + str(d1[:10])) if d1 else '', c1['markers'], c1['mism'], c1['g_pass'], c1['g_template']))
            if not os.path.isfile(ledger):
                verdict(False, '[%s] pass 1 wrote no ledger file' % ck)
                continue
            s = arm(ck, 'stream', base + ['--d3-ledger-in', ledger])
            c, r = s.get('c'), s.get('r')
            if not c or not r:
                verdict(False, '[%s] stream: no census / readiness line' % ck)
                continue
            d = diffs(s, ours)
            verdict(not d and len(s['rows']) == n, '[%s] stream (ledger in): the picture of all %d frames byte-equal to the ours arm (%d differ%s)' % (ck, len(s['rows']), len(d), (': ' + str(d[:8])) if d else ''))
            verdict(c['markers'] > 0 and c['parts'] > 0, '[%s] markers engaged: %d markers, %d part draws synthesized from sequences, %d part-learns, %d objects complete (%d marked of %d pinned)' % (
                ck, c['markers'], c['parts'], c['learned'], c['complete'], c['marked'], c['pinned']))
            verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['lost'] == 0 and r['seq_len'] == 0 and r['epoch'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0, '[%s] holes %d, no sequence %d, sequence length differs %d, epoch drops %d, marker ordinal mismatch %d, unconsumed packets %d (all required 0); pass fallbacks %d (info)' % (
                ck, c['holes'], c['no_seq'], r['seq_len'], r['epoch'], c['ord_mism'], c['unconsumed'], c['pass_fallback']))
            verdict(c['mism'] == 0 and r['differ'] == 0 and c['state_differ'] == 0 and r['skipped_in_proof'] == 0, '[%s] proof compare: %d parts, mismatching %d; marker parts composed %d, differing from the stream %d; state differ %d; markers in a proof frame %d' % (
                ck, c['proof_parts'], c['mism'], r['parts'], r['differ'], c['state_differ'], r['skipped_in_proof']))
            print('INFO  [%s] %s' % (ck, [l for l in s['text'].splitlines() if l.startswith('[rdr][d3] arena')][-1]))

            if want('fallback') and ck == 'B':
                t = arm(ck, 'table', base + ['--d3-ledger-in', ledger], env={'D3_TABLE': '20'})
                ct = t.get('c')
                dt = diffs(t, ours)
                verdict(ct is not None and ct['holes'] == 0 and ct['h_table'] > 0 and not dt, 'fallback: a side table of 20 packets fills: the hook records %d objects in full (table full), holes %s, %d picture frames differ' % (
                    ct['h_table'] if ct else -1, ct and ct['holes'], len(dt)))

            if want('sourced') and ck == 'B':
                sr = arm(ck, 'sourced', base + ['--d3-mode', 'sourced', '--d3-ledger-in', ledger])
                rr, pj = sr.get('r'), sr.get('proj')
                ds = diffs(sr, ours)
                verdict(rr is not None and pj is not None and rr['withheld'] > 0 and len(ds) > 0.9 * n and rr['differ'] > 0.9 * rr['parts'], 'sourced (the record groups withheld): the picture moves on %d of %d frames and %.1f %% of the marker parts differ (the withheld registers ARE read)' % (
                    len(ds), n, 100.0 * rr['differ'] / max(1, rr['parts']) if rr else 0))
                if pj:
                    verdict(pj[0] <= pj[1] <= pj[2] <= pj[3] and abs(pj[3] - 100.0) < 0.01 and pj[4] == 0.0, 'readiness [%s] (marker parts right once the groups are sourced): light+shadow %.2f %%, + world %.2f %%, + clock %.2f %%, + drift %.2f %%; right now %.2f %%' % (
                        ck, pj[0], pj[1], pj[2], pj[3], pj[4]))
                for l in sr['text'].splitlines():
                    if l.startswith('[rdr][d3] readiness'):
                        print('INFO  ' + l[:900])

        if want('record'):
            RE = {'marked': re.compile(r'objects marked (\d+) of (\d+) pinned'), 'unsrc': re.compile(r'marked with an unsourced register (\d+)'), 'never': re.compile(r'never proved (\d+)'),
                  'markmis': re.compile(r'proof mismatches of marked objects (\d+) \(all (\d+)\)'), 'missing': re.compile(r'light state missing (\d+)'), 'share': re.compile(r'part draws synthesized (\d+) of (\d+) keyed arena draws \(([\d.]+) %\)'),
                  'calls': re.compile(r'calls = runs checked (\d+) differ (\d+)'), 'verify': re.compile(r'verify: markers (\d+) parts (\d+) mismatching (\d+) \(unsourced (\d+)\), length differs (\d+)'), 'reg': re.compile(r'proof mismatch registers \(all objects\): (.*)'), 'overdue': re.compile(r'proof overdue (\d+), latched (\d+)'), 'indexed': re.compile(r'records indexed (\d+)'),
                  'afterm': re.compile(r'drift after mark (\d+), unsourced after mark (\d+), frozen markers (\d+), marked-unsourced peak (\d+)')}   # lane 301

            def rec(res):
                r = {}
                t = res['text']
                for k, rx in RE.items():
                    m = rx.search(t)
                    r[k] = tuple(m.groups()) if m else None
                r['reasons'] = re.findall(r'not marked: (\d+) objects: (.*)', t)
                return r
            J = 'J1'
            ours = arm(J, 'ours', [])
            d1 = arm(J, 'd1', ['--thin', 'arena'])
            print('INFO  [J1] D1 (--thin arena) vs the ours arm: %d of %d frames differ (lane 162: J1 is a THIN-RECORDED capture, its frames carry the own lite draws of the live reader, which an ours arm cannot draw; the surplus is exactly the template-drawn draws: tools/thin/thin_j1_check.py. D3 stays held to D1: its markers synthesize draws the lite-dropped ours arm lacks, so the record arm cannot be judged against --arena-ours on J1; the lite-free captures (Check B, 7079) are judged against ours as before)' % (len(diffs(d1, ours)), d1['n']))
            ledger = os.path.join(out, 'ledger_J1rec.txt')
            base = ['--thin', 'arena', '--d3', 'arena', '--d3-mode', 'record']
            p1 = arm(J, 'rec1', base + ['--d3-ledger-out', ledger])
            if not os.path.isfile(ledger):
                verdict(False, '[J1] record pass 1 wrote no ledger')
            s = arm(J, 'rec2', base + ['--d3-ledger-in', ledger])
            c, r = s.get('c'), rec(s)
            if not c or not r['marked']:
                verdict(False, '[J1] record: no census / record-route line')
            else:
                dd = diffs(s, d1)
                verdict(not dd and len(s['rows']) == s['n'], '[J1] record (ledger in): the picture of all %d frames byte-equal to the D1 arm (%d differ%s)' % (len(s['rows']), len(dd), (': ' + str(dd[:8])) if dd else ''))
                share = r['share']
                verdict(c['markers'] > 0, '[J1] markers engaged: %d markers, %d objects marked of %s pinned, %s part draws synthesized of %s keyed arena draws (%s %%)' % (c['markers'], c['marked'], r['marked'][1], share[0], share[1], share[2]))
                verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and int(r['missing'][0]) == 0 and int(r['markmis'][0]) == 0 and int(r['calls'][1]) == 0 and int(r['unsrc'][0]) == 0 and s['r']['differ'] == 0 and s['r']['skipped_in_proof'] == 0 and r['overdue'] == ('0', '0') and int(r['indexed'][0]) <= 8,
                        '[J1] holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, light state missing %s, proof mismatches of marked objects %s (all objects %s), marked with an unsourced register %s, calls = runs differ %s of %s frames, marker parts differing from the stream %d, markers in a proof frame %d, proof overdue / latched %s (all required 0); records delivered in live order, %s indexed at the end (<= 8)' % (
                            c['holes'], c['no_seq'], c['ord_mism'], c['unconsumed'], r['missing'][0], r['markmis'][0], r['markmis'][1], r['unsrc'][0], r['calls'][1], r['calls'][0], s['r']['differ'], s['r']['skipped_in_proof'], '/'.join(r['overdue']), r['indexed'][0]))
                print('INFO  [J1] proof mismatch registers: %s; never proved: %s' % (r['reg'][0] if r['reg'] else '-', r['never'][0] if r['never'] else '?'))
                for n, why in r['reasons']:
                    print('INFO  [J1] not marked: %s objects: %s' % (n, why[:200]))
            # ---- lane 301 (SKIP_GAME_WORK_MAP.md 11.10): what live always is = NO ledger. Pass 1 (rec1) is a real bar now; the drift learned AFTER a mark (a fade: VS c185 stepping on marked objects) must neither hole nor latch.
            r1 = rec(p1)
            c1 = p1.get('c')
            if c1 and r1['marked'] and r1['afterm']:
                a1 = list(map(int, r1['afterm']))
                verdict(c1['holes'] == 0 and c1['no_seq'] == 0 and c1['ord_mism'] == 0 and c1['unconsumed'] == 0 and int(r1['missing'][0]) == 0 and int(r1['markmis'][0]) == 0 and int(r1['unsrc'][0]) == 0 and a1[1] == 0 and a1[3] == 0 and r1['overdue'] == ('0', '0'),
                        '[J1] rec1 (record route, NO ledger: what live always is): holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, record missing %s, proof mismatches of marked objects %s, marked with an unsourced register %s (peak %d), unsourced after mark %d, proof overdue / latched %s (all required 0); drift after mark %d, frozen markers %d (info)' % (
                            c1['holes'], c1['no_seq'], c1['ord_mism'], c1['unconsumed'], r1['missing'][0], r1['markmis'][0], r1['unsrc'][0], a1[3], a1[1], '/'.join(r1['overdue']), a1[0], a1[2]))
            else:
                verdict(False, '[J1] rec1: no record-route / lane 301 counters line')
            # template_step (an env edit of the capture, NOT a --d3-fault: drift detection is off under a planted fault): from the 100th frame the VS c185 (MdlAlpha) of 3 objects that are MARKED by then steps every frame, as the
            # game's fade does. no_freeze (D3_NO_FREEZE=1) is the code before lane 301: THE PAST BUG (the 7118 / 7202 signature: holes = record missing, the gate latched). Lags 2 (the emulation's) and 4 / 8 (live-sized).
            for lag in (2, 4, 8):
                e = {'D3_TEMPLATE_STEP': '100:3'}
                tf = arm(J, 'ts_fix_%d' % lag, base + ['--thin-lag', str(lag)], env=e)
                to = arm(J, 'ts_old_%d' % lag, base + ['--thin-lag', str(lag)], env=dict(e, D3_NO_FREEZE='1'))
                rf, ro = rec(tf), rec(to)
                if not (rf['afterm'] and ro['afterm'] and tf.get('c') and to.get('c')):
                    verdict(False, '[J1] template_step lag %d: no census line' % lag)
                    continue
                af, ao = list(map(int, rf['afterm'])), list(map(int, ro['afterm']))
                verdict('[d3] template_step' in tf['text'] and tf['c']['holes'] == 0 and int(rf['missing'][0]) == 0 and af[1] == 0 and af[3] == 0 and rf['overdue'][1] == '0' and af[0] > 0,
                        '[J1] template_step lag %d, with the freeze: holes %d, record missing %s, unsourced after mark %d (peak %d), latched %s (required 0 / 0 / 0 / 0 / 0); drift after mark %d (> 0: the fade was met), markers drawn from the frozen parts %d' % (
                            lag, tf['c']['holes'], rf['missing'][0], af[1], af[3], rf['overdue'][1], af[0], af[2]))
                verdict(to['c']['holes'] > 0 and int(ro['missing'][0]) == to['c']['holes'] and ro['overdue'][1] == '1' and ao[1] == to['c']['holes'] and ao[3] > 0,
                        '[J1] template_step lag %d, planted no_freeze = THE PAST BUG (7118 / 7202) re-found: holes %d = record missing %s = unsourced after mark %d, the gate latched (latched %s; the freeze arm has 0); marked-unsourced peak %d (the bar can fail: review S4)' % (
                            lag, to['c']['holes'], ro['missing'][0], ao[1], ro['overdue'][1], ao[3]))
            v = arm(J, 'verify', base + ['--d3-ledger-in', ledger, '--d3-verify'])
            rv = rec(v)
            if not rv['verify']:
                verdict(False, '[J1] verify: no record-route line')
            else:
                vv = list(map(int, rv['verify']))
                verdict(not diffs(v, d1) and vv[0] > 0 and vv[1] > 0 and vv[2] == 0 and vv[4] == 0 and v['c']['holes'] == 0,
                        '[J1] verify arm: %d markers written with nothing dropped, %d part draws compared with the stream, mismatching %d, length differs %d; the picture is D1\'s (%d frames differ)' % (vv[0], vv[1], vv[2], vv[4], len(diffs(v, d1))))
            for ck in ('B', '7079'):
                if ck not in caps:
                    continue
                o = arm(ck, 'ours', [])
                nr = arm(ck, 'norec', ['--thin', 'arena', '--d3', 'arena', '--d3-mode', 'record'])
                rn = rec(nr)
                verdict(nr['c']['markers'] == 0 and rn['marked'] and int(rn['marked'][0]) == 0 and not diffs(nr, o) and int(rn['missing'][0]) > 0,
                        '[%s] record mode on a capture WITHOUT light states: 0 markers, 0 objects marked, the picture is the ours arm\'s (%d differ), %s proof compares found no light record (counted, named)' % (ck, len(diffs(nr, o)), rn['missing'][0] if rn['missing'] else '?'))
            d1rows = d1
            def rplant(name, detect):
                p = arm(J, 'rp_' + name, base + ['--d3-ledger-in', ledger, '--d3-fault', name])
                pc, pr = p.get('c'), rec(p)
                ok, why = detect(pc, pr, p)
                verdict(ok, 'planted %s (record route, J1): %s' % (name, why))
            rplant('state_prev_object', lambda c, r, p: (c['g_light'] > 0, 'light register mismatches in the proof compare %d (the honest arm has 0)' % c['g_light']))
            rplant('record_offset0', lambda c, r, p: (c['g_light'] + c['g_world'] > 0 or int(r['missing'][0]) > 0, 'the records joined at offset 0 (stamp = the stream frame, a stamp the live-order records do not hold yet): %s proof compares found no light record, light mismatches %d, world mismatches %d, %s objects marked (the honest arm has 0 / 0 / 0, 45 marked)' % (r['missing'][0], c['g_light'], c['g_world'], r['marked'][0])))
            rplant('negate_dirs', lambda c, r, p: (c['g_light'] > 0, 'light register mismatches %d with the directions negated (the honest arm has 0)' % c['g_light']))
            rplant('mirror_dirs', lambda c, r, p: (c['g_light'] > 0, 'light register mismatches %d with the mirrored copies\' x negated (the honest arm has 0)' % c['g_light']))
            rplant('clock_origin', lambda c, r, p: (c['g_clock'] > 0, 'THE PAST BUG (c186 from the MotionSetup origin, which J1 proved false): clock register mismatches %d in the proof compare (the honest arm has 0)' % c['g_clock']))
            rplant('mark_unsourced', lambda c, r, p: (int(r['unsrc'][0]) > 0 and c['holes'] > 0, 'objects marked with an unsourced register %s: every such marker became a HOLE (%d holes, the S1 check at marker time) instead of a zero-filled draw (the honest arm has 0 / 0)' % (r['unsrc'][0], c['holes'])))
            rplant('draw_unsourced', lambda c, r, p: (int(r['unsrc'][0]) > 0 and c['holes'] == 0 and bool(diffs(p, d1rows)), 'the marker-time check switched off: %s objects marked with an unsourced register drew with the zero fill, %d picture frames differ from D1, holes %d' % (r['unsrc'][0], len(diffs(p, d1rows)), c['holes'])))
            rplant('record_late', lambda c, r, p: (c['markers'] == 0 and int(r['marked'][0]) == 0, 'THE LIVE PAIRING AS IT WAS (record i reaches the binder at frame i+1): 0 markers, %s objects marked, %s proof compares found no light record (review B1)' % (r['marked'][0], r['missing'][0])))
            rplant('proof_skew', lambda c, r, p: (int(r['overdue'][0]) > 0, 'a skewed proof clock in the hook: %s object-frames proof overdue (marks expired, no re-proof; the honest arm has 0), %d markers' % (r['overdue'][0], c['markers'])))
            hp = arm(J, 'rp_hole_latch', base + ['--d3-ledger-in', ledger, '--d3-fault', 'hole_no_packet'])
            rh = rec(hp)
            verdict(rh['overdue'] is not None and rh['overdue'][1] == '1' and hp['c']['holes'] > 0, 'planted hole_no_packet (a SYSTEMATIC hole): the gate latched D3 off after %d resets (latched %s, holes %d; the honest arm has latched 0): no skip / hole / reset loop (review B2)' % (hp['c']['gate_resets'], rh['overdue'][1] if rh['overdue'] else '?', hp['c']['holes']))

            # ---- the live sink skips frames it does not bind (30 fps mode: every other frame; a backed-up queue: bursts). The hook already wrote their packets; none of it may count as a hole or latch D3 off.
            for shape in ('every2', 'burst'):
                e = {'D3_SKIP': shape}
                d1s = arm(J, 'sk_d1_' + shape, ['--thin', 'arena'], env=e)
                d3s = arm(J, 'sk_d3_' + shape, base + ['--d3-ledger-in', ledger], env=e)
                rs = rec(d3s)
                sk = re.search(r'sink-skipped frames (\d+)', d3s['text'])
                verdict(not diffs(d3s, d1s) and rs['overdue'] is not None and rs['overdue'][1] == '0' and d3s['c']['unconsumed'] == 0 and d3s['c']['holes'] == 0 and sk and int(sk.group(1)) > 0,
                        '[J1] frames skipped by the sink (%s: %s frames): the gate did not latch (latched %s), unconsumed %d, holes %d, the picture of the bound frames equals D1\'s with the same skips (%d differ), %s objects marked' % (
                            shape, sk.group(1) if sk else '?', rs['overdue'][1] if rs['overdue'] else '?', d3s['c']['unconsumed'], d3s['c']['holes'], len(diffs(d3s, d1s)), rs['marked'][0] if rs['marked'] else '?'))
            pl = arm(J, 'sk_plant', base + ['--d3-ledger-in', ledger], env={'D3_SKIP': 'burst', 'D3_SKIP_PLANT': '1'})
            rpl = rec(pl)
            verdict(pl['c']['unconsumed'] > 0 and rpl['overdue'] is not None and rpl['overdue'][1] == '1', 'planted skipped-frame packets left to be evicted (the sink without its discard): %d unconsumed, the gate latched D3 off (latched %s; the honest arm has 0 / 0)' % (pl['c']['unconsumed'], rpl['overdue'][1] if rpl['overdue'] else '?'))

            # lane 301: the gate-reset storm arm WITHOUT the ledger (live always has none): the sink's bursts (reset frames, stale gate) must not hole, unconsume or latch
            nl = arm(J, 'sk_d3_burst_noledger', base, env={'D3_SKIP': 'burst'})
            rnl = rec(nl)
            cn = nl.get('c')
            if cn and rnl['overdue'] and rnl['afterm']:
                an = list(map(int, rnl['afterm']))
                verdict(cn['holes'] == 0 and cn['unconsumed'] == 0 and cn['no_seq'] == 0 and rnl['overdue'][1] == '0' and an[1] == 0 and int(rnl['markmis'][0]) == 0,
                        '[J1] burst skips (D3_SKIP=burst), NO ledger: holes %d, unconsumed %d, no sequence %d, unsourced after mark %d, proof mismatches of marked objects %s, latched %s (all required 0); drift after mark %d, frozen markers %d (info)' % (
                            cn['holes'], cn['unconsumed'], cn['no_seq'], an[1], rnl['markmis'][0], rnl['overdue'][1], an[0], an[2]))
            else:
                verdict(False, '[J1] burst skips without the ledger: no census line')

        if want('valid'):
            cap, _ = caps['B']
            rc, text = run([exe, '--capture', cap, '--arena-ours', '--game', game, '--thin', 'arena', '--d3', 'arena', '--thin-run', os.path.join(out, 'arm_valid'), '--frames', '12', '--validation', '1'], 900)
            verdict('validation errors 0' in text and 'validation active: yes' in text and rc == 0, 'validation: 12 D3 frames with the layer on, errors 0 (exit %d)' % rc)

        # ---- planted faults (Check B) ---------------------------------------------------------------------------------------------------------------------------------------------------
        cap_key = 'B'
        ledger = os.path.join(out, 'ledger_B.txt')
        ours = cache[(cap_key, 'ours')]
        base = ['--thin', 'arena', '--d3', 'arena', '--d3-ledger-in', ledger]

        def plant(name, detect, extra=()):
            if not want('plants') and not want(name):
                return
            p = arm(cap_key, 'p_' + name + ('_np' if '--d3-no-proof' in extra else ''), base + ['--d3-fault', name] + list(extra))
            ok, why = detect(p)
            verdict(ok, 'planted %s%s: %s' % (name, ' (no proof)' if '--d3-no-proof' in extra else '', why))

        def holes(p):
            c = p.get('c')
            return bool(c and c['holes'] > 0), 'holes %s (bar > 0; the honest arm has 0), %d picture frames differ' % (c and c['holes'], len(diffs(p, ours)))

        def inproof(p):
            r = p.get('r')
            return bool(r and r['skipped_in_proof'] > 0), 'markers in an object\'s proof frame %s (bar > 0; the honest arm has 0)' % (r and r['skipped_in_proof'])

        def ordinal(p):
            c = p.get('c')
            return bool(c and c['ord_mism'] > 0), 'marker ordinal mismatches %s (bar > 0; the honest arm has 0), holes %s' % (c and c['ord_mism'], c and c['holes'])

        def pic(p):
            d = diffs(p, ours)
            return bool(d), '%d picture frames differ from the ours arm (bar > 0)' % len(d)

        def group(g):
            def f(p):
                c = p.get('c')
                return bool(c and c[g] > 0), 'proof compare: %s register mismatches %s (bar > 0; the honest arm has 0)' % (g[2:], c and c[g])
            return f

        def both(*fs):
            def f(p):
                rs = [x(p) for x in fs]
                return all(r[0] for r in rs), '; '.join(r[1] for r in rs)
            return f

        plant('hole_no_packet', holes)
        plant('join_by_index', holes)
        plant('offset0', holes)
        plant('skip_in_proof_frame', inproof)
        plant('ordinal_off', ordinal)
        plant('template_first', pic)
        plant('no_mirror', group('g_pass'))
        plant('no_mirror', pic, ('--d3-no-proof',))
        plant('moving_static', group('g_world'))
        plant('placed_identity', both(group('g_world'), pic), ('--d3-no-proof',))
        plant('record_sets', both(group('g_light'), pic), ('--d3-no-proof',))
        plant('no_pass', both(group('g_pass'), pic), ('--d3-no-proof',))
        if want('plants'):
            print('INFO  moving_static: no picture frame moves on Check B although the proof compare sees the stale world (the moving pieces are rotation-symmetric or off screen there: unconfirmed, the compare is the detector)')
            p = arm(cap_key, 'p_stale_template_np', base + ['--d3-fault', 'stale_template', '--d3-no-proof'])
            print('INFO  stale_template (templates never refreshed): %d picture frames differ, proof mismatching parts %s: the drifting registers are already in the ledger on Check B, so the plant is not visible here' % (len(diffs(p, ours)), p['c']['mism']))

        if want('lightstates'):
            rc, text = run([sys.executable, '-I', os.path.join(root, 'tools', 'light', 'light_states_selftest.py'), '--no-build'], 900)
            lines = [l for l in text.splitlines() if 'plant' in l]
            fail_lines = [l for l in text.splitlines() if l.startswith('FAIL')]
            verdict(rc == 0 and not fail_lines and any('plant.slot0_always' in l and l.startswith('PASS') for l in lines),
                    'light-state plants (lane 109\'s selftest: state_prev_object, slot0_always, negate_dirs, w_dropped each fail their named check): exit %d, %d plant lines, pre_attach %s' % (
                        rc, len(lines), 'SKIP (needs a capture with the second direction copy: Check J1)' if any('pre_attach' in l and l.startswith('SKIP') for l in lines) else '?'))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('d3_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
