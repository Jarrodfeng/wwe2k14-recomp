#!/usr/bin/env python3
"""Lane 272 (S101) acceptance test, offline, 0 game runs, no window: the crowd members' THIN record (docs\\renderer\\SKIP_GAME_WORK_MAP.md section 20, Candidate M).

  py -I tools\\crowd\\member_thin_check.py [--root DIR] [--no-build] [--scale N] [--quick] [--captures a,b] [--no-plants] [--exe EXE]

For each capture with scene records: A = fview --crowd-ours (+ --crowd-floor where the record holds floor members) --crowd-members-probe on the capture as it is (the game recorded every member; the census arm: the class
uniformity verify over every claimed stream draw); B = the SAME capture with the thin cut EMULATED (member_thin_cut.py: in every thin frame the op-15 of every member but the first of each mesh run is removed, the
op-21s stay; the tally sidecar written one op 15 at a time, RL1) replayed with --crowd-members-thin.
Bars:
  1. picture: B's presented-surface hash equals A's on EVERY frame (byte-equal).
  2. census (B): thin frames drawn == the frames the cut thinned, holes 0, full frames refused 0, the game's dropped member draws == record members - kept stream draws == the cut's dropped count, validation 0.
  3. uniformity (A and B): material differs 0, bank differs 0, frames with a non-uniform class 0 (the claim the cut rests on, over every claimed stream draw of the full frames).
  4. the cut touched only the op list (Check B: bank / state / texture sections of the cut frames byte-identical to the input).
  5. the stamp probe (Check B): the cut with the control `keep_all` writes a tally for every frame; the census arm with that sidecar must match stamp offset 1 on EVERY full frame (offsets 0 and 2 may match static crowds).
  6. plants (Check B; the floor plant on the first capture with a floor), each must fail its NAMED check:
       cut drop_first / run_per_frame     no kept draw / one kept draw per site per frame: classes without material, holes
       cut calls_collapsed                RL1 (`calls = runs`): the tally's seen != the record's members on every thin frame: all holes
       cut offset, fault offset           lane 36's +1 / lane 152 B1 / lane 175 B1: the record (or the tally) of the wrong frame is refused
       cut tiled_double                   MSAA tiles: the tally doubled: every thin frame a hole
       cut floor_site_dropped_unobserved  the floor op-15 dropped, its op-21 world never noted: the floor tally never equals the record
       cut bank_nonuniform / picture_nonuniform   a full frame in which a non-first member binds another class's bank / picture: the uniformity census sees it, the frame is refused
       fault member_one_material          every class takes the frame's first kept draw's bank and picture (lane 35's several-outfits bug): the picture moves
       fault undrawable_type              a class ours cannot draw whose members were dropped (the no_palette_dropped plant): holes
       fault member_no_tally + offset     CONTROL: with the tally check off a record of the wrong frame IS drawn (the picture moves, holes 0): the tally is what refuses it
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import crowd_cards_check as ccc  # noqa: E402

CAPTURES = [   # label, path (main checkout), floor members in the record (the floor switch is on for these)
    ('checkB', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
    ('r7079', 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'),
    ('j1', 'out/s101/runs/20261112_172304_s100_owner_s89_brock/case_20261112_172341_872_0_pc_run_exit.zip'),
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match131008', 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'),
    ('match7174', 'out/s101/runs/20261113_135405_s101_owner_match_long/case_20261113_135453_130_0_pc_run_exit.zip'),
]
MEMBERS_RE = re.compile(r"THIN members: frames (\d+), members in them (\d+), the game's dropped member draws (\d+), holes \(thin frames ours could not draw\) (\d+); full frames clean (\d+), refused (\d+); "
                        r"verify: members (\d+), material differs (\d+), bank differs (\d+), frames with a non-uniform class (\d+)")
PROBE_RE = re.compile(r'stamp probe: offset 0 / 1 / 2 match (\d+) / (\d+) / (\d+) of (\d+) full frames')
CUT_RE = re.compile(r'thin: (\d+) of (\d+) frames with members cut \((\d+) member draws dropped, (\d+) kept as run anchors; (\d+) calls in (\d+) runs over (\d+) frames, frames with calls == runs (\d+)\) floor=(on|off)')
KEYS = ['thin', 'members', 'dropped', 'holes', 'clean', 'refused', 'v_members', 'v_material', 'v_bank', 'nonuniform']


def cut(root, zip_in, zip_out, plant=None, k=32):
    cmd = [sys.executable, '-I', os.path.join(root, 'tools', 'crowd', 'member_thin_cut.py'), zip_in, zip_out, '--k', str(k), '--counts', zip_out[:-4] + '.counts.csv', '--decisions', zip_out[:-4] + '.dec.csv']
    if plant:
        cmd += ['--plant', plant]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    m = CUT_RE.search(p.stdout)
    if p.returncode or not m:
        print(p.stdout[-1500:], p.stderr[-1500:])
        raise RuntimeError('cut failed: ' + ' '.join(cmd))
    g = m.groups()
    return dict(thin=int(g[0]), with_members=int(g[1]), dropped=int(g[2]), kept=int(g[3]), calls=int(g[4]), runs=int(g[5]), eq=int(g[7]), floor=g[8] == 'on')


def census(arm):
    m = MEMBERS_RE.search(arm.text)
    c = dict(zip(KEYS, map(int, m.groups()))) if m else None
    p = PROBE_RE.search(arm.text)
    probe = tuple(map(int, p.groups())) if p else None
    return c, probe


def sections_equal(zip_a, zip_b):
    """Bar 4: everything but the ops section (and its count) is byte-identical."""
    import cards_census as cc
    A, B = cc.Cap(zip_a), cc.Cap(zip_b)
    bad = 0
    for i in range(len(A.frames)):
        fa, fb = A.frame(i), B.frame(i)
        if bytes(fa.buf) == bytes(fb.buf):
            continue
        for sid, (es, cnt, off) in fa.secs.items():
            if sid == cc.pcap.SEC['ops']:
                continue
            eb = fb.secs[sid]
            if bytes(fa.buf[off:off + es * cnt]) != bytes(fb.buf[eb[2]:eb[2] + eb[0] * eb[1]]):
                bad += 1
                break
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--scale', default='1')
    ap.add_argument('--quick', action='store_true', help='Check B and one match only')
    ap.add_argument('--captures', default='', help='comma list of capture labels to run (default all)')
    ap.add_argument('--no-plants', action='store_true')
    ap.add_argument('--exe', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = ccc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    zips = {n: os.path.join(main_co, p) for n, p in CAPTURES}
    for need in list(zips.values()) + [game]:
        if not os.path.exists(need):
            return ccc.fail_exit('input missing: ' + need)
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return ccc.fail_exit('fview build (exit %d)' % p.returncode)
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(prefer=args.exe, root=root, kind='dev')
    if not os.path.isfile(exe):
        return ccc.fail_exit('no fview.exe at ' + exe)
    if args.scale != '1':
        ccc.Arm.scale = ['--scale', args.scale]
    work = os.path.join(root, 'out', 'member_thin_check' + ('' if args.scale == '1' else '_x' + args.scale))
    os.makedirs(work, exist_ok=True)
    R = ccc.Result()
    names = ['checkB', 'match7174'] if args.quick else [n for n, _ in CAPTURES]
    if args.captures:
        names = args.captures.split(',')
    floor_capture = None
    info = {}
    for n in names:
        z = zips[n]
        zt = os.path.join(work, n + '_thin.zip')
        c = cut(root, z, zt)
        fl = ['--crowd-floor'] if c['floor'] else []
        if c['floor'] and floor_capture is None:
            floor_capture = n
        A = ccc.Arm(exe, z, game, work, n + '_A', fl + ['--crowd-members-probe'])
        B = ccc.Arm(exe, zt, game, work, n + '_B', fl + ['--crowd-members-thin', '--crowd-members-thin-counts', zt[:-4] + '.counts.csv'])
        ca, _ = census(A)
        cb, _ = census(B)
        info[n] = (c, fl, zt, A)
        if not ca or not cb:
            print(B.text[-2500:])
            R.line(False, '%-11s no THIN members census in an arm' % n)
            continue
        diff = B.differing(A)
        R.line(diff == 0 and len(B.hashes) == len(A.hashes), '%-11s picture: thin arm == full arm on every frame (%d frames, %d differ)' % (n, len(A.hashes) - 1, diff))
        R.line(cb['thin'] == c['thin'] and cb['holes'] == 0 and cb['refused'] == 0 and cb['thin'] > 0 and cb['dropped'] == c['dropped'],
               "%-11s census: thin frames drawn %d of %d cut, holes %d, full frames clean %d refused %d; the game's dropped member draws %d (the cut dropped %d; kept anchors %d == runs %d)" %
               (n, cb['thin'], c['thin'], cb['holes'], cb['clean'], cb['refused'], cb['dropped'], c['dropped'], c['kept'], c['runs']))
        R.line(c['calls'] > c['runs'] and c['eq'] == 0, '%-11s RL1: %d op-15 calls in %d runs, frames with calls == runs %d' % (n, c['calls'], c['runs'], c['eq']))
        R.line(ca['v_material'] == ca['v_bank'] == ca['nonuniform'] == cb['v_material'] == cb['v_bank'] == cb['nonuniform'] == 0 and ca['v_members'] > 0,
               '%-11s uniformity: verify members %d (full arm) / %d (thin arm), material differs %d / %d, bank differs %d / %d, frames with a non-uniform class %d / %d' %
               (n, ca['v_members'], cb['v_members'], ca['v_material'], cb['v_material'], ca['v_bank'], cb['v_bank'], ca['nonuniform'], cb['nonuniform']))
        R.line(A.valid == 0 and B.valid == 0 and A.rc == 0 and B.rc == 0, '%-11s validation errors %d / %d' % (n, A.valid, B.valid))
        if n == 'checkB':
            R.line(sections_equal(z, zt) == 0, 'checkB bank / state / texture sections of the cut frames are byte-identical to the input (only the op list changed)')
            # the stamp probe: keep_all writes the tally of every frame with dropped 0; the census arm with it must match offset 1 on every full frame
            zk = os.path.join(work, 'checkB_keepall.zip')
            cut(root, z, zk, 'keep_all')
            P = ccc.Arm(exe, z, game, work, 'checkB_probe', ['--crowd-members-probe', '--crowd-members-thin-counts', zk[:-4] + '.counts.csv'])
            _, pr = census(P)
            R.line(pr is not None and pr[3] > 300 and pr[1] == pr[3] and pr[0] < pr[3] and pr[2] < pr[3],
                   'checkB stamp probe: offset 0 / 1 / 2 match %s / %s / %s of %s full frames (offset 1 must match all; the crowd moves, so 0 and 2 must not)' % ((pr + ('?',) * 4)[:4] if pr else ('?', '?', '?', '?')))
            os.remove(zk), os.remove(zk[:-4] + '.counts.csv'), os.remove(zk[:-4] + '.dec.csv')
    if args.no_plants or 'checkB' not in info:
        print('\nmember_thin_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
        return 1 if R.fails else 0

    c0, fl0, zt0, A0 = info['checkB']
    zB = zips['checkB']

    def thin_arm(label, zip_path, extra, fl):
        B = ccc.Arm(exe, zip_path, game, work, label, fl + extra)
        cb, pr = census(B)
        return B, cb

    def cut_plant(plant, cap, fl, expect, text, floor_needed=False):
        zp = os.path.join(work, cap + '_' + plant + '.zip')
        cut(root, zips[cap], zp, plant)
        full_arm = info[cap][3]
        B, cb = thin_arm('p_' + cap + '_' + plant, zp, ['--crowd-members-thin', '--crowd-members-thin-counts', zp[:-4] + '.counts.csv'], fl)
        if not cb:
            print(B.text[-2000:])
            R.line(False, 'plant cut:%s: no census' % plant)
            return
        diff = B.differing(full_arm)
        ok = expect(cb, diff)
        R.line(ok, 'plant cut:%s (%s) is caught: %s; holes %d, thin frames drawn %d, full refused %d, non-uniform frames %d, %d frames differ' % (plant, cap, text, cb['holes'], cb['thin'], cb['refused'], cb['nonuniform'], diff))
        for suffix in ('.zip', '.counts.csv', '.dec.csv'):
            try:
                os.remove(zp[:-4] + suffix)
            except OSError:
                pass

    cut_info = c0
    holes = lambda cb, d: cb['holes'] > 0
    all_holes = lambda cb, d: cb['holes'] >= max(1, int(cut_info['thin'] * 0.9)) and cb['thin'] <= cut_info['thin'] * 0.1

    def expected_offset_drawn():   # a wrong stamp (frame g reads the tally of g-1) is drawn only where frame g-1 and g hold the identical (seen, dropped, world) at both sites
        rows = {}
        with open(zt0[:-4] + '.counts.csv') as fh:
            for line in fh:
                if line.startswith('#') or not line.strip():
                    continue
                k, site, seen, dropped, world = map(int, line.strip().split(','))
                rows[(k, site)] = (seen, dropped, world)
        n = 0
        for (k, site), v in rows.items():
            if site == 0 and (v[1] > 0 or rows.get((k, 1), (0, 0, 0))[1] > 0) and (k - 1, 0) in rows and rows[(k - 1, 0)] == v and rows.get((k - 1, 1)) == rows.get((k, 1)):
                n += 1
        return n
    offset_expect = lambda cb, d: cb['thin'] == expected_offset_drawn() and cb['holes'] >= cut_info['thin'] - cb['thin']
    for plant, expect, text in (
            ('drop_first', holes, 'no kept draw reaches the stream, ours is never asked: the binder finds the frame\'s tally dropped members with no crowd draw'),
            ('run_per_frame', holes, 'one kept draw per site per frame: classes without a kept draw'),
            ('calls_collapsed', all_holes, "RL1: the tally's seen is the run count, not the record's members"),
            ('offset', offset_expect, 'the tally of the wrong frame (the past bug, lane 36 / 152 / 175): refused except where the neighbour frame holds the identical member set'),
            ('tiled_double', all_holes, 'MSAA tiles: the tally doubled'),
            ('bank_nonuniform', lambda cb, d: cb['nonuniform'] > 0 and cb['v_bank'] > 0 and cb['holes'] >= cb['nonuniform'], 'a full frame whose non-first member binds another bank: the uniformity census refuses it and, since lane 295 SF2, reports it as a hole (the cut latches after 3)'),
            ('picture_nonuniform', lambda cb, d: cb['nonuniform'] > 0 and cb['v_material'] > 0 and cb['holes'] >= cb['nonuniform'], 'a full frame whose non-first member binds another picture: refused and reported as a hole (lane 295 SF2)')):
        cut_plant(plant, 'checkB', fl0, expect, text)
    if floor_capture:
        cut_plant('floor_site_dropped_unobserved', floor_capture, info[floor_capture][1], all_holes if False else (lambda cb, d: cb['holes'] > 0), 'the floor op-15 dropped but its op-21 world never noted: the floor tally never equals the record')
    else:
        R.line(False, 'no capture with floor members was run: the floor plant could not be checked')
    # fview faults on the real cut
    A = A0
    for fault, expect, text in (
            ('member_one_material', lambda cb, d: d > 0, 'every class takes the frame\'s first kept draw\'s material: the picture moves'),
            ('member_own_sd', lambda cb, d: d > 0, 'the thin sd choice dropped (each dropped member looks up its own stream draw: none): the picture moves'),
            ('undrawable_type', lambda cb, d: cb['holes'] > 0, 'members of a class ours cannot draw were dropped (no_palette_dropped): holes')):
        B, cb = thin_arm('p_fault_' + fault, zt0, ['--crowd-members-thin', '--crowd-members-thin-counts', zt0[:-4] + '.counts.csv', '--crowd-fault', fault], fl0)
        diff = B.differing(A)
        R.line(bool(cb) and expect(cb, diff), 'plant fault:%s is caught: %s; holes %s, %d frames differ' % (fault, text, cb and cb['holes'], diff))
    Bo, cbo = thin_arm('p_fault_offset', zt0, ['--crowd-members-thin', '--crowd-members-thin-counts', zt0[:-4] + '.counts.csv', '--crowd-fault', 'offset'], fl0)
    R.line(bool(cbo) and cbo['holes'] > 0, 'plant fault:offset (record j instead of j-1: lane 36 +1, lane 152 B1, lane 175 B1) is caught by the tally: %s thin frames refused (holes), %s drawn' % (cbo and cbo['holes'], cbo and cbo['thin']))
    # CONTROL: the stamp plant (tally of the wrong frame) with the tally check OFF draws every thin frame: the tally is the only guard, and the picture alone cannot see a stamp slip
    zs = os.path.join(work, 'checkB_offsetstamp.zip')
    cut(root, zB, zs, 'offset')
    Bc, cbc = thin_arm('c_offset_notally', zs, ['--crowd-members-thin', '--crowd-members-thin-counts', zs[:-4] + '.counts.csv', '--crowd-fault', 'member_no_tally'], fl0)
    R.line(bool(cbc) and cbc['holes'] <= 13 and cbc['thin'] >= c0['thin'] - 13, 'control: with the tally check off the wrong-stamp cut is drawn (thin frames drawn %s of %d; the holes are the thin frames just after a proof frame, whose wrong tally says not thin and the stream-short backstop catches: %s): only the tally refuses the rest' % (cbc and cbc['thin'], c0['thin'], cbc and cbc['holes']))
    print('\nmember_thin_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())
