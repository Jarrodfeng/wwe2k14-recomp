"""Session 97: the crowd's motions in m2.pac ("YMKs" key sets), read from the player's own game files (read-only).

    py -I tools\\assets\\m2mot.py --list [--act 14005]          list index entries
    py -I tools\\assets\\m2mot.py --act 14005 --type 0 [--keys]   decode one motion to keys (per slot angles)
    py -I tools\\assets\\m2mot.py --fit                          search every crowd motion for the dumped pose

Layout (all claims in out\\runtime\\s97\\m2_motion.md):
- m2.pac = EPAC; at 0x4000 `YMKs`; index header word (LE u32 count N) at 0x4110; N 16-byte records from 0x4120:
  {4 bytes (a pointer slot the game overwrites), u8, u8 type (0..19 = crowd person type), LE u16 act, LE u32 offset,
  LE u32 size (steps of 2 frames)}; key data starts at 0x4110 + 16 N + 4 (+ offset).
- An entry = a token stream: token < 96 = skip that many steps (no data); token 96 = chunk with a LE u16 length;
  token 97..255 = chunk of (token - 96) bytes. A chunk = one key.
- Chunk (optional prefix byte 0x80|n then n bytes), then `7D count` = key op, 3 LE float32 root position, then one item per
  slot (21 slots, bytes by slot type): LE int16 angles (65536 = 360 deg).
"""

import argparse
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))
import pac  # noqa: E402

M2 = os.path.join(REPO, 'game_data', 'extracted', 'pac', 'm2.pac')

# The crowd MoHo's 21 slots: (slot type from the slot table, node index of the 17-node skeleton or None).
# Slot -> node read from the live MoHo slot table (+368 pointer = the node's local matrix), type from +372.
SLOT_TYPES = [7, 7, 7, 6, 23, 6, 7, 3, 6, 6, 7, 3, 6, 7, 2, 23, 2, 7, 2, 23, 2]
SLOT_NODE = [0, 5, 6, 15, 16, 11, 12, 13, 14, 7, 8, 9, 10, 1, 2, None, None, 3, 4, None, None]
NODE_NAMES = ['root', 'l_momo', 'l_sune', 'r_momo', 'r_sune', 'koshi', 'mune', 'r_sakotsu', 'r_ninoude', 'r_kote',
              'r_te', 'l_sakotsu', 'l_ninoude', 'l_kote', 'l_te', 'kubi', 'atama']
ANG = 2 * math.pi / 65536.0
TSCALE = 10.0  # the type skeleton instances hold the node table translations x10 (measured: 43.48 vs 4.348)


def item_bytes(t):
    return {2: 2, 3: 4, 6: 4}.get(t, 6)


class M2:
    def __init__(self, path=M2):
        self.d = pac.load(path)
        d = self.d
        assert d[0x4000:0x4004] == b'YMKs'
        self.n = struct.unpack_from('<I', d, 0x4110)[0]
        self.base = 0x4110 + 16 * self.n + 4
        self.entries = []
        for i in range(self.n):
            p = 0x4120 + 16 * i
            typ = d[p + 5]
            act = struct.unpack_from('<H', d, p + 6)[0]
            off, size = struct.unpack_from('<II', d, p + 8)
            self.entries.append((i, act, typ, off, size))
        offs = sorted(set(e[3] for e in self.entries))
        self._next = {o: (offs[k + 1] if k + 1 < len(offs) else None) for k, o in enumerate(offs)}

    def find(self, act, typ):
        for e in self.entries:
            if e[1] == act and e[2] == typ:
                return e
        return None

    def chunks(self, e):
        """[(step, chunk bytes)] for an entry: step = index of the 2-frame step the key sits at."""
        _, act, typ, off, size = e
        end_off = self._next.get(off)
        p = self.base + off
        end = min(len(self.d), self.base + end_off if end_off is not None else p + 1000000)
        step = 0
        out = []
        while p < end and p + 3 <= len(self.d):
            t = self.d[p]
            if t < 96:
                step += t
                p += 1
                continue
            if t == 96:
                L = self.d[p + 1] | self.d[p + 2] << 8
                q = p + 3
            else:
                L = t - 96
                q = p + 1
            out.append((step, self.d[q:q + L]))
            step += 1
            p = q + L
        return out


