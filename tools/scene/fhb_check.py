#!/usr/bin/env python3
r"""Lane FHB check (Session 103): the foam hand binds its FILE picture when the record carries no picture row for it, offline (0 game runs).

  py -I tools\scene\fhb_check.py [--capture DIR] [--frames 20,110,200] [--out DIR]

Why (docs\research\s103\FH_foam_hand.md, run 38): on a walked (mirror) frame the record's sign / foam-hand rows had no usable picture row, so every hand drew the neutral 1x1 and was counted a hole
("visible sign / foam hand rows have no picture source yet", 24 of 24 mirror frames). The foam print is a FILE image (audience-set entry 42000 + h - 1 = the hand's mesh key + 1000, sub_8250B1E8), so the applier
(live_crowd.cpp FilePictureKey) loads it from the player's own arena pac at run time when the record has no valid row for the key. Never shipped, never written.

Rows are the d4_check ones (the capture's STREAM prop draws appended through the writer API, RELOCATED above stands people; picture 0 = no record row, the same as a mirror frame whose read failed). Bars:
  1. FILE     every foam-hand row drawn binds a real picture (the report's "file pictures N" == the hand rows drawn; neutral picture == the sign rows drawn), none is a neutral 1x1.
  2. COLOURS  the bound picture IS the game's: its hash (fnv over the DXT blocks, the rig's report) equals the hash of the capture's own image for the hand (slot 0 content of the stream draw, image 413 on
              brock_losswin; the byte join the S103 SG lane proved), and the hand's pixels differ from the neutral twin's (mean |diff| >= 3 / 255 inside the hand mask): the print is visible.
  3. HOLES    with the picture rule ON (no props_grey_ok) a hands-only record has 0 crowd holes (a hand has a picture source), and the planted twin no_file_pictures (the pre-FHB rule: no row, no picture)
              makes them holes again with the reason, and draws every hand with the neutral picture.
Planted twin no_file_pictures must FAIL bars 1 and 3. Exit 0 = every bar passed and the twin failed its bars."""
import argparse, collections, hashlib, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_rig as fr  # noqa: E402
import d4_check as d4  # noqa: E402

