# S101 lane 196 scratch probe (read-only, numbers only; quoted in docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 and 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
endererSKIP_GAME_WORK_MAP.md sections 17 / 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
import sys, os, collections, struct
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L
from ledger_io import CLASS_NAMES
cap = L.Capture(sys.argv[1])
reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
frames = list(range(int(sys.argv[2]), int(sys.argv[3])))
lfi = L.LightFrameIndex(cap.record(i) for i in range(cap.n))
off = int(sys.argv[4]); votes = None
print('light offset', off)
tot = collections.Counter()
hit = collections.Counter()
phit = collections.Counter()
for fi in frames:
    f = cap.frame(fi)
    rec = cap.record(fi - 1)
    # shadow_x candidates
    xs = set()
    for s in rec.light_states():
        for xk in s['shadow_x']:
            for c in range(2):
                xs.add(tuple(struct.unpack('<4I', struct.pack('<4f', *xk[4 * c:4 * c + 4]))))
    for i in range(f.ndraws):
        o = f.origin(i)
        if CLASS_NAMES[o['klass']] != 'model_part' or o['pass_site'] not in (L.SITE_A,):
            continue
        d = f.draw(i)
        vn = reg.variant_of(d['vs'])
        if not vn or not vn.startswith('yCh_'):
            continue
        pn = reg.variant_of(d['ps']) if d['ps'] else None
        for stage, name, bank in (('v', vn, f.bank(d['vs_bank'])), ('p', pn, f.bank(d['ps_bank']) if d['ps'] else None)):
            if not name:
                continue
            for r in sorted(reg.read_regs(name)):
                if (68 <= r <= 71) or (164 <= r <= 171) or r in (54, 64, 65, 66, 67):
                    v = tuple(bank[4 * r:4 * r + 4])
                    if set(v) <= L.TRIVIAL:
                        continue
                    k = (stage, r, L.reg_name(stage, r))
                    tot[k] += 1
                    if lfi.match(f.guest_frame - off, o['object'], stage, r, v):
                        hit[k] += 1
                    elif v in xs:
                        phit[k] += 1
for k in sorted(tot):
    print(k, 'reads', tot[k], 'lstate rows', hit[k], 'shadow_x vec', phit[k])