def decode_chunk(body):
    """-> dict(count, pos, slots) or None when the chunk is not a full key (op 0x7D)."""
    if body[0] & 0x80:
        body = body[(body[0] & 0x7F) + 1:]
    if not body or body[0] != 0x7D:
        return None
    if len(body) != 110:  # the crowd slot table; other skeletons (e.g. 15089+, 14500+) have other tables: not decoded
        return None
    cnt = body[1]
    pos = struct.unpack_from('<3f', body, 2)
    p = 14
    slots = []
    for t in SLOT_TYPES:
        n = item_bytes(t)
        slots.append(struct.unpack_from('<%dh' % (n // 2), body, p))
        p += n
    return {'count': cnt, 'pos': pos, 'slots': slots, 'used': p, 'len': len(body)}


# ---- pose synthesis -------------------------------------------------------------------------------------------------
# Rotation convention (row vectors, as the game stores them): E(a) = Rx(a0) Ry(a1) Rz(a2), the local rotation is
# E(angles) * Rrest, Rrest = E(node table rot of the body skeleton); translation = node table t (root: key position / 10).

def _rx(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0], [0, c, s], [0, -s, c]]


def _ry(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, -s], [0, 1, 0], [s, 0, c]]


def _rz(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, s, 0], [-s, c, 0], [0, 0, 1]]


def _mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def euler_matrix(ax, ay, az):
    return _mm(_mm(_rx(ax), _ry(ay)), _rz(az))


