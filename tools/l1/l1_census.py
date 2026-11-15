r"""S102 lane L1: the pre-light's constants vs the character draws that read each cube, from the captures alone (0 game runs).
  py -I tools\l1\l1_census.py CAPTURE_DIR [FRAME ...]
Per frame: every pre-light draw (call site 0x82C20B90) is a group: (cube = the dest image of the copies after it, the draw's PS bank). For every draw whose fetch slot 13 holds a group's cube:
which effect it is, and whether its own PS registers (DifLghCol 0-3, DifLghVec 8-11, DifLghMax 197, DifLghMaxA 199) equal the group's (the skin reads them for its specular, so they may already be the same).
Also the cube image's format. Exit 1 when a capture cannot be read."""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import numpy as np
import shadow_census as sc

PRE = 0x82C20B90


def main():
    if len(sys.argv) < 2:
        print('FAIL usage: l1_census.py CAPTURE_DIR [FRAME ...]')
        return 1
    cap = sc.Cap(sys.argv[1])
    frames = [int(x) for x in sys.argv[2:]] or [60]
    for fi in frames:
        f = cap.frame(fi)
        d = f.draws
        o = f.origins
        cs = sc.pcap.queue_site(o)
        copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
        pre = [i for i in range(len(d)) if int(o[int(d[i]['origin'])]['call_site'] if 'origin' in d.dtype.names else 0) == PRE] if False else None
        # the draws of the pre-light call site
        pre = [i for i in range(len(d)) if int(cs[i]) == PRE]
        print('frame %d: %d draws, %d pre-light draws, %d copies' % (fi, len(d), len(pre), len(copies)))
        cubes = {}
        for c in copies:
            dd = int(c['d']['image'])
            rd = cap.imgs.get(dd)
            if rd is not None and int(rd['w']) == 64 and int(rd['layers']) == 6:
                cubes.setdefault(dd, int(rd['format']))
        print('  cube images %s (format byte %s)' % (sorted(cubes), sorted(set(cubes.values()))))
        # pair each pre-light draw with the cube its group's copies write: the next copies after it in op order are not indexed here; use the draws' order = group order
        groups = []
        for k, i in enumerate(pre):
            pb = f.bank(int(d[i]['ps_bank']))
            groups.append((i, pb))
        cube_ids = sorted(cubes)
        for k, (i, pb) in enumerate(groups):
            v = pb.view('<f4').reshape(-1, 4)
            print('  group %d draw %d: Vec %s ... Col0 %s MaxD %s MaxA %s Prm %s' % (k, i, np.round(v[8], 3), np.round(v[0], 3), np.round(v[198], 3), np.round(v[199], 3), [round(float(v[16 + j][0]), 3) for j in range(4)]))
            print('        LobeA %s LobeB %s' % (np.round(v[204:207], 3).tolist(), np.round(v[207:210], 3).tolist()))
        # readers
        tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
        ps_eff = cap.effs(d['ps_hash'])
        read = collections.Counter()
        for i in range(len(d)):
            for sl in f.slots[int(tf[i]):int(tf[i]) + int(tc[i])]:
                if int(sl['slot']) == 13 and int(sl['image']) in cubes:
                    pb = f.bank(int(d[i]['ps_bank'])).view('<f4').reshape(-1, 4)
                    eq = []
                    for gi, (gdraw, gpb) in enumerate(groups):
                        g = gpb.view('<f4').reshape(-1, 4)
                        same = np.array_equal(pb[0:4], g[0:4]) and np.array_equal(pb[8:12], g[8:12])
                        eq.append(same)
                    read[(int(sl['image']), str(ps_eff[i]), tuple(i for i, e in enumerate(eq) if e))] += 1
        for k, n in sorted(read.items()):
            print('  cube %d read by %s: %d draws; own Col/Vec regs equal group(s) %s' % (k[0], k[1], n, list(k[2])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
