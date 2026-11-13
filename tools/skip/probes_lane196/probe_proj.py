# S101 lane 196 scratch probe (read-only, numbers only; quoted in docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 and 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
endererSKIP_GAME_WORK_MAP.md sections 17 / 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
import sys, os, itertools, struct
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L
from ledger_io import CLASS_NAMES
import numpy as np
cap = L.Capture(sys.argv[1])
reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
fi = int(sys.argv[2])
f = cap.frame(fi)
def vecs(bank, r0, n):
    return np.array([struct.unpack('<4f', struct.pack('<4I', *bank[4 * r:4 * r + 4])) for r in range(r0, r0 + n)], dtype=np.float64)
maps = []
wr = []
for i in range(f.ndraws):
    o = f.origin(i)
    d = f.draw(i)
    vn = reg.variant_of(d['vs'])
    if o['pass_site'] == 0x82C04634 and vn:
        maps.append((i, o, vecs(f.bank(d['vs_bank']), 56, 4), vecs(f.bank(d['vs_bank']), 64, 4), vecs(f.bank(d['vs_bank']), 54, 1)))
    if o['pass_site'] == L.SITE_A and vn and vn.startswith('yCh_') and 68 in reg.read_regs(vn):
        wr.append((i, o, vecs(f.bank(d['vs_bank']), 68, 4), vecs(f.bank(d['vs_bank']), 166, 1)))
print('map draws', len(maps), 'wrestler draws w/ c68', len(wr))
uniq = {}
for (i, o, V, T, D) in maps:
    uniq.setdefault((V.tobytes(), T.tobytes()), (i, V, T, D))
print('distinct (ViewProj, Trapezoidal) in the map draws:', len(uniq))
wu = {}
for (i, o, P, E) in wr:
    wu.setdefault(P.tobytes(), (i, P, E))
print('distinct c68-71 in wrestler draws:', len(wu))
w, h = 512.0, 512.0
bias = np.array([[0.5, 0, 0, 0], [0, -0.5, 0, 0], [0, 0, 1, 0], [0.5 + 0.5 / w, 0.5 - 0.5 / h, 0, 1]])
best = []
for ku, (i, V, T, D) in uniq.items():
    for tv in (0, 1):
        for tt in (0, 1):
            Vm = V.T if tv else V
            Tm = T.T if tt else T
            for order in (0, 1):
                for bm in (bias, bias.T):
                    for tr in (0, 1):
                        A = Vm @ Tm if order else Tm @ Vm
                        P = bm @ A if True else None
                        for Pm in (P, P.T):
                            for (j, Pw, E) in wu.values():
                                err = np.abs(Pm - Pw).max()
                                best.append((err, i, tv, tt, order, tr, j))
best.sort()
print('best fit errors (smallest 5):', best[:5])
for (i, V, T, D) in uniq.values():
    print('map draw', i, 'V row3 / c54:', V[3], D[0])
for (j, P, E) in list(wu.values())[:3]:
    print('wrestler draw', j, 'c166', E[0])
