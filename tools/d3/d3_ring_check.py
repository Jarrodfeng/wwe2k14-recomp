#!/usr/bin/env python3
"""Lane 229 (Ring D3 R3, INERT; docs/renderer/SKIP_GAME_WORK_MAP.md 16.8): the ring class's D3 chain through the REAL binder on the kept captures that carry ring draws, 0 game runs.

  py -I tools\\d3\\d3_ring_check.py [--root DIR] [--no-build] [--frames N] [--only a,b] [--plants a,b]

fview --ring-ours --thin ring --d3 ring (binder.h + binder_ring_d3.inc, d3_binder.h route Ring): the guest hook is EMULATED on the capture (D3REmulateReader -> EmulateHook runs the shipped DecideCall with the ring
class's own switch for every object call of the frame and turns the skipped runs into ring markers, kRingMarkerCall), the binder draws each marker from its learned sequence, the frame's pass bank and the packet
through BindDraw. Two passes per capture: pass 1 writes the offline ledger (the drift registers: c185 MdlAlpha, the canvas' c230-c237), pass 2 reads it. The reference picture is the D1 ring arm (`--thin ring`).

Bars (each line PASS / FAIL; a missing REQUIRED input FAILS, an optional capture is SKIPped by name):
  d1       the D1 ring arm (the reference): every frame drawn.
  stream   pass 2 (ledger in): the picture of ALL frames byte-equal to the D1 arm; markers > 0; part draws > 0; holes 0; no sequence 0; ordinal mismatch 0; unconsumed 0; sequence length differs 0; epoch drops 0;
           proof mismatches 0; marker parts composed differing from the stream 0; markers in a proof frame 0; calls = runs differ 0; marked with an unsourced register 0; latched 0.
           (pass 1, no ledger, is reported: the frames that differ before a drift register is known.)
  record   (captures with light states) --d3-mode record: no capture holds the ring's banks together with a ring-rows record (16.4 item 3), so NOTHING may be marked and nothing may break: 0 markers, holes 0, marked
           with an unsourced register 0, ring rows missing > 0 (named), the picture is the D1 arm's.
  Check B only: fallback (--d3 table 20: dropped by table full, holes 0, picture D1), both (--d3 arena,ring beside the arena's chain: picture byte-equal, both chains' holes / proof 0), skipped frames (the sink
           skips frames: no latch, no hole, the picture equals D1's with the same skips; the plant leaves the packets: unconsumed, latched), validation errors 0, and the plants:
           hole_no_packet / join_by_index / offset0 -> holes > 0; ordinal_off -> ordinal mismatch > 0; skip_in_proof_frame -> markers in a proof frame > 0; moving_static / placed_identity (world), record_sets (light),
           no_pass (pass) -> the proof compare's mismatch in the NAMED group and fewer markers (no-proof arms: the picture differs); lod_flip_ordinal -> more markers than the honest arm (the guard held them back).
  INFO     template_first and no_mirror are not detected on the ring (no multi-part ring run with differing templates, no mirrored copies): reported, not bars.
The record route's rows (the slot -> node map, the don't-care slots, the join, the other plants) are proved by tools\\d3\\d3_ring_selftest.py on a synthetic ring-rows record."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

LINE = re.compile(r'\[rdr\]\[d3\]\[ring\] mode (\w+): markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), learned (\d+), objects complete \(marked (\d+) of (\d+)\), proof objects (\d+) '
                  r'\(parts (\d+), mismatching (\d+), of marked objects (\d+)\), ordinal mismatch (\d+), unconsumed (\d+), gate resets (\d+), calls = runs checked (\d+) differ (\d+), record missing (\d+), hook: '
                  r'markers (\d+) dropped (\d+) \(gate not proven (\d+), proof frame (\d+), table full (\d+), state overflow (\d+), unreadable (\d+)\), readiness: marker parts composed (\d+), differing from the stream (\d+), register reads (\d+), withheld (\d+), '
                  r'epoch drops (\d+), sequence length differs (\d+), never proved (\d+), marked with an unsourced register (\d+), latched (\d+), sink-skipped frames (\d+), skipped in proof frames (\d+)')
KEYS = ['mode', 'markers', 'parts', 'holes', 'no_seq', 'learned', 'marked', 'complete_of', 'proof_obj', 'proof_parts', 'proof_mism', 'proof_mism_marked', 'ord_mism', 'unconsumed', 'gate_resets', 'calls_checked',
        'calls_differ', 'record_missing', 'h_markers', 'h_dropped', 'h_gate', 'h_proof', 'h_table', 'h_overflow', 'h_unreadable', 'composed', 'differ', 'reads', 'withheld', 'epoch', 'seq_len', 'never', 'unsrc', 'latched',
        'sink_skipped', 'skipped_in_proof']
FILL = re.compile(r'ordinal fill \(lane 289\): mode (\w+), generations published (\d+), binder fills (\d+) \(missing (\d+)\), oracle check frames (\d+) differ (\d+)')   # lane 289
GROUPS = re.compile(r'proof mismatches by group: pass (\d+) template (\d+) light (\d+) shadow (\d+) world (\d+) clock (\d+) drift (\d+), state differ (\d+)')
ROWS = re.compile(r'ring rows: records indexed (\d+) \(light refs (\d+), ring entries (\d+)\), ring rows missing (\d+), slots learned (\d+) \(unmatched (\d+), ambiguous (\d+)\), call-count guard trips (\d+)')
ARENA_CALLS = re.compile(r'\[rdr\]\[d3\] record route .*?calls = runs checked (\d+) differ (\d+)')
ARENA = re.compile(r'\[rdr\]\[d3\] arena markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), lost (\d+), learned (\d+), objects complete (\d+) \(marked (\d+) of (\d+)\), proof objects (\d+) \(parts (\d+), mismatching (\d+)')


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
    ap.add_argument('--plants', default='')
    ap.add_argument('--j2', default='', help='the capture folder of the ONE live verify run (J2: banks + light states + RING ROWS, lane 194 / 229): runs the record-route arms on it (never run on a real capture yet)')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    # the kept captures (the same inputs as d3_check.py / d3_caster_check.py); B, J1 and 7079 are required
    cands = [('B', os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture'), True, 'Check B (run 7051)'),
             ('7079', os.path.join(main_co, 'out', 'lane115', 'r7079', 'capture'), True, 'run 7079'),
             ('J1', os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_172341_872_0_pc_run_exit', 'capture'), True, 'J1 (run 7086, light states)'),
             ('7149', os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture'), False, 'the caster verify capture of 2026-11-13 09:35'),
             ('7147', os.path.join(main_co, 'out', 'd3caster', 'z7147', 'capture'), False, 'run 7147 (caster records)')]
    caps = {}
    for key, path, required, what in ([] if args.j2 else cands):
        if os.path.isfile(os.path.join(path, 'images.bin')) and os.path.isdir(os.path.join(path, 'scene')):
            caps[key] = path
        elif required:
            print('FAIL  input missing: %s: %s' % (what, path))
            return 1
        else:
            print('SKIP  %s not found (%s)' % (what, path))
    if args.j2:
        if not (os.path.isfile(os.path.join(args.j2, 'images.bin')) and os.path.isdir(os.path.join(args.j2, 'scene'))):
            print('FAIL  input missing: the J2 capture ' + args.j2)
            return 1
        caps = {'J2': os.path.abspath(args.j2)}   # J2 alone: the record-route arms below
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    exe = _fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'd3ring')
    os.makedirs(out, exist_ok=True)
    only = set(x for x in args.only.split(',') if x)
    want = lambda n: not only or n in only  # noqa: E731
    plants_only = set(x for x in args.plants.split(',') if x)
    cache = {}
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    def nframes(ck):
        return args.frames or len(os.listdir(os.path.join(caps[ck], 'frames')))

    def arm(ck, tag, extra, env=None, base=('--ring-ours', '--thin', 'ring'), frames=0):
        key = (ck, tag)
        if key in cache:
            return cache[key]
        n = frames or nframes(ck)
        d = os.path.join(out, 'arm_%s_%s' % (ck, tag))
        cmd = [exe, '--capture', caps[ck], '--game', game, '--frames', str(n), '--thin-run', d] + list(base) + extra
        rc, text = 1, ''
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
            raise RuntimeError('arm %s/%s failed (exit %d)' % (ck, tag, rc))
        m = LINE.search(text)
        res = {'rc': rc, 'rows': read_csv(csv), 'text': text, 'n': n, 'c': dict(zip(KEYS, [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None}
        g = GROUPS.search(text)
        res['g'] = dict(zip(['pass', 'template', 'light', 'shadow', 'world', 'clock', 'drift', 'state_differ'], map(int, g.groups()))) if g else None
        r = ROWS.search(text)
        res['rows_line'] = dict(zip(['records', 'light_refs', 'ring_entries', 'rows_missing', 'slots_learned', 'unmatched', 'ambiguous', 'guard_trips'], map(int, r.groups()))) if r else None
        a = ARENA.search(text)
        res['arena'] = dict(zip(['markers', 'parts', 'holes', 'no_seq', 'lost', 'learned', 'complete', 'marked', 'pinned', 'proof_obj', 'proof_parts', 'proof_mism'], map(int, a.groups()))) if a else None
        ac = ARENA_CALLS.search(text)
        res['arena_calls'] = (int(ac.group(1)), int(ac.group(2))) if ac else None
        cache[key] = res
        return res

    def fillinfo(res):   # lane 289: the 'ordinal fill' line (binder fills / missing, oracle frames / differ)
        m = FILL.search(res['text'])
        return dict(zip(['mode', 'gens', 'fills', 'missing', 'checked', 'differ'], [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None

    def diffs(a, b):
        fa, fb = a['rows'], b['rows']
        return [f for f in sorted(fa) if f not in fb or fa[f] != fb[f]]

    try:
        if args.j2:
            # THE ONE LIVE VERIFY RUN'S ZIP (SKIP_GAME_WORK_MAP.md 16.8): the first capture that holds the ring's banks together with a ring-rows record. The record route must now MARK ring objects, with the rows
            # from the record: markers > 0, ring rows missing ~0, slots learned, proof mismatches of marked objects 0, the picture equal to D1's; and the rows plants must each move the census.
            ck = 'J2'
            n = nframes(ck)
            d1 = arm(ck, 'd1', [])
            rl = os.path.join(out, 'ledger_J2_rec.txt')
            arm(ck, 'rec1', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-out', rl])
            rr = arm(ck, 'rec2', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl])
            c, rw = rr['c'], rr['rows_line']
            dr = diffs(rr, d1)
            verdict(bool(c) and bool(rw) and c['markers'] > 0 and c['marked'] > 0 and c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and c['proof_mism_marked'] == 0 and c['unsrc'] == 0 and c['latched'] == 0,
                    '[J2] record route: %s markers, %s objects marked of %s, holes %s, no sequence %s, proof mismatches of marked objects %s, marked with an unsourced register %s' % (
                        c and c['markers'], c and c['marked'], c and c['complete_of'], c and c['holes'], c and c['no_seq'], c and c['proof_mism_marked'], c and c['unsrc']))
            verdict(not dr, '[J2] the picture of all frames byte-equal to D1 (%d differ%s)' % (len(dr), (': ' + str(dr[:12])) if dr else ''))
            # lane 258 (RL1): no palette reason on any not-marked object (a lite draw keeps its part's palette size), slots matched, and calls = runs against the REAL hook's count (the ring-rows entries)
            nopal = [l for l in rr['text'].splitlines() if l.startswith('[rdr][d3][ring] not marked') and ('no wrestler palette' in l or 'NoPalette' in l or 'bone count unknown' in l)]
            verdict(not nopal and bool(rw) and rw['unmatched'] * 20 <= max(1, rw['slots_learned'] + rw['unmatched']), '[J2] no palette-size reason among the not-marked objects (%d lines), slots unmatched %s of %s' % (len(nopal), rw and rw['unmatched'], rw and rw['slots_learned'] + rw['unmatched']))
            rh = arm(ck, 'rec2_hook', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl, '--d3-hook-from-record'])
            ch = rh['c']
            verdict(bool(ch) and ch['calls_checked'] >= n - 4 and ch['calls_differ'] == 0, '[J2] calls = runs against the real hook count (one keyed call per ring-rows entry): checked %s differ %s (bar: checked >= %d, differ 0)' % (ch and ch['calls_checked'], ch and ch['calls_differ'], n - 4))
            pn = arm(ck, 'rec2_noreserve', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl], env={'D3_NO_RESERVE': '1'})   # THE PAST BUG of lane 258: the cut draws not counted in the thin ordinals: the instances after a cut take the wrong template
            dn = diffs(pn, d1)
            verdict(len(dn) > 0 and pn['c'] is not None and pn['c']['markers'] > 0, '[J2] planted ordinal_shift (D3_NO_RESERVE: the cut instances leave the thin ordinal count) detected: %d picture frames differ from D1 (bar > 0), markers %s' % (len(dn), pn['c'] and pn['c']['markers']))
            # lane 289 (the ring SKIP arm's live thin ordinals): the binder fills every marker's part triples from the published mark set (the live form is the default), the oracle (lane 258's list of the cut draws) must give the same keys
            pf = arm(ck, 'rec2_fillchk', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl], env={'D3_FILL_CHECK': '1'})
            fi = fillinfo(pf)
            verdict(bool(fi) and fi['mode'] == 'live' and fi['fills'] > 0 and fi['missing'] == 0 and fi['checked'] > 0 and fi['differ'] == 0 and not diffs(pf, d1),
                    '[J2] ordinal fill (live form) = the oracle: binder fills %s (missing %s), oracle frames %s differ %s, generations published %s, picture byte-equal to D1 (bar: fills > 0, missing 0, frames > 0, differ 0)' % (
                        fi and fi['fills'], fi and fi['missing'], fi and fi['checked'], fi and fi['differ'], fi and fi['gens']))
            pg = arm(ck, 'rec2_fillgen', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl, '--d3-fault', 'ring_fill_generation_off'])   # THE PLANT: the marker names a generation no set has (the ordinal fill finds nothing)
            fg = fillinfo(pg)
            dg = diffs(pg, d1)
            verdict(bool(fg) and fg['missing'] > 0 and bool(pg['c']) and pg['c']['gate_resets'] > 0,
                    '[J2] planted ring_fill_generation_off detected: fill missing %s, gate resets %s, picture frames differing from D1 %d (bars: missing > 0 and gate resets > 0; the picture may differ)' % (fg and fg['missing'], pg['c'] and pg['c']['gate_resets'], len(dg)))
            pm = arm(ck, 'rec2_hook_merge', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl, '--d3-hook-from-record', '--d3-fault', 'ring_merge_instances'])
            cm = pm['c']
            verdict(bool(cm) and cm['calls_checked'] > 0 and cm['calls_differ'] * 10 >= cm['calls_checked'] * 9, '[J2] planted ring_merge_instances (the old run scan) detected by calls = runs: checked %s differ %s (bar: about all)' % (cm and cm['calls_checked'], cm and cm['calls_differ']))
            verdict(bool(rw) and rw['ring_entries'] > 0 and rw['rows_missing'] * 20 <= max(1, rw['ring_entries']) and rw['slots_learned'] > 0, '[J2] ring rows: %s entries indexed, ring rows missing %s, slots learned %s (unmatched %s, ambiguous %s)' % (
                rw and rw['ring_entries'], rw and rw['rows_missing'], rw and rw['slots_learned'], rw and rw['unmatched'], rw and rw['ambiguous']))
            for l in rr['text'].splitlines():
                if l.startswith('[rdr][d3][ring] not marked'):
                    print('INFO  [J2] ' + l[len('[rdr][d3][ring] '):][:260])
            hon = c['marked'] if c else 0
            for nm in ('stale_slots_counted', 'dedupe_model_r5', 'record_offset0', 'late_record', 'canvas_marked', 'alpha_zero', 'ring_merge_instances'):
                p = arm(ck, 'rp_' + nm, ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl, '--d3-fault', nm])
                x = p['c']
                verdict(bool(x) and (x['marked'] < hon or x['proof_mism'] > 0 or x['record_missing'] > 0 or x['unsrc'] > 0), '[J2] planted %s detected: marked %s (honest %d), proof mismatching %s, record missing %s, unsourced %s' % (
                    nm, x and x['marked'], hon, x and x['proof_mism'], x and x['record_missing'], x and x['unsrc']))
            print('d3_ring_check (J2): %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
            return 1 if fails else 0
        for ck in caps:
            if only and ck not in only and 'stream' not in only and 'all' not in only:
                continue
            n = nframes(ck)
            d1 = arm(ck, 'd1', [])
            verdict(len(d1['rows']) == n and not all(h == '0' for h in d1['rows'].values()), '[%s] D1 ring arm (the reference, --ring-ours --thin ring): %d frames' % (ck, len(d1['rows'])))
            ledger = os.path.join(out, 'ledger_%s.txt' % ck)
            p1 = arm(ck, 'pass1', ['--d3', 'ring', '--d3-ledger-out', ledger])
            c1 = p1['c']
            if not c1:
                verdict(False, '[%s] pass 1: no [rdr][d3][ring] census line' % ck)
                continue
            dd1 = diffs(p1, d1)
            print('INFO  [%s] online (pass 1, no ledger): %d of %d frames differ before a drift register is known%s; markers %d, proof mismatching parts %d' % (ck, len(dd1), n, (' ' + str(dd1[:10])) if dd1 else '', c1['markers'], c1['proof_mism']))
            if not os.path.isfile(ledger):
                verdict(False, '[%s] pass 1 wrote no ledger file' % ck)
                continue
            s = arm(ck, 'stream', ['--d3', 'ring', '--d3-ledger-in', ledger])
            c = s['c']
            if not c:
                verdict(False, '[%s] stream: no census line' % ck)
                continue
            dd = diffs(s, d1)
            verdict(not dd and len(s['rows']) == n, '[%s] stream (ledger in): the picture of all %d frames byte-equal to the D1 ring arm (%d differ%s)' % (ck, len(s['rows']), len(dd), (': ' + str(dd[:8])) if dd else ''))
            verdict(c['markers'] > 0 and c['parts'] > 0 and c['marked'] > 0, '[%s] markers engaged: %d markers, %d part draws synthesized from sequences, %d part-learns, %d objects marked of %d' % (ck, c['markers'], c['parts'], c['learned'], c['marked'], c['complete_of']))
            verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and c['seq_len'] == 0 and c['epoch'] == 0 and c['latched'] == 0,
                    '[%s] holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, sequence length differs %d, epoch drops %d, latched %d' % (ck, c['holes'], c['no_seq'], c['ord_mism'], c['unconsumed'], c['seq_len'], c['epoch'], c['latched']))
            verdict(c['proof_mism'] == 0 and c['proof_parts'] > 0 and c['differ'] == 0 and c['skipped_in_proof'] == 0 and c['unsrc'] == 0 and c['withheld'] == 0,
                    '[%s] proof compare: %d parts, mismatching %d; marker parts composed %d, differing from the stream %d (%d register reads, withheld %d); markers in a proof frame %d; marked with an unsourced register %d' % (
                        ck, c['proof_parts'], c['proof_mism'], c['composed'], c['differ'], c['reads'], c['withheld'], c['skipped_in_proof'], c['unsrc']))
            verdict(c['calls_differ'] == 0 and c['calls_checked'] > 0, '[%s] calls = runs checked %d differ %d' % (ck, c['calls_checked'], c['calls_differ']))
            pf = arm(ck, 'fillchk', ['--d3', 'ring', '--d3-ledger-in', ledger], env={'D3_FILL_CHECK': '1'})   # lane 289: the live-form ordinal fill against the oracle
            fi = fillinfo(pf)
            verdict(bool(fi) and fi['fills'] > 0 and fi['missing'] == 0 and fi['differ'] == 0 and not diffs(pf, d1),
                    '[%s] ordinal fill (live form): binder fills %s (missing %s), oracle frames %s differ %s, generations published %s, picture equals D1 (bar: fills > 0, missing 0, differ 0)' % (ck, fi and fi['fills'], fi and fi['missing'], fi and fi['checked'], fi and fi['differ'], fi and fi['gens']))
            print('INFO  [%s] never proved %d, hook dropped %d (gate not proven %d, proof frame %d, table full %d), call-count guard trips %s' % (ck, c['never'], c['h_dropped'], c['h_gate'], c['h_proof'], c['h_table'], s['rows_line'] and s['rows_line']['guard_trips']))
            for l in s['text'].splitlines():
                if l.startswith('[rdr][d3][ring] not marked'):
                    print('INFO  [%s] %s' % (ck, l[len('[rdr][d3][ring] '):][:200]))
            # record mode: no ring-rows record exists in any kept capture: nothing may be marked, nothing may break, the reason is named
            if want('record') and ck in ('J1', '7147'):
                rl = os.path.join(out, 'ledger_%s_rec.txt' % ck)
                arm(ck, 'rec1', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-out', rl])
                rr = arm(ck, 'rec2', ['--d3', 'ring', '--d3-mode', 'record', '--d3-ledger-in', rl])
                rc_ = rr['c']
                dr = diffs(rr, d1)
                ok = bool(rc_) and rc_['markers'] == 0 and rc_['holes'] == 0 and rc_['unsrc'] == 0 and rc_['latched'] == 0 and rc_['proof_mism_marked'] == 0 and rr['rows_line'] and rr['rows_line']['rows_missing'] > 0 and not dr
                verdict(ok, '[%s] record route (no ring-rows record in the capture): 0 markers (%s), holes %s, marked with an unsourced register %s, ring rows missing %s (named), the picture equals D1\'s (%d frames differ)' % (
                    ck, rc_ and rc_['markers'], rc_ and rc_['holes'], rc_ and rc_['unsrc'], rr['rows_line'] and rr['rows_line']['rows_missing'], len(dr)))
                for l in rr['text'].splitlines():
                    if l.startswith('[rdr][d3][ring] not marked'):
                        print('INFO  [%s] record: %s' % (ck, l[len('[rdr][d3][ring] '):][:230]))

        if 'B' in caps and want('extras'):
            ck = 'B'
            n = nframes(ck)
            d1 = cache[(ck, 'd1')] if (ck, 'd1') in cache else arm(ck, 'd1', [])
            ledger = os.path.join(out, 'ledger_B.txt')
            if not os.path.isfile(ledger):
                arm(ck, 'pass1', ['--d3', 'ring', '--d3-ledger-out', ledger])
            s = arm(ck, 'stream', ['--d3', 'ring', '--d3-ledger-in', ledger])
            c = s['c']
            base = ['--d3', 'ring', '--d3-ledger-in', ledger]

            # fallback: a side table of 3 packets fills (the ring marks about 6 objects a frame): the hook records the objects in full
            t = arm(ck, 'table', base, env={'D3_TABLE': '3'})
            ct = t['c']
            dt = diffs(t, d1)
            verdict(ct is not None and ct['holes'] == 0 and ct['h_table'] > 0 and not dt, '[B] fallback: a side table of 3 packets fills (the ring marks about 6 objects a frame): the hook records %d objects in full (table full), holes %s, %d picture frames differ' % (
                ct['h_table'] if ct else -1, ct and ct['holes'], len(dt)))

            # both chains: the arena's and the ring's beside each other
            ref_both = arm(ck, 'both_ref', ['--thin', 'arena,ring'], base=('--arena-ours', '--ring-ours'))
            lb = os.path.join(out, 'ledger_both.txt')
            arm(ck, 'both1', ['--thin', 'arena,ring', '--d3', 'arena,ring', '--d3-ledger-out', lb], base=('--arena-ours', '--ring-ours'))
            b2 = arm(ck, 'both2', ['--thin', 'arena,ring', '--d3', 'arena,ring', '--d3-ledger-in', lb], base=('--arena-ours', '--ring-ours'))
            db = diffs(b2, ref_both)
            bc, ba = b2['c'], b2['arena']
            verdict(bc is not None and ba is not None and not db and bc['markers'] > 0 and ba['markers'] > 0 and bc['holes'] == 0 and ba['holes'] == 0 and bc['proof_mism'] == 0 and ba['proof_mism'] == 0 and bc['calls_differ'] == 0 and bc['unconsumed'] == 0,
                    '[B] both chains (--d3 arena,ring): the picture of all frames byte-equal to the arena+ring D1 arm (%d differ); ring markers %s holes %s proof mismatching %s calls differ %s; arena markers %s holes %s proof mismatching %s calls differ %s' % (
                        len(db), bc and bc['markers'], bc and bc['holes'], bc and bc['proof_mism'], bc and bc['calls_differ'], ba and ba['markers'], ba and ba['holes'], ba and ba['proof_mism'], b2['arena_calls'] and b2['arena_calls'][1]))

            print('INFO  [B] both chains: the ARENA chain\'s calls = runs differ %s in this offline arm (lane 258: the ring chain now cuts one marker per instance while the arena\'s emulated hook still counts a ring model\'s instances as one run; live the arena hook skips ring models via the class table, so this is an emulation artefact, not a bar)' % (b2['arena_calls'] and b2['arena_calls'][1]))
            # frames the sink skips: no hole, no latch; the plant leaves the packets
            for shape in ('every2', 'burst'):
                e = {'D3_SKIP': shape}
                d1s = arm(ck, 'sk_d1_' + shape, [], env=e)
                d3s = arm(ck, 'sk_d3_' + shape, base, env=e)
                cs = d3s['c']
                verdict(cs is not None and not diffs(d3s, d1s) and cs['latched'] == 0 and cs['unconsumed'] == 0 and cs['holes'] == 0 and cs['sink_skipped'] > 0,
                        '[B] frames skipped by the sink (%s: %s frames): latched %s, unconsumed %s, holes %s, the picture of the bound frames equals D1\'s with the same skips (%d differ)' % (
                            shape, cs and cs['sink_skipped'], cs and cs['latched'], cs and cs['unconsumed'], cs and cs['holes'], len(diffs(d3s, d1s))))
            pl = arm(ck, 'sk_plant', base, env={'D3_SKIP': 'burst', 'D3_SKIP_PLANT': '1'})
            verdict(pl['c'] is not None and pl['c']['unconsumed'] > 0 and pl['c']['latched'] == 1, '[B] planted skipped-frame packets left to be evicted: %s unconsumed, latched %s (the honest arm has 0 / 0)' % (pl['c'] and pl['c']['unconsumed'], pl['c'] and pl['c']['latched']))

            # validation layer
            rc, text = run([exe, '--capture', caps[ck], '--ring-ours', '--game', game, '--thin', 'ring', '--d3', 'ring', '--thin-run', os.path.join(out, 'arm_valid'), '--frames', '12', '--validation', '1'], 900)
            verdict('validation errors 0' in text and 'validation active: yes' in text and rc == 0, '[B] validation: 12 D3 frames with the layer on, errors 0 (exit %d)' % rc)

            # plants
            def plant(name, detect, extra=(), tag=None):
                if plants_only and name not in plants_only:
                    return
                p = arm(ck, tag or ('p_' + name + ('_np' if '--d3-no-proof' in extra else '')), base + ['--d3-fault', name] + list(extra))
                ok, why = detect(p)
                verdict(ok, 'planted %s%s: %s' % (name, ' (no proof)' if '--d3-no-proof' in extra else '', why))

            hon = c['markers']

            def holes(p):
                x = p['c']
                return bool(x and x['holes'] > 0), 'holes %s (bar > 0; the honest arm has 0), %d picture frames differ' % (x and x['holes'], len(diffs(p, d1)))

            def group(g):
                def f(p):
                    x, gg = p['c'], p['g']
                    return bool(x and gg and gg[g] > 0 and x['markers'] < hon), 'proof compare: %s register mismatches %s (bar > 0; the honest arm has 0), markers %s (honest %d)' % (g, gg and gg[g], x and x['markers'], hon)
                return f

            def pic(p):
                d = diffs(p, d1)
                return bool(d), '%d picture frames differ from the D1 arm (bar > 0)' % len(d)

            def both(*fs):
                def f(p):
                    rs = [x(p) for x in fs]
                    return all(r[0] for r in rs), '; '.join(r[1] for r in rs)
                return f
            plant('hole_no_packet', holes)
            plant('join_by_index', holes)
            plant('offset0', holes)
            plant('ordinal_off', lambda p: (bool(p['c'] and p['c']['ord_mism'] > 0), 'marker ordinal mismatches %s (bar > 0), holes %s' % (p['c'] and p['c']['ord_mism'], p['c'] and p['c']['holes'])))
            plant('skip_in_proof_frame', lambda p: (bool(p['c'] and p['c']['skipped_in_proof'] > 0), 'markers in an object\'s proof frame %s (bar > 0; the honest arm has 0)' % (p['c'] and p['c']['skipped_in_proof'])))
            plant('moving_static', group('world'))
            plant('placed_identity', group('world'))
            plant('record_sets', group('light'))
            plant('no_pass', group('pass'))
            for nm in ('moving_static', 'placed_identity', 'record_sets', 'no_pass'):
                plant(nm, pic, ('--d3-no-proof',))
            plant('lod_flip_ordinal', lambda p: (bool(p['c'] and p['c']['markers'] > hon and s['rows_line'] and s['rows_line']['guard_trips'] > 0),
                                                 'the call-count guard off: markers %s (honest %d: the guard held %s back at the LOD flips / quadrant changes), the guard tripped %s times in the honest arm; picture frames differing from D1: %d' % (
                                                     p['c'] and p['c']['markers'], hon, p['c'] and p['c']['markers'] - hon, s['rows_line'] and s['rows_line']['guard_trips'], len(diffs(p, d1)))))
            if want('plants') or not plants_only:
                for nm in ('template_first', 'no_mirror'):
                    p = arm(ck, 'p_' + nm + '_np', base + ['--d3-fault', nm, '--d3-no-proof'])
                    print('INFO  %s (no proof): %d picture frames differ, proof mismatching %s: not detected on the ring (no multi-part ring run with differing templates / no mirrored copies; unconfirmed that this holds for other arenas)' % (
                        nm, len(diffs(p, d1)), p['c'] and p['c']['proof_mism']))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('d3_ring_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
