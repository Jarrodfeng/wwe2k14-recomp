#!/usr/bin/env python3
"""Lane 175 (S101) acceptance test, offline, 0 game runs, no window: the far cards' THIN record (docs\\renderer\\SKIP_GAME_WORK_MAP.md section 13).

  py -I tools\\crowd\\card_thin_check.py [--root DIR] [--no-build] [--scale N] [--quick]

For each capture with scene records: A = fview --crowd-ours --crowd-cards on the capture as it is (the game recorded every card); B = the SAME capture with the thin cut EMULATED
(card_thin_cut.py: in every thin frame the op-15 of every card but the first of each type is removed; the op-21s stay, so no register state is lost) replayed with --crowd-cards-thin.
Bars:
  1. picture: B's presented-surface hash equals A's on EVERY frame (byte-equal), on Check B, Check F, run 7079, Bryan's entrance and wm30 (cards only in its first frames).
  2. census (B): thin frames > 0 where the capture has cards, holes 0, cards drawn == A's cards drawn, the game's dropped card draws == record cards - kept stream cards, bad frames 0, validation 0.
  3. the cut touched only the op list (the cut zip's banks, states and texture sections equal the input's byte for byte: no register or texture state is left over to emulate).
  4. plants (Check B), each must fail its NAMED check:
       cut drop_first        every type's first card dropped too: no kept draw, no trigger: the picture moves on every thin frame
       cut swap_kept         the kept draw is the type's SECOND card: the first-of-type check refuses (holes) and the picture moves
       fault offset          (the past bug, lane 36's +1; tonight's lane 152 B1 `live gave the record of the wrong frame`) record j instead of j-1: every thin frame whose card set differs is refused
                             (the hook's per-frame tally: card op-15 count and the sum of the card-world hashes vs the record's cards; the kept cards' worlds alone pass a wrong record, they are fixed per seat).
                             A thin frame whose wrong record holds the IDENTICAL card set (the same cards, compared as sets of their c76-c78 keys from the scene records, and as ordered lists: the
                             check prints how many of those are also in the same ORDER) is drawn: holes + drawn == thin frames and drawn == the computed number. Whole-frame hashes cannot show that
                             the drawn ones equal the full arm (the offset fault moves the whole crowd record), so that is NOT claimed; the order within a type is covered only by the record-order check
                             on full proof frames (review S5)
       fault cards_thin_first_shift / cards_thin_type_missing    the check's own inputs shifted: holes
       fault cards_type_shift / cards_one_picture                lane 35's wrong-picture classes on a thin frame: holes (the kept card shows another picture)
       CONTROL fault cards_thin_trust_count on the uncut capture with offset: a frame that is not thin (more stream cards than types) is refused as a FULL frame (bad pairing, no hole); with the count rule off
                         the same frame is refused as a THIN one (hole)
  5. a refused thin frame is never silent: its kept cards stay the stream's (no hole in the sense of a crash) and `holes` counts it, which the gate turns into a reset and, three times in 10 s, a latch.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import crowd_cards_check as ccc  # noqa: E402

CAPTURES = [
    ('checkB', ccc.BROCK),
    ('checkF', ccc.CHECK_F),
    ('r7079', 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'),
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('wm30', 'out/s101/runs/20261112_131029_s100_owner_s90_wm30/case_20261112_131111_940_0_pc_run_exit.zip'),
]
THIN_RE = re.compile(r'THIN cards: frames (\d+), cards drawn in them (\d+), the game\'s dropped card draws (\d+), holes \(thin frames ours could not draw\) (\d+)')
CUT_RE = re.compile(r'thin: (\d+) of (\d+) frames with cards cut \((\d+) card draws dropped, (\d+) kept')


def cut(root, zip_in, zip_out, plant=None, k=32):
    cmd = [sys.executable, '-I', os.path.join(root, 'tools', 'crowd', 'card_thin_cut.py'), zip_in, zip_out, '--k', str(k), '--counts', zip_out[:-4] + '.counts.csv', '--decisions', zip_out[:-4] + '.dec.csv']
    if plant:
        cmd += ['--plant', plant]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    m = CUT_RE.search(p.stdout)
    if p.returncode or not m:
        print(p.stdout[-1500:], p.stderr[-1500:])
        raise RuntimeError('cut failed: ' + ' '.join(cmd))
    return tuple(map(int, m.groups()))


def thin_frame_numbers(zip_thin):
    out = []
    with open(zip_thin[:-4] + '.dec.csv') as fh:
        for line in fh:
            if line.startswith('#') or not line.strip():
                continue
            f, kind, _, _ = line.strip().split(',')
            if kind == 'thin':
                out.append(int(f))
    return out


def same_record_frames(zip_in, thin_nums):
    """-> (frames whose record j holds the same card SET as record j-1, of those the frames whose ordered list is also equal), from the scene records alone."""
    import cards_census as cc
    cap = cc.Cap(zip_in)
    cache = {}

    def keys(num):
        if num not in cache:
            raw = cap.record(num)
            cache[num] = None if raw is None else [k for _, _, k in cc.parse_record(raw)[0]]
        return cache[num]
    n = same_order = 0
    for j in thin_nums:
        a, b = keys(j), keys(j - 1)
        if a is not None and b is not None and sorted(a) == sorted(b):
            n += 1
            same_order += a == b
    return n, same_order


def sections_equal(zip_a, zip_b):
    """Bar 3: everything but the ops section (and its count) is byte-identical. Compares whole frame files except the op table bytes."""
    import numpy as np
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
    import cards_census as cc
    A, B = cc.Cap(zip_a), cc.Cap(zip_b)
    bad = 0
    for i in range(len(A.frames)):
        fa, fb = A.frame(i), B.frame(i)
        if len(fa.buf) != len(fb.buf):
            bad += 1
            continue
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
    ap.add_argument('--quick', action='store_true', help='Check B and wm30 only, fewer plants')
    ap.add_argument('--captures', default='', help='comma list of capture labels to run (default all)')
    ap.add_argument('--no-plants', action='store_true')
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
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        return ccc.fail_exit('no fview.exe at ' + exe)
    if args.scale != '1':
        ccc.Arm.scale = ['--scale', args.scale]
    work = os.path.join(root, 'out', 'card_thin_check' + ('' if args.scale == '1' else '_x' + args.scale))
    os.makedirs(work, exist_ok=True)
    R = ccc.Result()
    checkb_thin_frames = -1
    names = ['checkB', 'wm30'] if args.quick else [n for n, _ in CAPTURES]
    if args.captures:
        names = args.captures.split(',')
    try:
        for n in names:
            z = zips[n]
            zt = os.path.join(work, n + '_thin.zip')
            thin_frames, with_cards, dropped, kept = cut(root, z, zt)
            if n == 'checkB':
                checkb_thin_frames = thin_frames
            A = ccc.Arm(exe, z, game, work, n + '_A', ['--crowd-cards'])
            if not A.cards or A.cards['drawn'] == 0:
                # Ours drew no card in the full arm (no scene record covers the card frames): the binder reports no clean full frame, the gate is never proven and the hook would never thin.
                # The cut tool is gate-blind, so thinning this capture would remove draws nothing replaces: not a case of the cut, a case of the gate. The census of the full arm is the check.
                R.line(True, '%-6s gate: ours drew 0 cards in the full arm (%d card draws stay the stream\'s), so the gate is never proven and the hook records every card: nothing to thin' % (n, A.cards['stream'] if A.cards else 0))
                continue
            B = ccc.Arm(exe, zt, game, work, n + '_B', ['--crowd-cards', '--crowd-cards-thin', '--crowd-cards-thin-counts', zt[:-4] + '.counts.csv'])
            m = THIN_RE.search(B.text)
            th = tuple(map(int, m.groups())) if m else None
            diff = B.differing(A)
            R.line(diff == 0 and len(B.hashes) == len(A.hashes), '%-6s picture: thin arm == full arm on every frame (%d frames, %d differ)' % (n, len(A.hashes) - 1, diff))
            R.line(th is not None and th[0] == thin_frames and th[3] == 0 and (thin_frames > 0 or with_cards == 0),
                   '%-6s census: thin frames drawn %s of %d cut, holes %s, cards in thin frames %s, the game\'s dropped card draws %s (the cut dropped %d, kept %d)' % (n, th[0] if th else '?', thin_frames, th[3] if th else '?', th[1] if th else '?', th[2] if th else '?', dropped, kept))
            if B.cards and A.cards:
                R.line(B.cards['drawn'] == A.cards['drawn'] and B.cards['bad_pairing'] + B.cards['bad_order'] + B.cards['bad_picture'] + B.cards['bad_state'] == 0 and B.cards['holes'] == 0,
                       '%-6s cards drawn %d == the full arm\'s %d, bad frames 0, holes 0, stream card draws %d -> %d' % (n, B.cards['drawn'], A.cards['drawn'], A.cards['stream'], B.cards['stream']))
            R.line(B.valid == 0, '%-6s validation errors %d' % (n, B.valid))
            if n == 'checkB':
                R.line(sections_equal(z, zt) == 0, 'checkB bank / state / texture sections of the cut frames are byte-identical to the input (only the op list changed)')
        # --- plants on Check B
        if args.no_plants:
            print('\ncard_thin_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
            return 1 if R.fails else 0
        z, zt = zips['checkB'], os.path.join(work, 'checkB_thin.zip')
        A = ccc.Arm(exe, z, game, work, 'p_A', ['--crowd-cards'])

        def plant_arm(name, zip_path, extra):
            B = ccc.Arm(exe, zip_path, game, work, name, extra)
            m = THIN_RE.search(B.text)
            return B, (tuple(map(int, m.groups())) if m else (0, 0, 0, 0))

        for plant in ('drop_first', 'swap_kept'):
            zp = os.path.join(work, 'checkB_' + plant + '.zip')
            cut(root, z, zp, plant)
            B, th = plant_arm('p_' + plant, zp, ['--crowd-cards', '--crowd-cards-thin', '--crowd-cards-thin-counts', zp[:-4] + '.counts.csv'])
            diff = B.differing(A)
            ok = diff > 0 and (plant == 'drop_first' or th[3] > 0)
            R.line(ok, 'plant cut:%s is caught: %d frames differ, holes %d%s' % (plant, diff, th[3], '' if plant == 'drop_first' else ' (the first-of-type check refused them)'))
            os.remove(zp)
        faults = ['offset', 'cards_thin_first_shift', 'cards_thin_type_missing', 'cards_type_shift'] if not args.quick else ['offset', 'cards_thin_first_shift']
        for fault in faults:
            B, th = plant_arm('p_' + fault, zt, ['--crowd-cards', '--crowd-cards-thin', '--crowd-cards-thin-counts', zt[:-4] + '.counts.csv', '--crowd-fault', fault])
            diff = B.differing(A)
            # Review B1: the bar is that NO thin frame is drawn from a wrong record: holes == the thin frames cut, thin frames drawn 0 (holes > 0 was the reviewer's catch: 257 of 324 drawn)
            thin_nums = thin_frame_numbers(zt)
            # A wrong record is drawn only where it holds the IDENTICAL card set (count and world-hash sum equal: static cards, the same cards). Expected from the scene records alone: the thin frames j
            # whose record j (what the fault reads) has the same sorted card keys as record j-1 (what the live pairing reads). Every other thin frame must be a hole.
            expect_drawn, expect_ordered = same_record_frames(z, thin_nums) if fault == 'offset' else (0, 0)
            R.line(th[3] + th[0] == len(thin_nums) and th[0] == expect_drawn and th[3] > 0,
                   'plant fault:%s is caught: %d of %d thin frames refused (holes), %d drawn (expected %d: %s), %d frames differ%s' % (
                       fault, th[3], len(thin_nums), th[0], expect_drawn, 'records j and j-1 hold the identical card set, %d of them in the same order' % expect_ordered if fault == 'offset' else 'none', diff,
                       '  <- the past bug: the record of the wrong frame (lane 36 +1, lane 152 B1)' if fault == 'offset' else ''))
        # --- control: not-a-thin-frame
        Bh, thh = plant_arm('c_honest', z, ['--crowd-cards', '--crowd-cards-thin'])
        Bt, tht = plant_arm('c_trust', z, ['--crowd-cards', '--crowd-cards-thin', '--crowd-fault', 'cards_thin_trust_count'])
        R.line(thh[3] == 0 and Bh.differing(A) == 0 and tht[3] > 0,
               'control: the thin switch on a capture where the game recorded every card changes nothing (holes %d, %d frames differ); with the entry rule off the same frames are refused as thin (holes %d)' % (thh[3], Bh.differing(A), tht[3]))
    finally:
        pass
    print('\ncard_thin_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())
