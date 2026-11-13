r"""S101 lane 275: the Arena Viewer's ring placements (assets::load_detail::PlaceRing, via out\tools\ring_place_dump.exe, built by tools\assets\build_ring_place.ps1) against a capture's ring-rows
records (the game's own world rows per ring instance). Offline, 0 game runs, reads the player's own bgNN.pac (for the join) and a capture.

  py -I tools\ring\ring_place_check.py CAPTURE --arena N [--tol-t 2.0] [--tol-r 0.002] [--plant NAME]

For the posts (951, palette node 3 -> rows[2]), the turnbuckles (952, both slots: rows[2] and rows[1]) and the covers (964, rows[2]) it compares, for EVERY recorded frame and every instance (call k
of its object = the viewer's copy k: 4 posts, 12 turnbuckles, 12 covers), the 3x4 world rows with the viewer's: rotation to --tol-r, translation to --tol-t units (the rope grid flexes the middle
turnbuckle / cover instances by up to ~1.5 units from frame to frame; the viewer holds the rest pose). Prints per piece: instances compared, max rotation error, max translation error, PASS / FAIL.
--plant old_buckle: the pre-lane-275 turnbuckle placement T(0, y, 0) * RotY(corner), which must FAIL (the past bug: pads floating beside the posts)."""
import argparse
import collections
import math
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ring'))
EXE = os.path.join(ROOT, 'out', 'tools', 'ring_place_dump.exe')
LABEL = {951: 'ring post', 952: 'ring turnbuckle', 964: 'ring corner cover'}
NODES = {951: (2,), 952: (2, 1), 964: (2,)}   # rows index of each palette slot (palette node - 1, the packer's r5 + 64 (n - 1))


def viewer_rows():
    out = subprocess.run([EXE], capture_output=True, text=True, check=True).stdout.splitlines()
    res = collections.defaultdict(list)
    for ln in out:
        t = ln.rsplit(' ', 12)
        label = ' '.join(t[0].split(' ')[:-1])
        res[label].append([float(x) for x in t[1:]])
    return res


def old_buckle():
    """the pre-lane-275 turnbuckle: T(0, y, 0) * RotY(i pi/2) in file space, column rows"""
    res = []
    for i in range(4):
        a = i * math.pi / 2
        c, s = math.cos(a), math.sin(a)
        for h in (43.5, 84.5, 126.5):
            y = -111.0 - h
            # R(a) column form [[c,0,s],[0,1,0],[-s,0,c]], translation (0,y,0) first then rotated: t' = (0, y, 0)
            res.append([c, 0, s, 0, 0, 1, 0, y, -s, 0, c, 0])
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--tol-t', type=float, default=2.0)
    ap.add_argument('--tol-r', type=float, default=0.002)
    ap.add_argument('--plant', default='', choices=('', 'old_buckle'))
    x = ap.parse_args()
    import ledger as L
    import ring_ledger as RL
    import ring_rows_check as RC
    cap = L.Capture(x.capture)
    joiner = RL.Joiner(x.arena)
    table = RC.build_table(cap.record(i) for i in range(cap.n))
    joined = {}
    for fi in range(cap.n):
        f = cap.frame(fi)
        by = collections.defaultdict(dict)
        for i in range(f.ndraws):
            o = f.origin(i)
            if o and o['klass'] == 1 and o['pass_site'] == RL.SITE_A:
                vc, ic = RL.draw_counts(f, i)
                if vc:
                    by[o['object']].setdefault(o['part'], (vc, ic))
        for ob, p in by.items():
            e = joiner.join(p)
            if e:
                joined[ob] = e
    vr = viewer_rows()
    if x.plant == 'old_buckle':
        vr['ring turnbuckle'] = old_buckle()
    bad = 0
    for entry in (951, 952, 964):
        objs = [o for o, e in joined.items() if e == (entry,)]
        mine = vr.get(LABEL[entry], [])
        n = 0
        mr = mt = 0.0
        missing = 0
        for g, fr in table.items():
            for ob in objs:
                for e in fr.get(ob, []):
                    k = e['call']
                    if k >= len(mine) or e['flags'] & (4 | 16) or e['n_nodes'] == 0:
                        missing += 1
                        continue
                    for node in NODES[entry]:
                        if node >= e['n_nodes']:
                            continue
                        cap_rows = e['rows'][node]   # 12 floats: 3 rows of (a b c d)
                        v = mine[k]
                        for row in range(3):
                            for col in range(3):
                                mr = max(mr, abs(cap_rows[4 * row + col] - v[4 * row + col]))
                            mt = max(mt, abs(cap_rows[4 * row + 3] - v[4 * row + 3]))
                    n += 1
        ok = n > 0 and mr <= x.tol_r and mt <= x.tol_t
        bad += 0 if ok else 1
        print('%-18s instances compared %5d (not comparable %d)  max rotation error %.4f  max translation error %.3f  %s' % (LABEL[entry], n, missing, mr, mt, 'PASS' if ok else 'FAIL'))
    print('ring_place_check: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
