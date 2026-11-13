# S101 lane 196 scratch probe (read-only, numbers only; quoted in docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 and 18). Run from the repo root: py -I THIS CASE_FOLDER ARGS
import sys, os, collections, struct
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L
cap = L.Capture(sys.argv[1])
reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
fi = int(sys.argv[2])
f = cap.frame(fi)
rec = cap.record(fi - 1)
print('states', len(rec.light_states()), 'refs', len(rec.light_refs()))
# all matrices / vectors available in the record states, as bit tuples, tagged
tags = {}
def add(tag, floats):
    b = struct.unpack('<%dI' % len(floats), struct.pack('<%df' % len(floats), *floats))
    tags.setdefault(b, tag)
for si, s in enumerate(rec.light_states()):
    for k in range(3):
        m = s['shadow_m'][k]
        for c in range(4):
            tags.setdefault(tuple(struct.unpack('<4I', struct.pack('<4f', *m[c::4]))), ('shadow_m%d' % k, 'col', c))
            tags.setdefault(tuple(struct.unpack('<4I', struct.pack('<4f', *m[4*c:4*c+4]))), ('shadow_m%d' % k, 'row', c))
    sf = s['shadow_f']
    for c in range(4):
        tags.setdefault(tuple(struct.unpack('<4I', struct.pack('<4f', *sf[4*c:4*c+4]))), ('shadow_f', 'row', c))
    for k in range(2):
        sx = s['shadow_x'][k]
        for c in range(2):
            tags.setdefault(tuple(struct.unpack('<4I', struct.pack('<4f', *sx[4*c:4*c+4]))), ('shadow_x%d' % k, 'row', c))
cam = rec.camera()
res = collections.Counter()
for i in range(f.ndraws):
    o = f.origin(i); d = f.draw(i)
    if o['pass_site'] in (0x82C04634, 0x82BF4D38, 0x82BFBD58):
        vb = f.bank(d['vs_bank'])
        for r in (54, 56, 57, 58, 59, 64, 65, 66, 67, 13, 14, 15, 16, 9, 10, 11, 12, 72, 162, 7):
            v = tuple(vb[4*r:4*r+4])
            t = tags.get(v)
            res[(hex(o['pass_site']), r, str(t))] += 1
for k, v in sorted(res.items()):
    print(k, v)