def _to_quat(M):
    """Quaternion (x, y, z, w) of a row-vector rotation matrix (its transpose is the column form)."""
    m = [[M[j][i] for j in range(3)] for i in range(3)]
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4)
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        return (s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
    if m[1][1] > m[2][2]:
        s = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        return ((m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
    s = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
    return ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s)


def _from_quat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    m = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[m[j][i] for j in range(3)] for i in range(3)]


def _slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b = tuple(-x for x in b)
        d = -d
    if d > 0.9995:
        return tuple(x + (y - x) * t for x, y in zip(a, b))
    th = math.acos(d)
    s = math.sin(th)
    wa, wb = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def slot_angles(slot_type, raw):
    """(ax, ay, az) in 65536 = 360 deg units for a slot item: slot type 2 = y, 3 = x y, 6 = y z, 7 / 23 = x y z."""
    if slot_type == 2:
        return (0, raw[0], 0)
    if slot_type == 3:
        return (raw[0], raw[1], 0)
    if slot_type == 6:
        return (0, raw[0], raw[1])
    return tuple(raw)


def keys(m, entry):
    """Decoded full keys of an entry, each with 'frame' = 2 * step."""
    out = []
    for step, body in m.chunks(entry):
        k = decode_chunk(body) if body else None
        if k:
            k['frame'] = 2 * step
            out.append(k)
    return out


def pose(ks, frame, rest):
    """Local 4x4 (rows as lists) per node at game frame `frame` (fractional ok); rest = per node dicts(t, rot) of the
    body skeleton. The motion holds its last key past the end."""
    i = 0
    while i + 1 < len(ks) and ks[i + 1]['frame'] <= frame:
        i += 1
    a = ks[i]
    b = ks[i + 1] if i + 1 < len(ks) else a
    dur = 2 * (a['count'] + 1)
    t = 0.0 if b is a else min(1.0, max(0.0, (frame - a['frame']) / float(dur)))
    out = [None] * len(rest)
    for s, node in enumerate(SLOT_NODE):
        if node is None:
            continue
        ty = SLOT_TYPES[s]
        va = slot_angles(ty, a['slots'][s])
        vb = slot_angles(ty, b['slots'][s])
        if ty == 7:
            ra = euler_matrix(*[v * ANG for v in va])
            rb = euler_matrix(*[v * ANG for v in vb])
            E = _from_quat(_slerp(_to_quat(ra), _to_quat(rb), t))
        else:
            ang = []
            for x, y in zip(va, vb):
                dd = (y - x + 32768) % 65536 - 32768  # shortest way round
                ang.append((x + dd * t) * ANG)
            E = euler_matrix(*ang)
        R = _mm(E, euler_matrix(*rest[node]['rot']))
        if node == 0:
            tr = [(a['pos'][j] + (b['pos'][j] - a['pos'][j]) * t) * 0.1 for j in range(3)]
        else:
            tr = [v * TSCALE for v in rest[node]['t']]
        out[node] = [R[0] + [0.0], R[1] + [0.0], R[2] + [0.0], tr + [1.0]]
    return out


def load_rest(arena=5, body=0):
    """The 17-node skeleton of a crowd body (same table as the mesh-less entry 20200) from an arena pac."""
    import crowd_pose
    data = pac.load(os.path.join(REPO, 'game_data', 'extracted', 'pac', 'bg', 'bg%02d.pac' % arena))
    crowd_pose.LEAVES.update(dict(pac.entries(data, 3)))
    return crowd_pose.read_body(crowd_pose.LEAVES['p0/20000/%d' % (20000 + body)])['nodes']


def dumped_pose():
    """The 17 local matrices of the first crowd type skeleton from the live dump (local only)."""
    s = open(os.path.join(REPO, 'out', 'runtime', 's97', 'crowd_skel_dump.bin'), 'rb').read()
    return [[list(struct.unpack('>4f', s[0x120 + 64 * k + 16 * r:0x120 + 64 * k + 16 * r + 16])) for r in range(4)]
            for k in range(17)]


def pose_error(a, b):
    """Per node: max abs difference over the 3x3 rotation and the translation row of two poses."""
    return [max(abs(x[r][c] - y[r][c]) for r in range(4) for c in range(3)) for x, y in zip(a, b)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--fit', action='store_true', help='search every crowd motion / frame for the dumped pose')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--act', type=int)
    ap.add_argument('--type', type=int, default=0)
    ap.add_argument('--keys', action='store_true')
    a = ap.parse_args()
    m = M2()
    if a.list:
        for e in m.entries:
            if a.act is None or e[1] == a.act:
                print('entry %3d act %5d type %3d offset %8d steps %4d' % e)
        return 0
    if a.fit:
        rest = load_rest()
        target = dumped_pose()
        best = []
        for e in m.entries:
            if not (14001 <= e[1] <= 14011) or e[2] != 0:
                continue
            ks = keys(m, e)
            if not ks:
                continue
            for f in range(0, ks[-1]['frame'] + 1):
                best.append((max(pose_error(pose(ks, f, rest), target)), e[1], e[2], f))
        best.sort()
        for b in best[:5]:
            print('max abs err %.5f  act %d type %d frame %d' % b)
        _, act, typ, f = best[0]
        errs = pose_error(pose(keys(m, m.find(act, typ)), f, rest), target)
        print('per node:', ' '.join('%s %.4f' % (NODE_NAMES[i], v) for i, v in enumerate(errs)))
        return 0
    if a.act is not None:
        e = m.find(a.act, a.type)
        for step, body in m.chunks(e):
            k = decode_chunk(body)
            print(step, len(body), None if k is None else (k['count'], [round(v, 2) for v in k['pos']]))
            if a.keys and k:
                for i, s in enumerate(k['slots']):
                    print('   slot %2d %-10s %s' % (i, NODE_NAMES[SLOT_NODE[i]] if SLOT_NODE[i] is not None else '-', s))
    return 0


if __name__ == '__main__':
    sys.exit(main())