ROOT = fr.ROOT
sys.path[:0] = [os.path.join(ROOT, 'tools', 'solver'), os.path.join(ROOT, 'tools', 'wm30'), os.path.join(ROOT, 'tools', 'crowd')]
W, H = d4.W, d4.H


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--out', default=None)
    ap.add_argument('--image', type=int, default=413, help="the capture's image id of the foam hand's picture (brock_losswin: 413)")
    a = ap.parse_args()
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = a.capture or os.path.join(main_root, 'out', 'captures', 'brock_losswin')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('FHB', a.out)
    work = os.path.join(out, 'fhb_work')
    os.makedirs(work, exist_ok=True)
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    rig = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    frames = [int(x) for x in a.frames.split(',')]
    meshes = d4.oracle_meshes(game)
    fails = []

    # the frame with the most foam hand rows
    best, cams, rows = None, {}, {}
    for n in frames:
        cam, why = fr.stream_camera(fview, capture, n, game, work)
        if cam is None:
            print('FAIL  frame %d: no camera: %s' % (n, why))
            return 1
        cams[n] = cam
        rows[n] = d4.export_rows(capture, n, meshes)
        hands = sum(1 for k, _, _ in rows[n] if k == 2)
        print('INFO  frame %d: %d prop draws (%d foam hands, %d signs)' % (n, len(rows[n]), hands, len(rows[n]) - hands))
        if best is None or hands > best[1]:
            best = (n, hands)
    mid, n_hands = best
    if not n_hands:
        print('FAIL  no foam hand in the stream draws of frames %s' % frames)
        return 1
    n_signs = len(rows[mid]) - n_hands
    allf = os.path.join(work, 'props_all.txt')
    handf = os.path.join(work, 'props_hands.txt')
    with open(allf, 'w') as fa, open(handf, 'w') as fh:
        for kind, key, c in rows[mid]:
            line = 'P %d %d 0 %s\n' % (kind, key, ' '.join('%.9g' % x for x in c))
            fa.write(line)
            if kind == 2:
                fh.write(line)
    keys = sorted({k for kind, k, _ in rows[mid] if kind == 2})
    print('INFO  frame %d is the check frame: %d hands (mesh keys %s), %d signs' % (mid, n_hands, keys, n_signs))

    def rig_run(prefix, extra):
        vp, eye, _op, _M = cams[mid]
        cmd = [rig, '--record', os.path.join(capture, 'scene', 'frame_%06d.rsc' % mid), '--game', game, '--size', '%dx%d' % (W, H), '--prefix', prefix, '--families', '0x10',
               '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)] + list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1800)
        if p.returncode != 0:
            print('FAIL  rig exit %d: %s' % (p.returncode, (p.stderr or p.stdout)[-500:]))
            return None
        return open(prefix + '_report.txt').read()

    def counts(rep):
        m = re.search(r'props (\d+) \(drawn (\d+), hidden (\d+), skipped (\d+), in place (\d+), neutral picture (\d+)\)', rep)
        f = re.search(r'file pictures (\d+) \[([^\]]*)\]', rep)
        return (dict(listed=int(m.group(1)), drawn=int(m.group(2)), neutral=int(m.group(6))) if m else None,
                (int(f.group(1)), f.group(2)) if f else (0, ''))

    # ---- 1 / 2: the default rule (file pictures on), grey signs allowed so every row is drawn ----
    pre = os.path.join(work, 'on')
    rep = rig_run(pre, ['--props', allf, '--plant-crowd', 'only_props+props_grey_ok'])
    pre_t = os.path.join(work, 'twin')
    rep_t = rig_run(pre_t, ['--props', allf, '--plant-crowd', 'only_props+props_grey_ok+no_file_pictures'])
    if rep is None or rep_t is None:
        return 1
    c_on, (nf_on, txt_on) = counts(rep)
    c_tw, (nf_tw, txt_tw) = counts(rep_t)
    ok1 = bool(c_on) and nf_on == n_hands and c_on['neutral'] == n_signs and c_on['drawn'] == len(rows[mid])
    print('%s  bar 1 FILE: %d hand rows drawn with a file picture (want %d), neutral-picture rows %s (want %d signs), drawn %s of %d' % ('PASS' if ok1 else 'FAIL', nf_on, n_hands, c_on and c_on['neutral'], n_signs, c_on and c_on['drawn'], len(rows[mid])))
    if not ok1:
        fails.append('file pictures')
    # colours: the bound picture's hash is the capture's own image (the game's picture)
    blob = None
    try:
        import crowd_prop_keys as pk
        blob, info = pk.capture_blob(capture, a.image)
    except Exception as e:   # noqa: BLE001
        print('INFO  capture image %d unreadable: %s' % (a.image, e))
    m = re.search(r'(\d+) (\d+)x(\d+) fmt(\d+) fnv ([0-9A-F]{16})', txt_on)
    ok2 = False
    if blob is not None and m:
        w, h_, fmt = int(m.group(2)), int(m.group(3)), int(m.group(4))
        n = ((w + 3) // 4) * ((h_ + 3) // 4) * (8 if fmt == 18 else 16)
        want = '%016X' % fnv(blob[:n])
        ok2 = m.group(5) == want and len(blob) >= n
        print('%s  bar 2 COLOURS (bytes): file picture entry %s %sx%s fmt %s hash %s == the capture image %d (%d bytes) hash %s' % ('PASS' if ok2 else 'FAIL', m.group(1), w, h_, fmt, m.group(5), a.image, n, want))
    else:
        print('FAIL  bar 2 COLOURS: no file picture reported (%r) or no capture image %d' % (txt_on, a.image))
    if not ok2:
        fails.append('colour bytes')
    # colours: the picture shows in the pixels (the hand mask, on vs neutral twin)
    pre_h, pre_ht = os.path.join(work, 'hands_on'), os.path.join(work, 'hands_twin')   # the hands alone (signs would dilute the mean)
    if rig_run(pre_h, ['--props', handf, '--plant-crowd', 'only_props+props_grey_ok']) is None or rig_run(pre_ht, ['--props', handf, '--plant-crowd', 'only_props+props_grey_ok+no_file_pictures']) is None:
        return 1
    ca = np.asarray(Image.open(pre_h + '_mask16.png').convert('RGB'), dtype=np.int16)
    cb = np.asarray(Image.open(pre_ht + '_mask16.png').convert('RGB'), dtype=np.int16)
    mask = d4.lit(pre_h + '_mask16.png') & d4.lit(pre_ht + '_mask16.png')
    mean = float(np.abs(ca - cb).sum(axis=2)[mask].mean() / 3.0) if mask.any() else 0.0
    ok3 = mean >= 3.0
    print('%s  bar 2 COLOURS (pixels): the hands\' lit pixels differ from the neutral twin by %.1f / 255 on average (>= 3 needed; %d px)' % ('PASS' if ok3 else 'FAIL', mean, int(mask.sum())))
    if not ok3:
        fails.append('colour pixels')
    # ---- bar 4: the record wins over the file. (a) a record row carrying image 413's blob under the props' picture key renders byte-for-byte like the file arm and counts file pictures 0;
    # (b) a record row with different valid content (solid red BC1) renders differently and counts file pictures 0 ----
    if blob is not None and m:
        nb = ((w + 3) // 4) * ((h_ + 3) // 4) * 8
        fa_, fb_ = os.path.join(work, 'rec_413.bin'), os.path.join(work, 'rec_red.bin')
        open(fa_, 'wb').write(bytes(blob[:nb]))
        open(fb_, 'wb').write(bytes([0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0]) * (nb // 8))   # BC1 blocks: c0 = c1 = 0xF800 (red), all indices 0
        base = os.path.join(work, 'rec_a')
        rep_a = rig_run(base, ['--props', handf, '--prop-picture', '0x1234', '--picture-row', '0x1234,%d,%d,%d,%s' % (w, h_, fmt, fa_), '--plant-crowd', 'only_props+props_grey_ok'])
        rep_b = rig_run(os.path.join(work, 'rec_b'), ['--props', handf, '--prop-picture', '0x1234', '--picture-row', '0x1234,%d,%d,%d,%s' % (w, h_, fmt, fb_), '--plant-crowd', 'only_props+props_grey_ok'])
        if rep_a is None or rep_b is None:
            return 1
        c_a, (nf_a, _) = counts(rep_a)
        c_b, (nf_b, _) = counts(rep_b)
        img_file = np.asarray(Image.open(pre_h + '_mask16.png').convert('RGB'), dtype=np.int16)
        img_a = np.asarray(Image.open(base + '_mask16.png').convert('RGB'), dtype=np.int16)
        img_b = np.asarray(Image.open(os.path.join(work, 'rec_b') + '_mask16.png').convert('RGB'), dtype=np.int16)
        same_a = bool((img_file == img_a).all())
        okA = same_a and nf_a == 0 and bool(c_a) and c_a['neutral'] == 0
        print('%s  bar 4a RECORD (same bytes): a record row with image %d blob renders byte-for-byte like the file arm (equal %s), file pictures %d, neutral %s' % ('PASS' if okA else 'FAIL', a.image, same_a, nf_a, c_a and c_a['neutral']))
        diff_b = float(np.abs(img_file - img_b).sum(axis=2)[mask].mean() / 3.0) if mask.any() else 0.0
        okB = diff_b >= 3.0 and nf_b == 0 and bool(c_b) and c_b['neutral'] == 0
        print('%s  bar 4b RECORD wins: a record row of different valid content (solid BC1) renders differently (%.1f / 255 on the hands), file pictures %d, neutral %s' % ('PASS' if okB else 'FAIL', diff_b, nf_b, c_b and c_b['neutral']))
        if not okA:
            fails.append('record same')
        if not okB:
            fails.append('record wins')
    # the twin must fail bar 1
    caught1 = nf_tw == 0 and bool(c_tw) and c_tw['neutral'] == len(rows[mid])
    print('%s  planted twin no_file_pictures: file pictures %d, neutral %s of %d -> %s' % ('PASS' if caught1 else 'FAIL', nf_tw, c_tw and c_tw['neutral'], len(rows[mid]), 'caught' if caught1 else 'NOT caught'))
    if not caught1:
        fails.append('twin 1')

    # ---- 3: the picture rule ON (no props_grey_ok), hands only: no holes; the twin: holes with the reason ----
    pre = os.path.join(work, 'rule')
    rep = rig_run(pre, ['--props', handf, '--plant-crowd', 'only_props'])
    pre_t = os.path.join(work, 'rule_twin')
    rep_t = rig_run(pre_t, ['--props', handf, '--plant-crowd', 'only_props+no_file_pictures'])
    if rep is None or rep_t is None:
        return 1
    holes = [int(x) for x in re.findall(r'family crowd[^\n]*holes (\d+)', rep)]
    holes_t = [int(x) for x in re.findall(r'family crowd[^\n]*holes (\d+)', rep_t)]
    ok4 = bool(holes) and max(holes) == 0 and 'have no picture source' not in rep
    print('%s  bar 3 HOLES: hands only, picture rule on: crowd holes %s (want 0)' % ('PASS' if ok4 else 'FAIL', holes))
    caught3 = bool(holes_t) and max(holes_t) >= 1 and 'have no picture source' in rep_t
    print('%s  planted twin no_file_pictures: crowd holes %s with the reason -> %s' % ('PASS' if caught3 else 'FAIL', holes_t, 'caught' if caught3 else 'NOT caught'))
    if not ok4:
        fails.append('holes')
    if not caught3:
        fails.append('twin 3')
    print('fhb_check: %s' % ('PASS' if not fails else 'FAIL ' + ', '.join(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
