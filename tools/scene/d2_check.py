#!/usr/bin/env python3
r"""Lane D2 check (Session 103): the crowd through the class table, offline (0 game runs).

  py -I tools\scene\d2_check.py [--capture DIR] [--frames 20,110,200] [--out DIR]

Per frame, with the stream's own camera (frame_rig.stream_camera: offline only): frame_rig.exe --families 0x10 (the crowd class alone through BuildFrame + the crowd hook).
  1. ALONE: each frame in its own process.  2. SEQUENCE: the three frames in ONE process (--also-record: same front-end, cache and crowd).
Bars: validation errors 0; members and cards drawn > 0 and equal the visible objects of the record; misses 0; every frame of the sequence is byte-identical to its lone draw
(no cross-frame state); the crowd is in the picture (lit pixels > 1500).
Planted twins (each must FAIL its bar): --plant-crowd palettes (the pose dropped: the picture changes >= 5 % of the lit pixels), cards_black (the cards' pictures cleared:
>= 20 % lit pixels lost), inst_first (every instance at the first world: lit area < 60 %).  Exit 0 = all bars passed and all twins failed their bar.
"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_rig as fr  # noqa: E402

ROOT = fr.ROOT


def img(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def lit(a, clear):
    return (np.abs(a - clear).sum(axis=2) > 12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--out', default=None)
    ap.add_argument('--size', default='1280x720')
    a = ap.parse_args()
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = a.capture or os.path.join(main_root, 'out', 'captures', 'brock_losswin')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('D2', a.out)
    work = os.path.join(out, 'd2_work')
    os.makedirs(work, exist_ok=True)
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')   # (fview only reads the stream's camera here: a stale build is fine)
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    rig = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    frames = [int(x) for x in a.frames.split(',')]
    cams = {}
    for n in frames:
        cam, why = fr.stream_camera(fview, capture, n, game, work)
        if cam is None:
            print('FAIL  frame %d: no camera: %s' % (n, why))
            return 1
        cams[n] = cam
    fails = []

    def args_for(n):
        vp, eye = cams[n][0], cams[n][1]
        return ['--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)]

    def rsc(n):
        return os.path.join(capture, 'scene', 'frame_%06d.rsc' % n)

    def rig_run(prefix, n, extra=(), also=()):
        cmd = [rig, '--record', rsc(n), '--game', game, '--size', a.size, '--prefix', prefix, '--families', '0x10'] + args_for(n) + list(extra)
        for m in also:
            cmd += ['--also-record', rsc(m), '--also-camera', ','.join('%.9g' % x for x in cams[m][0]), '--also-eye', ','.join('%.9g' % x for x in cams[m][1])]
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1800)
        if p.returncode != 0:
            print('FAIL  rig exit %d: %s' % (p.returncode, (p.stderr or p.stdout)[-500:]))
            return None
        return open(prefix + '_report.txt').read()

    def crowd_lines(rep):
        return [l.strip() for l in rep.splitlines() if l.strip().startswith('crowd:') or re.match(r'also \d+ crowd:', l.strip()) or l.strip().startswith('also') and 'crowd:' in l]

    def full_of(line):
        m = re.search(r'record (\d+) .*?members drawn (\d+) .*?skipped (\d+); cards drawn (\d+) skipped (\d+);.*?refused (\d+); misses (\d+); hard failures (\d+).*?CPU pose (\d+) verts (\d+) us', line)
        return dict(zip(('record', 'members', 'm_skip', 'cards', 'c_skip', 'refused', 'misses', 'hard', 'verts', 'pose_us'), (int(x) for x in m.groups()))) if m else None

    def counts_of(line):
        m = re.search(r'members drawn (\d+).*?cards drawn (\d+).*?misses (\d+)', line)
        return tuple(int(x) for x in m.groups()) if m else None

    # 1. alone
    alone, alone_counts = {}, {}
    for n in frames:
        pre = os.path.join(work, 'alone_%d' % n)
        rep = rig_run(pre, n)
        if rep is None:
            return 1
        alone[n] = img(pre + '_mask16.png')
        cl = [l for l in rep.splitlines() if 'crowd: record' in l]
        alone_counts[n] = counts_of(cl[0]) if cl else None
        verr = [int(x) for x in re.findall(r'validation_errors (\d+)', rep)]
        fu = full_of(cl[0]) if cl else None
        ok = bool(fu) and fu['members'] > 0 and fu['misses'] == 0 and bool(verr) and max(verr) == 0
        # S103 D2 (review 6): nothing refused or skipped, and everything the record lists is drawn; the CPU pose is printed and barred
        ok = ok and fu['refused'] == 0 and fu['m_skip'] == 0 and fu['c_skip'] == 0 and fu['hard'] == 0 and fu['members'] + fu['cards'] == fu['record'] and fu['pose_us'] < 5000
        print('%s  frame %d alone: %s' % ('PASS' if ok else 'FAIL', n, (cl[0].strip() + ' [bars: refused 0, skipped 0, drawn == record, pose < 5000 us]') if cl else 'no crowd line'))
        if not ok:
            fails.append('alone %d' % n)
    clear = alone[frames[0]][0, 0]
    # 2. the sequence, one process
    first = frames[0]
    pre = os.path.join(work, 'seq')
    rep = rig_run(pre, first, also=frames[1:])
    if rep is None:
        return 1
    seq = {first: img(pre + '_mask16.png')}
    for i, n in enumerate(frames[1:]):
        seq[n] = img(pre + '_also%d.png' % i)
    seq_lines = [l.strip() for l in rep.splitlines() if 'crowd: record' in l]
    for i, n in enumerate(frames):
        same = np.array_equal(seq[n], alone[n])
        diff = int((np.abs(seq[n] - alone[n]).sum(axis=2) > 0).sum())
        c = counts_of(seq_lines[i]) if i < len(seq_lines) else None
        ok = same and c == alone_counts[n]
        print('%s  frame %d in one process (%d of %d): identical to its lone draw %s (%d pixels differ), counts %s vs %s' % ('PASS' if ok else 'FAIL', n, i + 1, len(frames), same, diff, c, alone_counts[n]))
        if not ok:
            fails.append('sequence %d' % n)
    lit110 = int(lit(alone[frames[len(frames) // 2]], clear).sum())
    ok = lit110 > 1500
    print('%s  the crowd is in the picture: %d lit pixels (frame %d)' % ('PASS' if ok else 'FAIL', lit110, frames[len(frames) // 2]))
    if not ok:
        fails.append('lit')
    # 2b. the live path's crowd: bodies on a worker thread. The first build must be a MISS (the frame stays the game's), later ones whole, and the picture the same as the sync one.
    mid = frames[len(frames) // 2]
    pre = os.path.join(work, 'async')
    rep = rig_run(pre, mid, extra=['--async-crowd'])
    if rep is None:
        return 1
    m = re.search(r'async crowd: first build missed (\d+), (\d+) builds were a miss', rep)
    t = img(pre + '_mask16.png')
    cl = [l.strip() for l in rep.splitlines() if 'crowd: record' in l]
    ok = bool(m) and int(m.group(1)) > 0 and int(m.group(2)) >= 1 and np.array_equal(t, alone[mid]) and bool(cl) and counts_of(cl[-1]) == alone_counts[mid]
    print('%s  async bodies (the live path): %s; picture identical to the sync one %s' % ('PASS' if ok else 'FAIL', m.group(0) if m else 'no async line', np.array_equal(t, alone[mid])))
    if not ok:
        fails.append('async')
    # 2c. an ARENA CHANGE in one process (async bodies): the same record naming another arena's pac after the first. The old arena's crowd and stored model are released, the
    #     new arena's classes load on the worker; the frame is a miss until then, then whole with the record's counts.
    pre = os.path.join(work, 'rearena')
    rep = rig_run(pre, mid, extra=['--async-crowd', '--also-rearena', '5'], also=[mid])
    if rep is None:
        return 1
    cl = [l.strip() for l in rep.splitlines() if 'crowd: record' in l]
    fu = full_of(cl[-1]) if cl else None
    ok = bool(fu) and len(cl) >= 2 and fu['misses'] == 0 and fu['hard'] == 0 and fu['refused'] == 0 and fu['members'] + fu['cards'] == fu['record'] and counts_of(cl[-1]) == alone_counts[mid]
    print('%s  arena change (arena 4 -> 5, async): %s' % ('PASS' if ok else 'FAIL', cl[-1] if cl else 'no crowd line'))
    if not ok:
        fails.append('arena change')
    # 3. the planted twins on the middle frame
    base = alone[mid]
    base_lit = max(1, int(lit(base, clear).sum()))
    for plant, bar_name, bar in (('palettes', 'pose dropped changes >= 5 % of the lit pixels', lambda t: int((np.abs(t - base).sum(axis=2) > 12).sum()) / base_lit >= 0.05),
                                 ('cards_black', 'cards cleared loses >= 20 % of the lit pixels', lambda t: 1 - int(lit(t, clear).sum()) / base_lit >= 0.20),
                                 ('convert_off', 'space rule off (the game-space camera not restored) changes >= 5 % of the lit pixels', lambda t: int((np.abs(t - base).sum(axis=2) > 12).sum()) / base_lit >= 0.05),
                                 ('inst_first', 'instances collapsed leave < 60 % of the lit area', lambda t: int(lit(t, clear).sum()) / base_lit < 0.60)):
        pre = os.path.join(work, 'plant_%s' % plant)
        rep = rig_run(pre, mid, extra=['--plant-crowd', plant])
        if rep is None:
            fails.append('plant %s did not run' % plant)
            continue
        t = img(pre + '_mask16.png')
        caught = bar(t)
        print('%s  planted twin %s: %s -> %s' % ('PASS' if caught else 'FAIL', plant, bar_name, 'caught' if caught else 'NOT caught (the check is blind to it)'))
        if not caught:
            fails.append('plant ' + plant)
    print('d2_check: %s' % ('PASS' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
