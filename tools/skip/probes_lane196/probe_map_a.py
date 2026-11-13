# S101 lane 196 scratch probe (read-only, numbers only; quoted in docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 and 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
endererSKIP_GAME_WORK_MAP.md sections 17 / 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
import sys, os, collections
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L
from ledger_io import CLASS_NAMES
cap = L.Capture(sys.argv[1])
reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
res = collections.defaultdict(collections.Counter)
for fi in range(int(sys.argv[2]), int(sys.argv[3])):
    f = cap.frame(fi)
    rec = cap.record(fi - 1)
    ri = L.RecordIndex(rec)
    colour = collections.defaultdict(set)
    for i in range(f.ndraws):
        o = f.origin(i)
        d = f.draw(i)
        if CLASS_NAMES[o['klass']] == 'model_part' and o['pass_site'] in (L.SITE_A, L.SITE_B):
            vb = f.bank(d['vs_bank'])
            for r in range(76, 139):
                colour[(o['object'], r)].add(tuple(vb[4 * r:4 * r + 4]))
    seen = collections.Counter()
    for i in range(f.ndraws):
        o = f.origin(i)
        d = f.draw(i)
        if o['pass_site'] != 0x82C04634:
            continue
        k = (o['object'], o['part'])
        ordn = seen[k]
        seen[k] += 1
        vb = f.bank(d['vs_bank'])
        for r in range(76, 139):
            v = tuple(vb[4 * r:4 * r + 4])
            if set(v) <= L.TRIVIAL:
                continue
            src = v in colour[(o['object'], r)] or ri.lookup(v)
            res[ordn]['reads'] += 1
            if not src:
                res[ordn]['NONE'] += 1
for k in sorted(res):
    print('ordinal', k, dict(res[k]), 'NONE %.1f%%' % (100 * res[k]['NONE'] / max(1, res[k]['reads'])))
