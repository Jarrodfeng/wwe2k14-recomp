"""Wwise node volume (lane 283): the Volume property (id 0, dB) of a Sound / container / actor-mixer and its parents, from the
HIRC objects of Game_Sound_E_S.pck (bank v65, big-endian).  Numbers only.
node_props(g, oid) -> (parent id or 0, {prop id: float}); chain_volume_db(g, oid) -> (sum dB, [(type, dB), ...]).
The DirectParentID is found by scanning for the first 32-bit window (after the source fields) that names another object, then
two flag bytes, cProps, the ids and the f32 values follow (checked against hand-read examples: 01 00 c0a00000 = Volume -5.0 dB,
01 04 42c80000 = Priority 100)."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _u32(b, i):
    return struct.unpack('>I', b[i:i + 4])[0]


def node_props(g, oid):
    t, b = g.objs[oid]
    if t in (8, 19):  # bus: [parent bus id][cProps][ids][f32 values] (hand-read: master bus 00000000 01 00 c0000000 = -2 dB)
        n = b[4]
        ids = list(b[5:5 + n])
        vals = [struct.unpack('>f', b[5 + n + 4 * k:9 + n + 4 * k])[0] for k in range(n)]
        return _u32(b, 0), dict(zip(ids, vals))
    start = 12 if t == 2 else 6
    for i in range(start, len(b) - 8):
        v = _u32(b, i)
        if v in g.objs and g.objs[v][0] in (2, 5, 7, 8, 9, 19) and v != oid:
            n = b[i + 6]
            if n > 12 or i + 7 + 5 * n > len(b):
                continue
            ids = list(b[i + 7:i + 7 + n])
            vals = [struct.unpack('>f', b[i + 7 + n + 4 * k:i + 11 + n + 4 * k])[0] for k in range(n)]
            return v, dict(zip(ids, vals))
    return 0, {}


def chain_volume_db(g, oid, limit=12):
    total, chain = 0.0, []
    for _ in range(limit):
        if oid not in g.objs:
            break
        parent, props = node_props(g, oid)
        v = props.get(0, 0.0)
        chain.append((g.objs[oid][0], round(v, 2)))
        total += v
        if not parent:
            break
        oid = parent
    return total, chain
