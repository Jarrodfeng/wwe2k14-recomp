#!/usr/bin/env python3
"""Session 102 lane T2 acceptance, offline, 0 game runs, no window: the far-card cut ON (--wwe2k14_rdr_card_cut; docs\\renderer\\SKIP_GAME_WORK_MAP.md 13.8).

  py -I tools\\crowd\\card_cut_check.py [--root DIR] [--no-build] [--captures bryan,checkB]

The cut itself and its picture proof are lane 175's (card_thin_check.py). This check adds what T2 needs:
  1. COUNTS on the Bryan capture with the cut emulated (card_thin_cut.py: the op-15 of every card but the first of each type is removed in every thin frame):
     stream draws per frame (all classes) before / after, the card draws among them, and OUR card draws unchanged (cards drawn thin arm == full arm, holes 0).
  2. THE FALLBACK. A frame ours has no scene record for:
       honest   the record of a FULL frame (one the hook recorded in full) is removed: ours falls back, the game's cards stay in the stream; binder fallback +K, holes 0.
       PLANT    the record of a THIN frame (a frame whose cards the hook dropped) is removed: nothing replaces the dropped cards. The invariant "a frame without our record keeps every card
                the game made" (computed from the zips alone: stream card draws of the frame in the cut zip == the original capture's) is violated on K frames and the binder must count K holes.
                The plant is CAUGHT only when both hold; it is the hole the gate (unproves at once, latches at 3 in 10 s) exists for.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import crowd_cards_check as ccc  # noqa: E402
import card_thin_check as ctc    # noqa: E402
import cards_census as cc        # noqa: E402


def stream_counts(zip_path):
    """-> {frame number: (all draws, card draws)} from the zip alone (card draws = draws whose call site is the card loop's)."""
    cap = cc.Cap(zip_path)
    out = {}
    for fi, num in enumerate(cap.nums):
        f = cap.frame(fi)
        pos, _ = __import__('card_thin_cut').card_runs(cap, f)
        out[num] = (int((f.ops['kind'] == 2).sum()), len(pos))   # draws the OP LIST replays (the cut edits the op list; the draw table keeps the dropped draws' rows)
    return out, cap


def without_records(zip_in, zip_out, drop_nums, tweak_nums=()):
    """A copy of the case zip without the scene records of the frames in drop_nums (a record file is named by its frame number); the records of tweak_nums keep existing but ONE card's world
    (the translation word of the record's first card object) is changed, so a record standing in for a missing one holds another card set."""
    cap = cc.Cap(zip_in)
    gone = {cap.scene[n] for n in drop_nums if n in cap.scene}
    if len(gone) != len(drop_nums):
        raise RuntimeError('a frame to strip has no scene record: %s' % sorted(set(drop_nums) - set(cap.scene)))
    tw = {cap.scene[n] for n in tweak_nums}
    zin = cap.c.zf
    with zipfile.ZipFile(zip_out, 'w', zipfile.ZIP_STORED, allowZip64=True) as zo:
        for info in zin.infolist():
            if info.filename in gone:
                continue
            data = zin.read(info.filename)
            if info.filename in tw:
                data = tweak_first_card(data)
            zo.writestr(info.filename, data)
    return len(gone)


def tweak_first_card(raw):
    import struct
    magic, ver, nsec = struct.unpack_from('<III', raw, 0)
    if magic != 0x4E435352 or ver != 1:   # S102 T19 F1: version-blind before; a version 2 record's crowd members are markers this reader cannot tweak
        raise ValueError('scene record: magic %08X version %d (this reader reads version 1)' % (magic, ver))
    secs = {}
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', raw, 40 + 24 * i)
        secs[sid] = (es, cnt, off)
    st = raw[secs[1][2]:secs[1][2] + secs[1][1]]
    es, cnt, off = secs[4]
    buf = bytearray(raw)
    for i in range(cnt):
        b = raw[off + es * i:off + es * (i + 1)]
        o = struct.unpack_from('<I', b, 12)[0]
        if o and st[o - 1:st.index(b'\0', o - 1)].startswith(b'crowd:card:'):
            buf[off + es * i + 16 + 12 * 4] ^= 0x01   # w[12] = column 3, row 0 of the world (part of the key the tally hashes)
            return bytes(buf)
    raise RuntimeError('no card object to tweak')


def model(nums, dec, kset, stripped, tweaked, check=True):
    """What the binder must do (crowd_ours.cpp InjectCards, F4) on a capture whose records in `stripped` are missing and whose records in `tweaked` hold another card set: -> (holes, previous-record draws).
    A thinned frame draws from its own record when that holds the frame's true card set (the tally's delivery check), else from the last record that gave a usable plan when THAT holds it, else it is a hole.
    check=False is the planted model: a stand-in is taken without the tally's proof."""
    last, holes, prev = None, 0, 0
    for f in nums:
        truth = kset(f - 1)
        if truth is None:
            continue
        own = None if (f - 1) in stripped else (('tweaked',) + tuple(truth) if (f - 1) in tweaked else truth)
        if own is not None and own == truth:
            last = own
            continue
        if dec[f] != 'thin':
            continue    # a full frame keeps the game's cards: no hole
        if last is not None and (last == truth or not check):
            prev += 1
        else:
            holes += 1
    return holes, prev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--captures', default='bryan')
    ap.add_argument('--strip', type=int, default=6, help='K: frames whose record is removed in the fallback arms')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = ccc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    zips = {n: os.path.join(main_co, p) for n, p in ctc.CAPTURES}
    names = args.captures.split(',')
    for need in [zips[n] for n in names] + [game]:
        if not os.path.exists(need):
            return ccc.fail_exit('input missing: ' + need)
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return ccc.fail_exit('fview build (exit %d)' % p.returncode)
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        return ccc.fail_exit('no fview.exe at ' + exe)
    work = os.path.join(root, 'out', 's102_t2')
    os.makedirs(work, exist_ok=True)
    R = ccc.Result()
    thin_extra = lambda z: ['--crowd-cards', '--crowd-cards-thin', '--crowd-cards-thin-counts', z[:-4] + '.counts.csv']
    for n in names:
        z = zips[n]
        zt = os.path.join(work, n + '_cut.zip')
        thin_frames, with_cards, dropped, kept = ctc.cut(root, z, zt)
        # --- 1. counts
        before, _ = stream_counts(z)
        after, capt = stream_counts(zt)
        fr = [k for k in before if before[k][1] > 0]
        tot_b = sum(before[k][0] for k in fr) / len(fr)
        tot_a = sum(after[k][0] for k in fr) / len(fr)
        card_b = sum(before[k][1] for k in fr) / len(fr)
        card_a = sum(after[k][1] for k in fr) / len(fr)
        A = ccc.Arm(exe, z, game, work, n + '_A', ['--crowd-cards'])
        B = ccc.Arm(exe, zt, game, work, n + '_B', thin_extra(zt))
        print('COUNT %-6s stream draws per card frame (%d frames): all classes %.0f -> %.0f (-%.0f), card draws %.0f -> %.0f; our card draws %s -> %s (instanced draws kept)' % (
            n, len(fr), tot_b, tot_a, tot_b - tot_a, card_b, card_a, A.cards and A.cards['drawn'], B.cards and B.cards['drawn']))
        sm = re.search(r'card thin stamp: offset 0 / 1 / 2 match (\d+) / (\d+) / (\d+) of (\d+) full frames', B.text)
        R.line(sm is not None and int(sm.group(4)) > 0 and int(sm.group(2)) == int(sm.group(4)), '%s: the stamp census line prints (final report, probe follows the switch) and offset 1 matches every full frame: %s' % (n, sm.group(0) if sm else 'MISSING'))
        m = ctc.THIN_RE.search(B.text)
        th = tuple(map(int, m.groups())) if m else (0, 0, 0, 0)
        R.line(A.cards is not None and B.cards is not None and A.cards['drawn'] > 0 and B.cards['drawn'] == A.cards['drawn'] and th[3] == 0 and thin_frames > 0 and tot_a < tot_b and B.valid == 0,
               '%s: the cut removes %d card draws over %d thin frames, our card draws unchanged (%s == %s), holes %d, validation %d' % (
                   n, dropped, thin_frames, B.cards and B.cards['drawn'], A.cards and A.cards['drawn'], th[3], B.valid))
        # --- 2. the fallback
        dec = {}
        with open(zt[:-4] + '.dec.csv') as fh:
            for line in fh:
                if line.startswith('#') or not line.strip():
                    continue
                f, kind, _, _ = line.strip().split(',')
                dec[int(f)] = kind
        scene = capt.scene
        # frame j is drawn from record j-1 (the live pairing); the previous usable record is j-2: strip the record of frame j = scene file j-1
        nums = sorted(dec)
        K = args.strip
        cap0 = cc.Cap(z)
        keyset = {}

        def kset(n):
            if n not in keyset:
                raw = cap0.record(n)
                keyset[n] = None if raw is None else sorted(k for _, _, k in cc.parse_record(raw)[0])
            return keyset[n]

        thin_ok = [j for j in nums if dec[j] == 'thin' and (j - 1) in scene and (j - 2) in scene and kset(j - 1) is not None and kset(j - 2) is not None]
        same = [j for j in thin_ok if kset(j - 1) == kset(j - 2)][3::3][:K]        # the previous record holds exactly this frame's card set
        diff = [j for j in thin_ok if kset(j - 1) != kset(j - 2)][3::3][:K]        # it does not
        fulls = [j for j in nums if dec[j] == 'full' and (j - 1) in scene][1:1 + K]
        for nm, js in (('same-set thin frames', same), ('different-set thin frames', diff), ('full frames', fulls)):
            if len(js) < 2:
                return ccc.fail_exit('%s: not enough %s to strip (%d)' % (n, nm, len(js)))
        prev_re = re.compile(r'previous record tried (\d+) drawn (\d+)')

        def arm(label, js, tweak, extra_name):
            zm = os.path.join(work, '%s_%s.zip' % (n, extra_name))
            without_records(zt, zm, [j - 1 for j in js], [j - 2 for j in js] if tweak else [])
            shutil.copy(zt[:-4] + '.counts.csv', zm[:-4] + '.counts.csv')
            M = ccc.Arm(exe, zm, game, work, n + '_' + extra_name, thin_extra(zm))
            mm = ctc.THIN_RE.search(M.text)
            mth = tuple(map(int, mm.groups())) if mm else (0, 0, 0, 0)
            pm = prev_re.search(M.text)
            prev = tuple(map(int, pm.groups())) if pm else (0, 0)
            viol = sum(1 for j in js if after[j][1] < before[j][1])
            fb_extra = (M.binder['fallback'] - B.binder['fallback']) if (M.binder and B.binder) else -1
            print('  %s: stripped %d records%s; stream invariant violated on %d frames; fallback +%d, holes %d, previous record tried %d drawn %d, cards drawn %s (cut arm %s)' % (
                label, len(js), ' and tweaked the previous ones' if tweak else '', viol, fb_extra, mth[3], prev[0], prev[1], M.cards and M.cards['drawn'], B.cards and B.cards['drawn']))
            os.remove(zm)
            os.remove(zm[:-4] + '.counts.csv')
            exp = model(nums, dec, kset, {j - 1 for j in js}, {j - 2 for j in js} if tweak else set())
            R.line((mth[3], prev[1]) == exp, '%s: %s: the binder\'s holes %d / previous-record draws %d equal the model\'s %s' % (n, label, mth[3], prev[1], exp))
            return M, mth, prev, viol, fb_extra
        # honest: full frames (the game kept its cards)
        M, mth, prev, viol, fb = arm('full frames (the game kept its cards)', fulls, False, 'miss_full')
        R.line(viol == 0 and mth[3] == 0 and fb >= len(fulls) and M.valid == 0,
               '%s: a FULL frame ours has no record for keeps the game\'s cards (stream card draws intact on %d of %d, fallback +%d, holes %d, validation %d)' % (n, len(fulls) - viol, len(fulls), fb, mth[3], M.valid))
        # F4: a THINNED frame with no record whose previous record holds this frame's card set: no hole, our card draws unchanged
        M, mth, prev, viol, fb = arm('same-set thin frames (F4)', same, False, 'miss_same')
        R.line(mth[3] == 0 and prev[1] == len(same) and M.cards['drawn'] == B.cards['drawn'] and M.valid == 0,
               '%s: F4 a thinned frame with no record takes the previous record (tally proves the set): holes %d, previous record drawn %d of %d, our card draws %s == %s, validation %d' % (
                   n, mth[3], prev[1], len(same), M.cards['drawn'], B.cards['drawn'], M.valid))
        # F4 PLANT: the same strip, one card world changed in each previous record: the delivery check must refuse it, every frame still a hole
        M, mth, prev, viol, fb = arm('PLANT same-set thin frames, previous record tweaked', same, True, 'miss_tweak')
        exp_wrong = model(nums, dec, kset, {j - 1 for j in same}, {j - 2 for j in same}, check=False)
        R.line(exp_wrong != (mth[3], prev[1]) and mth[3] >= len(same) and M.cards['drawn'] < B.cards['drawn'],
               '%s: PLANT caught: a stand-in whose card world differs is refused by the tally: holes %d (>= the %d stripped frames), previous record drawn %d; a binder with no delivery check would give %s, so the numbers tell them apart; cards drawn %s < %s' % (
                   n, mth[3], len(same), prev[1], exp_wrong, M.cards['drawn'], B.cards['drawn']))
        # a thinned frame whose previous record holds ANOTHER card set stays a hole (nothing is guessed)
        M, mth, prev, viol, fb = arm('different-set thin frames', diff, False, 'miss_diff')
        R.line(mth[3] == len(diff) and prev[1] == 0 and viol == len(diff),
               '%s: a thinned frame whose previous record holds another card set stays a HOLE: holes %d of %d, previous record drawn %d (invariant violated on %d)' % (n, mth[3], len(diff), prev[1], viol))
    print('\ncard_cut_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
    return 1 if R.fails else 0

if __name__ == '__main__':
    sys.exit(main())
