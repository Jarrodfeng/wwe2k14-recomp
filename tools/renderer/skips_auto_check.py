#!/usr/bin/env python3
"""S102 lane K3 offline proof, 0 game runs: the D3 skip switched ON and OFF by the controller's decision at forced frames, through the REAL binder and the shipped DecideCall (fview, the guest hook EMULATED).

  py -I tools\\renderer\\skips_auto_check.py [--root DIR] [--no-build] [--frames N]

Capture: Check B (the Brock match, out\\s101\\thin\\case_brock\\capture, 341 frames; the main checkout's), arena chain (--thin arena --d3 arena), two passes as tools\\d3\\d3_check.py (pass 1 writes the offline ledger).
fview's emulated hook runs DecideCall for every object call; skips_auto::Allows() is read there (d3_guest_core.h), and fview latches the schedule `D3_AUTO_SCHED` ("0:on,100:off,160:on") at each frame
boundary exactly as frame_stats.h latches the controller's word. Arms:
  ours     the --arena-ours picture, the reference
  on       the shipped D3 (no schedule: Allows() is true): markers > 0, picture equal to ours
  alloff   schedule "0:off": 0 markers (every call recorded in full, SwitchOff), picture equal to ours
  flip     schedule "0:on,100:off,160:on,230:off,270:on": the picture of ALL frames equal to ours across the flips, holes 0, no sequence 0, unconsumed 0, ordinal mismatches 0, the gate never latched,
           proof mismatches 0; markers strictly between alloff and on (the flips took effect); the markers of the off windows are 0 (counted by the markers of the arm that is on in 0-99 only)
  planted  the flip arm with the fault hole_no_packet (a marker without its packet): holes > 0 (the bars above see a real fault under a flip)
  default  the same flip arm under the shipped default bundle's arena side (thin arena on, the proven pin / verify rules): that is the arm above (arena D3 needs thin arena); how many frames the lever took is
           the markers count of `on` vs `flip`
Exit 0 = every bar passes."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'd3'))
import d3_check as dc  # noqa: E402  (its CENSUS regex, run, read_csv, main_checkout)
import d3_caster_check as cc  # noqa: E402  (the caster chain's census line)
import d3_ring_check as rr  # noqa: E402  (the ring chain's census line)
import zipfile  # noqa: E402

SCHED = '0:on,100:off,160:on,230:off,270:on'


def _hard_sched():
    """The caster / ring schedule: a 1-frame OFF (30), a 1-frame ON inside an off stretch (150), then every frame alternating 240-279 (a flip on the frame each object's proof falls due: the proof period
    is 32 frames, so 40 consecutive flips cover every object's proof frame)."""
    parts = ['0:on', '30:off', '31:on', '100:off', '150:on', '151:off', '200:on']
    parts += ['%d:%s' % (f, 'off' if f % 2 == 0 else 'on') for f in range(240, 280)]
    parts.append('280:on')
    return ','.join(parts)


HARD = _hard_sched()


def throttle(text):
    """(frames bound with the chains, without) from fview's last D3_AUTO_THROTTLE line, or None."""
    m = re.findall(r'frames bound with the chains (\d+), without (\d+)', text)
    return (int(m[-1][0]), int(m[-1][1])) if m else None
OFF_FRAMES = (100, 160), (230, 270)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = dc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    for need in (game, os.path.join(cap, 'images.bin'), os.path.join(cap, 'scene')):
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            return 1
    if not a.no_build:
        rc, text = dc.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'k3check')
    os.makedirs(out, exist_ok=True)
    n = a.frames or 341
    base = ['--thin', 'arena', '--d3', 'arena']
    ledger = os.path.join(out, 'ledger_B.txt')
    cache = {}
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    def arm(tag, extra, env=None):
        if tag in cache:
            return cache[tag]
        d = os.path.join(out, 'arm_' + tag)
        cmd = [exe, '--capture', cap, '--arena-ours', '--game', game, '--frames', str(n), '--thin-run', d] + extra
        text, rc = '', 0
        for attempt in range(8):
            rc, text = dc.run(cmd, 1500, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, rc))
        m = dc.CENSUS.search(text)
        res = {'rows': dc.read_csv(csv), 'text': text, 'rc': rc}
        if m:
            g = list(map(int, m.groups()))
            res['c'] = dict(zip(['markers', 'parts', 'holes', 'no_seq', 'lost', 'learned', 'complete', 'marked', 'pinned', 'proof_obj', 'proof_parts', 'mism', 'g_pass', 'g_template', 'g_light', 'g_shadow',
                                 'g_world', 'g_clock', 'g_drift', 'state_differ', 'gate_resets', 'ord_mism', 'unconsumed', 'pass_fallback', 'frames', 'h_markers', 'h_dropped', 'h_gate', 'h_proof', 'h_table', 'h_overflow', 'h_unreadable'], g))
        res['validation_errors'] = (re.search(r'validation errors (\d+)', text) or [None, None])[1]
        cache[tag] = res
        return res

    def diffs(x, y):
        return [f for f in sorted(x['rows']) if f not in y['rows'] or x['rows'][f] != y['rows'][f]]

    try:
        ours = arm('ours', [])
        verdict(len(ours['rows']) == n and not all(h == '0' for h in ours['rows'].values()), 'ours (the reference, --arena-ours): %d frames' % len(ours['rows']))
        p1 = arm('pass1', base + ['--d3-ledger-out', ledger])
        verdict(p1.get('c') is not None and os.path.isfile(ledger), 'pass 1 wrote the offline ledger')
        lb = base + ['--d3-ledger-in', ledger]
        on = arm('on', lb)
        off = arm('alloff', lb, env={'D3_AUTO_SCHED': '0:off', 'D3_AUTO_THROTTLE': '1'})
        flip = arm('flip', lb, env={'D3_AUTO_SCHED': SCHED, 'D3_AUTO_THROTTLE': '1'})
        co, cf, c0 = on.get('c'), flip.get('c'), off.get('c')
        if not (co and cf and c0):
            verdict(False, 'a census line is missing (on %s, flip %s, alloff %s)' % (bool(co), bool(cf), bool(c0)))
            return 1
        verdict(not diffs(on, ours) and co['markers'] > 0, 'on (the shipped D3, Allows() true): picture of all %d frames equal to ours (%d differ), %d markers' % (n, len(diffs(on, ours)), co['markers']))
        verdict(c0['markers'] == 0 and not diffs(off, ours) and c0['holes'] == 0 and c0['unconsumed'] == 0,
                'alloff (every call recorded in full, SwitchOff): 0 markers (%d), picture equal to ours (%d differ), holes %d, unconsumed %d' % (c0['markers'], len(diffs(off, ours)), c0['holes'], c0['unconsumed']))
        d = diffs(flip, ours)
        verdict(not d and len(flip['rows']) == n, 'flip %s: the picture of all %d frames equal to the reference across the flips (%d differ%s)' % (SCHED, n, len(d), (': ' + str(d[:8])) if d else ''))
        verdict(cf['holes'] == 0 and cf['no_seq'] == 0 and cf['ord_mism'] == 0 and cf['unconsumed'] == 0 and cf['lost'] == 0,
                'flip: holes %d, no sequence %d, ordinal mismatches %d, unconsumed %d, lost %d (all must be 0)' % (cf['holes'], cf['no_seq'], cf['ord_mism'], cf['unconsumed'], cf['lost']))
        verdict(cf['mism'] == 0 and cf['gate_resets'] == 0 and '[rdr][d3] latched off' not in flip['text'], 'flip: proof mismatches %d, gate resets %d (never reset), the gate never latched' % (cf['mism'], cf['gate_resets']))
        off_len = sum(b - a_ for a_, b in OFF_FRAMES)
        lo, hi = c0['markers'], co['markers']
        verdict(lo < cf['markers'] < hi and cf['markers'] <= hi * (1.0 - 0.5 * off_len / n),
                'flip: markers %d between alloff %d and on %d (%d of %d frames off): the flips took effect' % (cf['markers'], lo, hi, off_len, n))
        # frames the lever took: the on / off split of the emulated hook's decisions
        print('INFO  the lever took %.0f %% of the markers the always-on arm made (%d of %d); the off windows are frames %s' % (100.0 * cf['markers'] / max(1, hi), cf['markers'], hi, OFF_FRAMES))
        tf, ta = throttle(flip['text']), throttle(off['text'])
        verdict(tf is not None and tf[1] > 0 and tf[0] > 0 and ta is not None and ta[0] <= 8 + 2, '[arena] the throttle ran (the chains bound only around an applied skip + the warm-up): flip bound %s frames with / without the chains, alloff %s (at most the first frames before the ring fills)' % (tf and tf, ta and ta))
        verdict(on.get('validation_errors') in (None, '0') and flip.get('validation_errors') in (None, '0'), 'validation errors: on %s, flip %s' % (on.get('validation_errors'), flip.get('validation_errors')))
        pl = arm('planted', lb + ['--d3-fault', 'hole_no_packet'], env={'D3_AUTO_SCHED': SCHED, 'D3_AUTO_THROTTLE': '1'})
        cp = pl.get('c')
        verdict(cp is not None and cp['holes'] > 0, 'planted hole_no_packet under the flip schedule: holes %s (> 0: the bars above see a real fault across flips)' % (cp and cp['holes']))
        # ---- the caster and the ring chains: the same shipped DecideCall, the harder schedule ------------------------------------------------------------------------------------------------------------
        def chain(name, cap_dir, n_frames, cmd_extra, line_re, keys, ref_extra, ledger_path, picture_bar):
            def carm(tag, extra, env=None):
                key = name + '_' + tag
                if key in cache:
                    return cache[key]
                d = os.path.join(out, 'arm_' + key)
                cmd = [exe, '--capture', cap_dir, '--game', game, '--frames', str(n_frames), '--thin-run', d] + extra
                rc2, text2 = 1, ''
                for attempt in range(8):
                    rc2, text2 = dc.run(cmd, 1500, env)
                    if 'Not enough graphics memory for the frame ring' in text2 and attempt < 7:
                        time.sleep(40)
                        continue
                    break
                csv = os.path.join(d, 'frames.csv')
                if rc2 not in (0, 4) or not os.path.isfile(csv):
                    print(text2[-2500:])
                    raise RuntimeError('arm %s failed (exit %d)' % (key, rc2))
                m = line_re.search(text2)
                res = {'rows': dc.read_csv(csv), 'text': text2, 'rc': rc2, 'c': dict(zip(keys, [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None}
                cache[key] = res
                return res
            ref = carm('ref', ref_extra) if ref_extra is not None else None
            carm('p1', cmd_extra + ['--d3-ledger-out', ledger_path])
            lb2 = cmd_extra + ['--d3-ledger-in', ledger_path]
            on2, off2 = carm('on', lb2), carm('alloff', lb2, {'D3_AUTO_SCHED': '0:off', 'D3_AUTO_THROTTLE': '1'})
            fl = carm('flip', lb2, {'D3_AUTO_SCHED': HARD, 'D3_AUTO_THROTTLE': '1'})
            c1, c0, cf = on2['c'], off2['c'], fl['c']
            if not (c1 and c0 and cf):
                verdict(False, '[%s] a census line is missing (on %s, alloff %s, flip %s)' % (name, bool(c1), bool(c0), bool(cf)))
                return
            verdict(c1['markers'] > 0 and c0['markers'] == 0, '[%s] on: %d markers; alloff (every call recorded in full): %d markers' % (name, c1['markers'], c0['markers']))
            bad_keys = [k for k in ('holes', 'no_seq', 'ord_mism', 'unconsumed') if cf.get(k, 0) != 0]
            verdict(not bad_keys and cf.get('gate_resets', 0) == 0 and 'latched off' not in fl['text'] and cf.get('proof_mism_marked', 0) == 0,
                    '[%s] flip (1-frame off, 1-frame on, 40 alternating frames over every proof period): holes %s, no sequence %s, ordinal mismatches %s, unconsumed %s, gate resets %s, proof mismatches of marked objects %s, never latched; markers %d (on %d)' % (
                        name, cf.get('holes'), cf.get('no_seq'), cf.get('ord_mism'), cf.get('unconsumed'), cf.get('gate_resets'), cf.get('proof_mism_marked'), cf['markers'], c1['markers']))
            tt = throttle(fl['text'])
            verdict(tt is not None and tt[1] > 0 and tt[0] > 0, '[%s] the throttle ran: frames bound with / without the chains %s' % (name, tt))
            verdict(c0['markers'] < cf['markers'] < c1['markers'], '[%s] flip: markers %d between alloff %d and on %d (the flips took effect)' % (name, cf['markers'], c0['markers'], c1['markers']))
            if picture_bar and ref is not None:
                dd = diffs(fl, ref)
                verdict(not dd, '[%s] flip: the picture of all %d frames equals the reference arm across the flips (%d differ)' % (name, n_frames, len(dd)))
            if cf.get('skipped_in_proof') is not None:
                verdict(cf['skipped_in_proof'] == 0, '[%s] flip: markers in an object\'s proof frame %s (must be 0)' % (name, cf['skipped_in_proof']))

        chain('ring', cap, n, ['--ring-ours', '--thin', 'ring', '--d3', 'ring'], rr.LINE, rr.KEYS, ['--ring-ours', '--thin', 'ring'], os.path.join(out, 'ledger_ring.txt'), True)
        cap_c = os.path.join(main_co, 'out', 'd3caster', 'z7147', 'capture')
        if not os.path.isfile(os.path.join(cap_c, 'images.bin')):
            zp = os.path.join(main_co, 'out', 's101', 'runs', '20261113_081920_s100_owner_s89_brock', 'case_20261113_082000_320_0_pc_run_exit.zip')
            if not os.path.isfile(zp):
                verdict(False, '[casters] input missing: the caster verify capture (run 7147) ' + zp)
                cap_c = None
            else:
                dz = os.path.join(out, 'z7147')
                if not os.path.isfile(os.path.join(dz, 'capture', 'images.bin')):
                    zipfile.ZipFile(zp).extractall(dz)
                cap_c = os.path.join(dz, 'capture')
        if cap_c:
            nc = len(os.listdir(os.path.join(cap_c, 'frames')))
            chain('casters', cap_c, nc, ['--d3', 'casters', '--d3-mode', 'stream'], cc.LINE, cc.KEYS, None, os.path.join(out, 'ledger_casters.txt'), False)
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('%d bar(s) failed' % fails if fails else 'all bars passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
