"""Lane 135: what the sweat map (yLiquidMap) and yPreLight draws are in a capture frame: sites, layouts, vertex/index counts, targets, effects, texture slots. Numbers only."""
import sys, os, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import shadow_census as sc
import numpy as np
SITES = {0x8279A19C: 'paste_a', 0x82799CB4: 'paste_b', 0x82799DC8: 'paste_c', 0x82C20B90: 'prelight'}


def main():
    cap = sc.Cap(sys.argv[1])
    frames = [int(x) for x in sys.argv[2].split(',')]
    for fi in frames:
        f = cap.frame(fi)
        d = f.draws
        o = f.origins
        cs = o['call_site'].astype(np.int64)
        ps = o['pass_site'].astype(np.int64)
        print('frame', fi, 'draws', len(d))
        for i in range(len(d)):
            names = [n for s, n in SITES.items() if cs[i] == s or ps[i] == s]
            if not names:
                continue
            dr = d[i]
            stride, els = sc.layout_of(f, dr)
            print(' draw', i, names, 'cs %x ps %x' % (cs[i], ps[i]), 'obj %x part %d tech %x' % (o['object'][i], o['part'][i], o['technique'][i]),
                  'eff', cap.eff(int(dr['vs_hash'])), cap.eff(int(dr['ps_hash'])), 'vc', int(dr['vertex_count']), 'ic', int(dr['index_count']), 'stride', stride, 'els', els,
                  'prim', int(dr['source_prim']), 'idx32', int(dr['index_32bit']))


main()
