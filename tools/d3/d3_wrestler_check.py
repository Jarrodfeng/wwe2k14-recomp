#!/usr/bin/env python3
"""Lane 227 (Wrestlers D3 H2 / H4, the wrestlers' colour pass skipped whole; docs/renderer/SKIP_GAME_WORK_MAP.md 18.7): the wrestler class's D3 chain through the REAL binder on the kept captures, 0 game runs.

  py -I tools\\d3\\d3_wrestler_check.py [--root DIR] [--no-build] [--frames N] [--caps B,7079,wm30,J1,7147,7149] [--plants a,b] [--no-plants]

fview --wrestlers-ours --thin wrestlers --d3 wrestlers (binder.h D3W* functions, d3_binder.h route Wrestlers): the guest hook is EMULATED on the capture (D3WEmulateReader -> EmulateHook runs the shipped DecideCall with
the wrestler class's own switch for every colour call of the frame and turns the skipped runs into wrestler markers), the binder draws each marker from its learned sequence (the D1 wrestler store's templates) and the
packet through BindDraw. Two passes: pass 1 writes the offline ledger (drift registers), pass 2 reads it. The reference arm is the D1 wrestler arm (--thin wrestlers without --d3): the picture of every frame must be byte-equal.

Bars per capture (each line PASS / FAIL; a missing input FAILS):
  stream    pass 2 (ledger in): the picture of ALL frames byte-equal to the D1 arm; markers > 0; part draws > 0; holes 0; no sequence 0; ordinal mismatch 0; unconsumed 0; sequence length differs 0; epoch drops 0;
            proof mismatches 0; skipped in a proof frame 0; never proved 0; marked with an unsourced register 0; calls = runs differ 0; marker banks equal to the stream's (differing 0, withheld 0); validation errors 0;
            the wrestler core still joins (D1's `objects joined` equals this arm's; object unpins 0).
  record    (captures with light refs: J1, 7147, 7149) --d3-mode record: INFO + bars: holes 0, proof mismatches of MARKED objects 0, composed registers with a source differ from the stream 0 (proof mismatching 0 on the
            registers the record sources: lights, ShadowPrm, TexProjLight / LightEye from the wrestler light camera, RimLightDepth, View via the pass bank, the palette where the part's bone count is known); the objects not
            marked are listed by named reason (H4: SphereOcc, WklLvLst / MuscleParam, no palette, no light state, cloth / pieces never pinned).
  plants    each in the arm where it can be seen, against the honest arm of the same capture:
              stream arm   hole_no_packet / join_by_index / offset0 / marker_count0 -> holes > 0; ordinal_off -> ordinal mismatch > 0; skip_in_proof_frame -> a skip in a proof frame (> 0);
                           sphere_from_prev_frame -> proof mismatches > 0 (VS c168-c171); template_first / stale_template -> proof mismatches or fewer markers
              record arm   texproj_from_colour_call (VS c68-c71 from the colour call's state: 0 of 1920 match) / camera_prev_frame / props_camera_for_wrestlers -> proof mismatches > 0 at VS c68-c71 / c166;
                           brock_update_palette (THE PAST BUG, the palette of the update hook's timing) / skel_palette_zero / stale_slots -> proof mismatches > 0 at the palette registers (or a marked object with zero rows);
                           state_prev_object / record_offset0 / negate_dirs -> light mismatches > 0
  part_counts_missing is reported, not required: nothing reads the counts of a marker's synthesized draws (the wrestler core's per-frame pass reads the REAL frame, where a marked object has no draws and its join is kept by
            the object's proof frames), see 18.7."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

LINE = re.compile(r'\[rdr\]\[d3\]\[wrestlers\] mode (\w+): markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), learned (\d+), objects complete \(marked (\d+) of (\d+)\), proof objects (\d+) '
                  r'\(parts (\d+), mismatching (\d+), of marked objects (\d+)\), ordinal mismatch (\d+), unconsumed (\d+), gate resets (\d+), calls = runs checked (\d+) differ (\d+), record missing (\d+), hook: '
                  r'markers (\d+) dropped (\d+) .*?readiness: marker parts composed (\d+), differing from the stream (\d+), register reads (\d+), withheld (\d+), epoch drops (\d+), sequence length differs (\d+), '
                  r'never proved (\d+), marked with an unsourced register (\d+), latched (\d+), sink-skipped frames (\d+), skipped in proof frames (\d+), state differ (\d+)')
KEYS = ['mode', 'markers', 'parts', 'holes', 'no_seq', 'learned', 'marked', 'complete_of', 'proof_obj', 'proof_parts', 'proof_mism', 'proof_mism_marked', 'ord_mism', 'unconsumed', 'gate_resets', 'calls_checked',
        'calls_differ', 'record_missing', 'h_markers', 'h_dropped', 'composed', 'differ', 'reads', 'withheld', 'epoch', 'seq_len', 'never', 'unsrc', 'latched', 'sink_skipped', 'skipped_in_proof', 'state_differ']
JOINED = re.compile(r'objects seen (\d+) joined (\d+)')
UNPINS = re.compile(r'object unpins (\d+)')
RUNS = re.compile(r'keyed runs seen (\d+), refused \(stay recorded in full\): wrestler program not pinned \(cloth, piece, unknown model, not learned yet\) (\d+), another class\'s program \(arena, props, ring\) (\d+), no variant (\d+), other (\d+); records indexed (\d+), light cameras (\d+)')
REGS = re.compile(r'proof mismatch registers \(all objects\): (.*)')


def run(cmd, timeout, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e)
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


def find_caps(main_co, out):
    """name -> (capture dir, frames, has_records)"""
    caps = {}
    cands = {
        'B': (os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture'), False),
        '7079': (os.path.join(main_co, 'out', 'lane115', 'r7079', 'capture'), False),
        'wm30': (os.path.join(main_co, 'out', 's101', 'pieces_ours', 'wm30_7084', 'capture'), False),
        'J1': (os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_172341_872_0_pc_run_exit', 'capture'), True),
        '7147': (os.path.join(main_co, 'out', 'd3caster', 'z7147', 'capture'), True),
        '7149': (os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture'), True),
    }
    zips = {'7147': os.path.join(main_co, 'out', 's101', 'runs', '20261113_081920_s100_owner_s89_brock', 'case_20261113_082000_320_0_pc_run_exit.zip'),
            '7149': os.path.join(main_co, 'out', 's101', 'runs', '20261113_093518_s100_owner_s89_brock', 'case_20261113_093550_855_0_pc_run_exit.zip')}
    for k, (d, rec) in cands.items():
        if not os.path.isfile(os.path.join(d, 'images.bin')) and k in zips and os.path.isfile(zips[k]):   # a kept zip: unpack under out\ of THIS checkout
            dd = os.path.join(out, 'z' + k)
            if not os.path.isfile(os.path.join(dd, 'capture', 'images.bin')):
                zipfile.ZipFile(zips[k]).extractall(dd)
            d = os.path.join(dd, 'capture')
        if os.path.isfile(os.path.join(d, 'images.bin')):
            n = len([x for x in os.listdir(os.path.join(d, 'frames')) if x.endswith('.bin')])
            # 7147 / 7149 were recorded with the LIVE reader's wrestler thin draws on: most of their wrestler draws are already lite (kDrawThin, no vertices) and the full ones follow the live reader's PER-KEY proof schedule, which no
            # emulated hook can change. The wrestler core releases a part nobody claimed for 240 frames (Sweep), and a marked object's claimed draws are the capture's full ones that fall on frames the hook did not skip,
            # so past frame ~240 the offline arm loses templates for a reason the live arm (proof frames by OBJECT: every part full and claimed together) does not have. Those three captures (J1 too: it was recorded with the live reader's thin draws on) are judged over 230 frames.
            if k in ('J1', '7147', '7149'):
                n = min(n, 230)
            caps[k] = (d, n, rec and os.path.isdir(os.path.join(d, 'scene')))
    return caps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--caps', default='')
    ap.add_argument('--plants', default='')
    ap.add_argument('--no-plants', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    out = os.path.join(root, 'out', 'd3wrestler')
    os.makedirs(out, exist_ok=True)
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    caps = find_caps(main_co, out)
    only = set(x for x in args.caps.split(',') if x)
    if only:
        caps = {k: v for k, v in caps.items() if k in only}
    if not caps:
        print('FAIL  input missing: no kept capture found (Check B, 7079, wm30, J1, 7147, 7149)')
        return 1
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        fails += not ok

    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc != 0:
            print(text[-2500:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    cache = {}

    def arm(ck, tag, extra, d3=True, env=None, frames=0):
        key = (ck, tag)
        if key in cache:
            return cache[key]
        cap, n, _ = caps[ck]
        n = frames or args.frames or n
        d = os.path.join(out, 'arm_%s_%s' % (ck, tag))
        cmd = [exe, '--capture', cap, '--wrestlers-ours', '--game', game, '--frames', str(n), '--thin', 'wrestlers', '--thin-run', d] + (['--d3', 'wrestlers'] + extra if d3 else extra)
        rc, text = 1, ''
        for attempt in range(8):   # (the GPU is shared: this exact message is retried, nothing else)
            rc, text = run(cmd, 1800, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s/%s failed (exit %d)' % (ck, tag, rc))
        m = LINE.search(text)
        j = JOINED.search(text)
        u = UNPINS.search(text)
        r = RUNS.search(text)
        g = REGS.search(text)
        res = {'rc': rc, 'rows': read_csv(csv), 'text': text, 'n': n, 'c': dict(zip(KEYS, [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None,
               'joined': tuple(map(int, j.groups())) if j else None, 'unpins': int(u.group(1)) if u else None, 'runs': tuple(map(int, r.groups())) if r else None, 'regs': g.group(1) if g else ''}
        res['reasons'] = re.findall(r'not marked: (\d+) objects: (.*)', text)
        cache[key] = res
        return res

    def diffs(a, b):
        return [f for f in sorted(a['rows']) if a['rows'].get(f) != b['rows'].get(f)]

    plant_arg = set(x for x in args.plants.split(',') if x)
    try:
        for ck, (cap, nfull, has_rec) in caps.items():
            ref = arm(ck, 'd1', [], d3=False)
            n = ref['n']
            verdict(len(ref['rows']) == n and 'validation errors 0' in ref['text'], '[%s] D1 reference arm (--wrestlers-ours --thin wrestlers): %d frames, wrestler core joined %s, object unpins %s' % (ck, len(ref['rows']), ref['joined'], ref['unpins']))
            ledger = os.path.join(out, 'ledger_%s.txt' % ck)
            p1 = arm(ck, 'p1', ['--d3-mode', 'stream', '--d3-ledger-out', ledger])
            s = arm(ck, 'p2', ['--d3-mode', 'stream', '--d3-ledger-in', ledger])
            c = s['c']
            if not c:
                verdict(False, '[%s] stream: no [rdr][d3][wrestlers] census line' % ck)
                continue
            d = diffs(s, ref)
            verdict(not d and len(s['rows']) == n, '[%s] stream (ledger in): the picture of all %d frames byte-equal to the D1 arm (%d differ%s)' % (ck, len(s['rows']), len(d), (': ' + str(d[:8])) if d else ''))
            d1 = diffs(p1, ref)
            print('INFO  [%s] pass 1 (no ledger): %d of %d frames differ before a drift register is known' % (ck, len(d1), n))
            verdict(c['markers'] > 0 and c['parts'] > 0 and c['complete_of'] > 0, '[%s] markers engaged: %d markers, %d part draws synthesized from sequences, %d part-learns, %d objects marked of %d pinned' % (ck, c['markers'], c['parts'], c['learned'], c['marked'], c['complete_of']))
            verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and c['seq_len'] == 0 and c['epoch'] == 0 and c['record_missing'] == 0,
                    '[%s] holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, sequence length differs %d, epoch drops %d' % (ck, c['holes'], c['no_seq'], c['ord_mism'], c['unconsumed'], c['seq_len'], c['epoch']))
            verdict(c['proof_mism'] == 0 and c['proof_parts'] > 0 and c['skipped_in_proof'] == 0 and c['never'] == 0 and c['unsrc'] == 0 and c['state_differ'] == 0,
                    '[%s] proof compare: %d objects, %d parts, mismatching %d (marked objects %d), state differ %d, skipped in a proof frame %d, never proved %d, marked with an unsourced register %d' % (
                        ck, c['proof_obj'], c['proof_parts'], c['proof_mism'], c['proof_mism_marked'], c['state_differ'], c['skipped_in_proof'], c['never'], c['unsrc']))
            verdict(c['calls_differ'] == 0 and c['calls_checked'] > 0, '[%s] calls = runs: checked %d differ %d' % (ck, c['calls_checked'], c['calls_differ']))
            verdict(c['composed'] > 0 and c['differ'] == 0 and c['withheld'] == 0, '[%s] marker banks: %d marker parts composed, %d differing from the stream (%d register reads, withheld %d)' % (ck, c['composed'], c['differ'], c['reads'], c['withheld']))
            verdict(s['rc'] == 0 and 'validation errors 0' in s['text'], '[%s] validation layer: errors 0 (exit %d)' % (ck, s['rc']))
            verdict(s['joined'] is not None and ref['joined'] is not None and s['joined'][1] >= ref['joined'][1] and (s['unpins'] or 0) == 0,
                    '[%s] the wrestler core keeps its joins under markers: objects joined %s (D1 %s), object unpins %s' % (ck, s['joined'], ref['joined'], s['unpins']))
            if s['runs']:
                print('INFO  [%s] H4 (stay recorded in full): keyed runs seen %d, wrestler programs not pinned (cloth, pieces, unknown models, not learned yet) %d, other classes (arena, props, ring) %d, no variant %d, other %d' % ((ck,) + s['runs'][:5]))

            # ---- the record route on the captures that carry the records
            if has_rec:
                rr = arm(ck, 'rec', ['--d3-mode', 'record', '--d3-ledger-in', ledger])
                rc_ = rr['c']
                if not rc_:
                    verdict(False, '[%s] record: no census line' % ck)
                    continue
                verdict(rc_['holes'] == 0 and rc_['proof_mism'] == 0 and rc_['proof_mism_marked'] == 0 and rc_['record_missing'] == 0,
                        '[%s] record route: %d markers, %d of %d objects marked, holes %d, proof mismatching %d on %d parts (the registers the records source are bit-equal to the stream), record missing %d, marked with an unsourced register %d' % (
                            ck, rc_['markers'], rc_['marked'], rc_['complete_of'], rc_['holes'], rc_['proof_mism'], rc_['proof_parts'], rc_['record_missing'], rc_['unsrc']))
                if rr['runs']:
                    print('INFO  [%s] record route: records indexed %d, wrestler light cameras %d' % (ck, rr['runs'][5], rr['runs'][6]))
                for cnt, why in rr['reasons'][:8]:
                    print('INFO  [%s] not marked: %s objects: %s' % (ck, cnt, why[:300]))

        # ---- plants: one capture each (the newest that carries records; the stream plants on the same)
        if not args.no_plants:
            pk = '7149' if '7149' in caps else ('7147' if '7147' in caps else next(iter(caps)))
            n_pl = min(caps[pk][1], 200)
            ledger = os.path.join(out, 'ledger_%s.txt' % pk)
            honest_s = arm(pk, 'pl_honest_s', ['--d3-mode', 'stream', '--d3-ledger-in', ledger], frames=n_pl)
            hs = honest_s['c']
            stream_want = {'hole_no_packet': lambda x: x['holes'] > 0, 'join_by_index': lambda x: x['holes'] > 0, 'offset0': lambda x: x['holes'] > 0, 'marker_count0': lambda x: x['holes'] > 0,
                           'ordinal_off': lambda x: x['ord_mism'] > 0, 'skip_in_proof_frame': lambda x: x['skipped_in_proof'] > 0, 'sphere_from_prev_frame': lambda x: x['proof_mism'] > 0,
                           }
            for pl, test in stream_want.items():
                if plant_arg and pl not in plant_arg:
                    continue
                x = arm(pk, 'pls_' + pl, ['--d3-mode', 'stream', '--d3-ledger-in', ledger, '--d3-fault', pl], frames=n_pl)
                xc = x['c']
                pic = diffs(x, honest_s)
                ok = bool(xc) and (test(xc) if test else (len(pic) > 0 or xc['proof_mism'] > 0 or xc['markers'] < hs['markers']))
                verdict(ok, '[%s] plant %s detected (stream arm): markers %s (honest %s), holes %s, ordinal mismatch %s, proof mismatching %s (%s), skipped in a proof frame %s, pictures differing from the honest arm %d' % (
                    pk, pl, xc and xc['markers'], hs and hs['markers'], xc and xc['holes'], xc and xc['ord_mism'], xc and xc['proof_mism'], x['regs'][:60], xc and xc['skipped_in_proof'], len(pic)))
            if caps[pk][2]:
                honest_r = arm(pk, 'pl_honest_r', ['--d3-mode', 'record', '--d3-ledger-in', ledger], frames=n_pl)
                hr = honest_r['c']
                verdict(hr is not None and hr['proof_mism'] == 0, '[%s] record arm honest baseline (%d frames): proof mismatching %s, marked %s of %s' % (pk, n_pl, hr and hr['proof_mism'], hr and hr['marked'], hr and hr['complete_of']))
                regs_of = lambda x: x['regs']  # noqa: E731
                record_want = {'texproj_from_colour_call': lambda x, r: x['proof_mism'] > 0 and 'VS c68' in r,
                               'camera_prev_frame': lambda x, r: x['proof_mism'] > 0 and ('VS c68' in r or 'VS c166' in r),
                               'props_camera_for_wrestlers': lambda x, r: x['proof_mism'] > 0 and 'VS c68' in r,
                               'state_prev_object': lambda x, r: x['proof_mism'] > 0, 'record_offset0': lambda x, r: x['proof_mism'] > 0 or x['record_missing'] > 0,
                               'negate_dirs': lambda x, r: x['proof_mism'] > 0, 'brock_update_palette': lambda x, r: x['proof_mism'] > 0 or x['marked'] < hr['marked'],
                               'skel_palette_zero': lambda x, r: x['proof_mism'] > 0, 'stale_slots': lambda x, r: x['proof_mism'] > 0}
                for pl, test in record_want.items():
                    if plant_arg and pl not in plant_arg:
                        continue
                    x = arm(pk, 'plr_' + pl, ['--d3-mode', 'record', '--d3-ledger-in', ledger, '--d3-fault', pl], frames=n_pl)
                    xc = x['c']
                    verdict(bool(xc) and test(xc, regs_of(x)), '[%s] plant %s detected (record arm): proof mismatching %s (%s), marked %s (honest %s), record missing %s' % (
                        pk, pl, xc and xc['proof_mism'], regs_of(x)[:80], xc and xc['marked'], hr and hr['marked'], xc and xc['record_missing']))
            # reported, not required (see the docstring): lane 35's template plants and part_counts_missing change nothing for the wrestlers
            for pl in ('template_first', 'stale_template'):
                x = arm(pk, 'pls_' + pl, ['--d3-mode', 'stream', '--d3-ledger-in', ledger, '--d3-fault', pl], frames=n_pl)
                print('INFO  plant %s (stream arm, NOT a bar: the sequences of wrestler parts are static, one material per part): markers %s (honest %s), pictures differing from the honest arm %d' % (pl, x['c'] and x['c']['markers'], hs and hs['markers'], len(diffs(x, honest_s))))
            x = arm(pk, 'pls_part_counts_missing', ['--d3-mode', 'stream', '--d3-ledger-in', ledger, '--d3-fault', 'part_counts_missing'], frames=n_pl)
            print('INFO  plant part_counts_missing (stream arm, NOT a bar): joined %s (honest %s), object unpins %s, pictures differ from the honest arm on %d frames, holes %s' % (
                x['joined'], honest_s['joined'], x['unpins'], len(diffs(x, honest_s)), x['c'] and x['c']['holes']))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('PASS' if not fails else 'FAIL (%d)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
