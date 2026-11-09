r"""Session 97: motion reader (our code). Reads the player's own motion pacs (game_data\extracted\pac\mot*.pac).

    py tools\assets\yanm.py                          list the banks of the mot pacs
    py tools\assets\yanm.py mot00 CA01               list the acts of a bank (top id, act, halves)
    py tools\assets\yanm.py mot00 CA01 40 23101      list the motions of an act (zlib half; 'bpe' = the other half)
    py tools\assets\yanm.py mot00 CA01 40 23101 00000000 [bpe] [ikfill] [f=0,30]
                                                      decode one motion: tracks, self-checks, per-node frames

Library: open_pac, iter_acts, load_act, parse_motion(blk, yanm_offset, layouts), sample_rot, sample_pos,
motion_length, fill_ik_from_bpe, hash_name, node_hash. Format notes: out\runtime\s97\motion.md.
Nothing game-derived is written to the repo; the BPE half and some ZLIB motions need the layout tables, read
from the local guest image out\harness\index\image.bin (a build artefact, never committed).
"""
import math
import os
import struct
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACDIR = os.path.join(REPO, 'game_data', 'extracted', 'pac')


# ---------------------------------------------------------------- containers
def bpe_decode(block):
    expanded = struct.unpack_from('<I', block, 12)[0]
    p = 16
    out = bytearray()
    while len(out) < expanded:
        left = list(range(256))
        right = [0] * 256
        c = 0
        while True:
            count = block[p]
            p += 1
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c] = block[p]
                p += 1
                if c != left[c]:
                    right[c] = block[p]
                    p += 1
                c += 1
            if c == 256:
                break
        n = block[p] | block[p + 1] << 8
        p += 2
        for i in range(n):
            stack = [block[p + i]]
            while stack:
                x = stack.pop()
                if x == left[x]:
                    out.append(x)
                else:
                    stack.append(right[x])
                    stack.append(left[x])
        p += n
    return bytes(out[:expanded])


def unpack_leaf(data):
    tag = data[:4]
    if tag == b'BPE ':
        return bpe_decode(data)
    if tag == b'ZLIB':
        return zlib.decompressobj().decompress(data[16:])
    return bytes(data)


def pach(b, base=0):
    """[(id, offset, size)] of the PACH at b[base:] (offsets absolute in b)."""
    if b[base:base + 4] != b'PACH':
        raise ValueError('not a PACH')
    n = struct.unpack_from('<I', b, base + 4)[0]
    first = base + 8 + 12 * n
    out = []
    for i in range(n):
        i_, o, s = struct.unpack_from('<III', b, base + 8 + 12 * i)
        out.append((i_, first + o, s))
    return out


def banks(d):
    """[(name, offset, size)] of a mot pac (ZMOT / MOT directory at 0x800)."""
    o = 0x80C
    out = []
    while d[o:o + 8].strip(b' \x00'):
        n = d[o:o + 8].decode('latin1').strip()
        s, z = struct.unpack_from('<II', d, o + 8)
        out.append((n, 0x4000 + s * 0x800, z * 256))
        o += 16
    return out


def open_pac(name):
    if not name.lower().endswith('.pac'):
        name += '.pac'
    with open(os.path.join(PACDIR, name), 'rb') as fh:
        return fh.read()


def bank_blocks(d, bank):
    """{top_id: [(sub_id, offset, size)]} for a bank; offsets absolute in d."""
    for n, off, sz in banks(d):
        if n == bank:
            res = {}
            for ti, to, ts in pach(d, off):
                res[ti] = pach(d, to)
            return res
    raise KeyError(bank)


def load_block(d, off, size):
    return unpack_leaf(d[off:off + size])




# ---------------------------------------------------------------- motion block ("act")
def node_hash(name):
    """The game's node-name hash (sub_823D2050 loop): h = ((h << 7) | c) ^ (h >> 25), 32 bit, per byte."""
    h = 0
    for c in name.encode('latin1'):
        h = ((((h << 7) & 0xFFFFFFFF) | c) ^ (h >> 25)) & 0xFFFFFFFF
    return h


class Act:
    """A decoded motion block (one leaf of a mid PACH): name table + one YANM chunk per motion."""

    def __init__(self, blk):
        self.blk = blk
        end_names, payload, name_off, first = struct.unpack_from('>4I', blk, 0)
        self.names = []
        for o in range(name_off, end_names, 40):
            w = struct.unpack_from('>I', blk, o)[0]
            self.names.append((w, blk[o + 4:o + 36].split(b'\x00')[0].decode('latin1')))
        self.yanm = []  # offsets of the 'YANM' tags
        p = first
        n = len(blk)
        while p + 8 <= n:
            tag = blk[p:p + 4]
            if tag in (b'YANM', b'POF0', b'YAT2'):
                sz = struct.unpack_from('>I', blk, p + 4)[0]
                if tag == b'YANM':
                    self.yanm.append(p)
                p += 8 + sz
            else:
                p += 4
        # motions = names after the "root" and "<act>" records
        self.motion_names = [nm for w, nm in self.names if nm.isdigit() and len(nm) == 8]




# ---------------------------------------------------------------- YANM tracks
# Track record (direct form): u16 flags, u16 size, u32 node-name hash, then components in this bit order (sizes from
# the game's finder functions sub_823D2E48 & co, checked against every track size in mot00..mot08).
ORDER = (15, 14, 13, 12, 11, 10, 9, 8, 4, 3, 2, 1, 0)
SCALE_COARSE = math.pi / 128      # 0x8200FFD0
SCALE_FINE = math.pi / 4608       # 0x8200FFD4
SCALE_FINEST = math.pi / 11520    # 0x8200FFD8
SCALE_I16 = math.pi / 32768       # 0x82004F48


def comp_size(bit, flags):
    b5 = (flags >> 5) & 1
    if bit == 15:
        return 12 * (b5 + 2)
    if bit == 8:
        return 8 * b5 + 24
    if bit in (14, 13, 12, 11, 10, 9, 4, 3, 2):
        return 8 * (b5 + 1)
    return 8  # bits 1 and 0


# bits 6..15 sizes used by the indirect (layout table) form, sub_823DC020
INDIRECT_SIZE = {6: 8, 7: 8, 8: 24, 9: 8, 10: 8, 11: 8, 12: 8, 13: 8, 14: 8, 15: 24}


def qmul(a, b):
    """Hamilton product a*b, quaternions (x, y, z, w). sub_82C40878(dst, A, B) = A*B (matrix read in the listing)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q))
    return (0.0, 0.0, 0.0, 1.0) if n < 1e-12 else tuple(c / n for c in q)


def qslerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b = tuple(-c for c in b)
        d = -d
    if d > 0.9995:
        return qnorm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    th = math.acos(min(1.0, d))
    s = math.sin(th)
    wa = math.sin((1 - t) * th) / s
    wb = math.sin(t * th) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def euler_quat(ax, ay, az):
    """sub_82C40E60: q = qz * qy * qx, each about its axis by the angle (radians)."""
    qx = (math.sin(ax / 2), 0.0, 0.0, math.cos(ax / 2))
    qy = (0.0, math.sin(ay / 2), 0.0, math.cos(ay / 2))
    qz = (0.0, 0.0, math.sin(az / 2), math.cos(az / 2))
    return qmul(qmul(qz, qy), qx)


def axis_quat(axis, ang):
    s, c = math.sin(ang / 2), math.cos(ang / 2)
    v = [0.0, 0.0, 0.0]
    v[axis] = s
    return (v[0], v[1], v[2], c)


def half(h):
    s = -1.0 if h & 0x8000 else 1.0
    e = (h >> 10) & 31
    m = h & 1023
    if e == 0:
        return s * m * 2.0 ** -24
    if e == 31:
        return s * float('inf') if m == 0 else float('nan')
    return s * (1 + m / 1024.0) * 2.0 ** (e - 15)


class Track:
    def __init__(self, h, flags):
        self.hash = h
        self.flags = flags
        self.name = None
        self.comps = {}      # bit -> absolute offset in the block of the component record
        self.keys_rot = []   # [(label, [(t, quat)])] components to multiply in order
        self.keys_pos = None  # [(t, (x,y,z))] or ('const', (x,y,z))
        self.unsupported = []
        self.ik = None       # b0 curve: [(t, (x, y, z, w))] raw, see _rot_packed


def _times(dts):
    t, out = 0, []
    for d in dts:
        t += d
        out.append(t)
    return out


def _rot_euler4(blk, base, ptr, cnt, scale):
    """4-byte keys: three signed bytes (x, y, z euler angle steps) + unsigned time delta (sub_823D26C8 / 2748 / 27C8)."""
    keys, t = [], 0
    for i in range(cnt):
        o = base + ptr + 4 * i
        x, y, z = struct.unpack_from('bbb', blk, o)
        t += blk[o + 3]
        keys.append((t, euler_quat(x * scale, y * scale, z * scale)))
    return keys


def _rot_euler16(blk, base, ptr, cnt):
    """b13: 8-byte keys: i16 x, y, z + u16 time delta, scale pi/32768 (sub_823D2648)."""
    keys, t = [], 0
    for i in range(cnt):
        o = base + ptr + 8 * i
        x, y, z = struct.unpack_from('>hhh', blk, o)
        t += struct.unpack_from('>H', blk, o + 6)[0]
        keys.append((t, euler_quat(x * SCALE_I16, y * SCALE_I16, z * SCALE_I16)))
    return keys


def _rot_e20(blk, base, ptr, cnt):
    """b2: 20-byte keys: i16 time delta, i16 x, y, z (scale pi/32768), 12 more bytes not read here (tangents?)."""
    keys, t = [], 0
    for i in range(cnt):
        o = base + ptr + 20 * i
        t += struct.unpack_from('>h', blk, o)[0]
        x, y, z = struct.unpack_from('>hhh', blk, o + 2)
        keys.append((t, euler_quat(x * SCALE_I16, y * SCALE_I16, z * SCALE_I16)))
    return keys


def _rot_hinge(blk, base, ptr, cnt, axis, scale):
    """b11 / b10 / b3: 2-byte keys: signed byte angle step, unsigned time delta; one axis (comp + 6)."""
    keys, t = [], 0
    for i in range(cnt):
        o = base + ptr + 2 * i
        a = struct.unpack_from('b', blk, o)[0]
        t += blk[o + 1]
        keys.append((t, axis_quat(axis, a * scale)))
    return keys


_STRIDE_A = (4, 6, 8, 16, 3, 6, 6, 12)  # table at 0x835E1A98 (first 8 words)


def _rot_packed(blk, base, ptr, mode, cnt):
    """b0 (the general curve) with mode & 7 == 1: single stream, a 16 byte header of eight float16 (base x4, scale x4)
    unless (mode >> 3) & 7 == 3, then keys = time-delta byte + a packed 4-vector, format by (mode >> 3) & 7:
    0 = 4 x u8, 1 = 4 x 12 bit, 2 = 4 x u16 (two planes), 3 = 4 floats. Returns [(t, (x, y, z, w))] (raw values).
    These are NOT local rotations: sub_823E2210 hands them to the IK solver sub_823DA1F0 (mode & 7 in 1, 2) together
    with the character's bone chain. Cross-check: frame 0 of a b0 calf differs 160-172 degrees from the BPE half's
    hinge for the same bone, while every b14 / b11 track agrees exactly."""
    m = (mode >> 3) & 7
    if (mode & 7) != 1 or m > 3:
        return None
    o = base + ptr
    if m != 3:
        A = [half(struct.unpack_from('>H', blk, o + 2 * i)[0]) for i in range(4)]
        B = [half(struct.unpack_from('>H', blk, o + 8 + 2 * i)[0]) for i in range(4)]
        o += 16
    stride = _STRIDE_A[m] + 1
    keys, t = [], 0
    for i in range(cnt):
        k = o + stride * i
        t += blk[k]
        d = blk[k + 1:k + stride]
        if m == 0:
            c = list(d)
        elif m == 1:
            c = [(d[0] << 4) | (d[4] >> 4), (d[1] << 4) | (d[4] & 15), (d[2] << 4) | (d[5] >> 4), (d[3] << 4) | (d[5] & 15)]
        elif m == 2:
            c = [(d[0] << 8) | d[4], (d[1] << 8) | d[5], (d[2] << 8) | d[6], (d[3] << 8) | d[7]]
        if m == 3:
            raw = struct.unpack_from('>4f', d, 0)
        else:
            raw = [A[j] + B[j] * c[j] for j in range(4)]
        keys.append((t, tuple(raw)))
    return keys


def _pos_keys(blk, base, comp, wide):
    """b15 (4-byte keys: 3 signed bytes + time byte) and b8 (8-byte keys: 3 x i16 + u16 time): record
    {ptr, count, scale, base x, y, z}; position = base + scale * running sum of the key deltas (sub_823D8CF0 / 8EC0).
    scale < 1e-7 means a constant at base."""
    ptr, cnt = struct.unpack_from('>II', blk, comp)
    scale, bx, by, bz = struct.unpack_from('>4f', blk, comp + 8)
    if scale < 1e-7 or ptr == 0 or cnt == 0:
        return ('const', (bx, by, bz))
    keys, t, sx, sy, sz = [], 0, 0, 0, 0
    for i in range(cnt):
        if wide:
            o = base + ptr + 8 * i
            dx, dy, dz, dt = struct.unpack_from('>hhhH', blk, o)
        else:
            o = base + ptr + 4 * i
            dx, dy, dz = struct.unpack_from('bbb', blk, o)
            dt = blk[o + 3]
        sx += dx
        sy += dy
        sz += dz
        t += dt
        keys.append((t, (bx + sx * scale, by + sy * scale, bz + sz * scale)))
    return keys


def _build_track(blk, base, tr, comp_at):
    """Decode the components of tr. comp_at: bit -> absolute offset of the component record."""
    fl = tr.flags
    if (fl >> 5) & 1:
        tr.unsupported.append('b5 (second key set)')
        return tr

    def kc(bit):
        return struct.unpack_from('>II', blk, comp_at[bit])

    rots = []
    if fl & 0x0001:
        mode, cnt, ptr = struct.unpack_from('>HHI', blk, comp_at[0])
        ks = _rot_packed(blk, base, ptr, mode, cnt)
        if ks is None:
            tr.unsupported.append('b0 mode 0x%04x (not decoded)' % mode)
        else:
            tr.ik = ks
            tr.unsupported.append('b0 IK/effector curve (mode 0x%04x): solved by sub_823DA1F0, needs the skeleton' % mode)
    elif fl & 0x2000:
        ptr, cnt = kc(13)
        rots.append(('b13', _rot_euler16(blk, base, ptr, cnt)))
    elif fl & 0x4000:
        ptr, cnt = kc(14)
        rots.append(('b14', _rot_euler4(blk, base, ptr, cnt, SCALE_COARSE)))
        if fl & 0x1000:
            ptr, cnt = kc(12)
            rots.append(('b12', _rot_euler4(blk, base, ptr, cnt, SCALE_FINE)))
        elif fl & 0x0010:
            ptr, cnt = kc(4)
            rots.append(('b4', _rot_euler4(blk, base, ptr, cnt, SCALE_FINEST)))
    elif fl & 0x0800:
        for bit, sc in ((11, SCALE_COARSE), (10, SCALE_FINE), (3, SCALE_FINEST)):
            if fl & (1 << bit) and (bit == 11 or not any(l in ('b10', 'b3') for l, _ in rots)):
                ptr = struct.unpack_from('>I', blk, comp_at[bit])[0]
                cnt, axis = struct.unpack_from('>HH', blk, comp_at[bit] + 4)
                if axis > 2:
                    tr.unsupported.append('hinge axis %d' % axis)
                    continue
                rots.append(('b%d' % bit, _rot_hinge(blk, base, ptr, cnt, axis, sc)))
    elif fl & 0x0004:
        ptr, cnt = kc(2)
        rots.append(('b2', _rot_e20(blk, base, ptr, cnt)))
    tr.keys_rot = rots
    if fl & 0x8000:
        tr.keys_pos = _pos_keys(blk, base, comp_at[15], False)
    elif fl & 0x0100:
        tr.keys_pos = _pos_keys(blk, base, comp_at[8], True)
    known = 0x8000 | 0x4000 | 0x2000 | 0x1000 | 0x0800 | 0x0400 | 0x0100 | 0x0010 | 0x0008 | 0x0004 | 0x0001
    if fl & ~known & 0xFFDF:
        tr.unsupported.append('flags 0x%04x' % (fl & ~known & 0xFFDF))
    return tr


def parse_motion(blk, p, layouts=None):
    """Tracks of the YANM chunk at blk[p:]. Direct form (ZLIB half) is self-contained. The indirect form (BPE half,
    byte 1 & 2) needs the game's static layout tables (the executable image): pass layouts(id) -> [(hash, flags)]."""
    base = p + 8
    b0, b1, cnt = blk[base], blk[base + 1], struct.unpack_from('>H', blk, base + 2)[0]
    ptr = struct.unpack_from('>I', blk, base + 4)[0]
    tracks = []
    if not (b1 & 2):
        q = base + ptr
        for _ in range(cnt):
            fl, sz, h = struct.unpack_from('>HHI', blk, q)
            tr = Track(h, fl)
            comp_at, o = {}, q + 8
            for bit in ORDER:
                if fl & (1 << bit):
                    comp_at[bit] = o
                    o += comp_size(bit, fl)
            if o != q + sz:
                tr.unsupported.append('size mismatch %d vs %d' % (o - q, sz))
            else:
                _build_track(blk, base, tr, comp_at)
            tracks.append(tr)
            q += sz
    else:
        if layouts is None:
            raise ValueError('indirect motion: layout tables needed (executable image)')
        lay = layouts(cnt)
        o = base + ptr
        for h, fl in lay:
            tr = Track(h, fl)
            comp_at, size = {}, 0
            for bit in ORDER:
                if fl & (1 << bit):
                    comp_at[bit] = o + size
                    size += INDIRECT_SIZE.get(bit, 0)
            _build_track(blk, base, tr, comp_at)
            tracks.append(tr)
            o += size
    return tracks


def _hold(keys, f):
    """index i, u such that the value at frame f is between keys[i] and keys[i+1]."""
    if f <= keys[0][0]:
        return 0, 0.0
    if f >= keys[-1][0]:
        return len(keys) - 1, 0.0
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= f:
            lo = mid
        else:
            hi = mid
    span = keys[lo + 1][0] - keys[lo][0]
    return lo, (f - keys[lo][0]) / span if span > 0 else 0.0


def sample_rot(tr, f):
    """Local rotation quaternion at frame f: components multiply in order (main * extra) like the game; linear/slerp
    between keys (the game's spline mode [player+20]==1 is not reproduced)."""
    q = (0.0, 0.0, 0.0, 1.0)
    first = True
    for _, keys in tr.keys_rot:
        if not keys:
            continue
        i, u = _hold(keys, f)
        v = keys[i][1] if u == 0.0 else qslerp(keys[i][1], keys[i + 1][1], u)
        q = v if first else qmul(q, v)
        first = False
    return q


def sample_pos(tr, f):
    k = tr.keys_pos
    if k is None:
        return None
    if k[0] == 'const':
        return k[1]
    i, u = _hold(k, f)
    if u == 0.0:
        return k[i][1]
    a, b = k[i][1], k[i + 1][1]
    return tuple(x + (y - x) * u for x, y in zip(a, b))


def motion_length(tracks):
    n = 0
    for tr in tracks:
        for _, ks in tr.keys_rot:
            if ks:
                n = max(n, ks[-1][0])
        if tr.keys_pos and tr.keys_pos[0] != 'const':
            n = max(n, tr.keys_pos[-1][0])
    return n


# ---------------------------------------------------------------- names, layouts, checks, CLI
BIP = ('Bip', 'Bip Pelvis', 'Bip Spine', 'Bip Spine1', 'Bip Neck', 'Bip Head',
       'Bip L Clavicle', 'Bip L UpperArm', 'Bip L Forearm', 'Bip L Hand',
       'Bip R Clavicle', 'Bip R UpperArm', 'Bip R Forearm', 'Bip R Hand',
       'Bip L Thigh', 'Bip L Calf', 'Bip L Foot', 'Bip L Toe0',
       'Bip R Thigh', 'Bip R Calf', 'Bip R Foot', 'Bip R Toe0', 'vector', 'root') + \
    tuple('bone%02d' % i for i in range(50))
IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')  # the guest image (local build artefact)
_NAMES = {node_hash(n): n for n in BIP}


def hash_name(h):
    return _NAMES.get(h)


def image_layouts(path=IMAGE):
    """Layout tables for the indirect (BPE half) form: pointer table at guest 0x82012270 (sub_82415188), each table a
    list of {u32 node hash, u16 flags, u16 pad} ended by flags 0xFFFF. Read from the local guest image (not shipped)."""
    img = open(path, 'rb').read()

    def rd(ad, n):
        return img[ad - 0x82000000:ad - 0x82000000 + n]

    def layouts(i):
        ad = struct.unpack_from('>I', rd(0x82012270 + 4 * i, 4))[0]
        out = []
        while True:
            h, fl, _ = struct.unpack_from('>IHH', rd(ad + 8 * len(out), 8))
            if fl == 0xFFFF:
                return out
            out.append((h, fl))
    return layouts


def iter_acts(d, bank):
    """yield (top_id, mid_index, half, act_id, offset, size); half is 'bpe' (even mid) or 'zlib' (odd mid)."""
    for top, mids in bank_blocks(d, bank).items():
        for mi, (mid_id, mo, ms) in enumerate(mids):
            try:
                leaves = pach(d, mo)
            except ValueError:
                continue
            for i, o, s in leaves:
                if i < 65000 and d[o:o + 4] in (b'BPE ', b'ZLIB'):
                    yield top, mid_id, 'bpe' if d[o:o + 4] == b'BPE ' else 'zlib', i, o, s


def load_act(d, o, s):
    return Act(unpack_leaf(d[o:o + s]))


def pose_frames(tracks, nframes):
    """{hash: [(quat, pos or None)] per frame 0..nframes-1}"""
    out = {}
    for tr in tracks:
        out[tr.hash] = [(sample_rot(tr, f), sample_pos(tr, f)) for f in range(nframes)]
    return out


def fill_ik_from_bpe(tracks, bpe_tracks):
    """Stand-in for the IK solver: a b0 track takes the rotation of the BPE half's track for the same node (a hinge
    for calves, an approximation: the BPE half is the low-detail copy with sparse keys)."""
    by = {t.hash: t for t in bpe_tracks}
    n = 0
    for tr in tracks:
        if tr.ik is not None and not tr.keys_rot and tr.hash in by and by[tr.hash].keys_rot:
            tr.keys_rot = by[tr.hash].keys_rot
            tr.unsupported = [u for u in tr.unsupported if not u.startswith('b0 IK')]
            tr.unsupported.append('b0 IK replaced by the BPE half rotation')
            n += 1
    return n


def self_check(tracks, label=''):
    """Prints the checks required of the decoder. Returns True when all pass."""
    n = max(1, motion_length(tracks) + 1)
    ok = True
    worst_norm, worst_step, worst_who = 0.0, 0.0, None
    unnamed = []
    for tr in tracks:
        if not tr.keys_rot and not tr.keys_pos:
            continue
        prev = None
        for f in range(n):
            q = sample_rot(tr, f)
            worst_norm = max(worst_norm, abs(math.sqrt(sum(c * c for c in q)) - 1.0))
            if prev is not None:
                dot = min(1.0, abs(sum(a * b for a, b in zip(q, prev))))
                step = math.degrees(2 * math.acos(dot))  # sign flips (q vs -q) are not a step
                if step > worst_step:
                    worst_step, worst_who = step, (tr.hash, f)
            prev = q
        if hash_name(tr.hash) is None:
            unnamed.append(hex(tr.hash))
    print('%s tracks=%d frames=%d' % (label, len(tracks), n))
    print('  unit quaternions: max | |q|-1 | = %.2e  [%s]' % (worst_norm, 'PASS' if worst_norm < 1e-4 else 'FAIL'))
    ok &= worst_norm < 1e-4
    nm = hash_name(worst_who[0]) if worst_who else None
    print('  continuity: largest frame-to-frame rotation step = %.1f deg (track %s frame %s) [%s]' %
          (worst_step, nm or (hex(worst_who[0]) if worst_who else None), worst_who[1] if worst_who else '-',
           'PASS' if worst_step < 60 else 'WARN'))
    print('  nodes by name hash: %d named, unnamed hashes: %s' % (len(tracks) - len(unnamed), ', '.join(unnamed) or 'none'))
    bad = [(hash_name(t.hash) or hex(t.hash), t.unsupported) for t in tracks if t.unsupported]
    if bad:
        print('  UNSUPPORTED tracks: %s' % bad)
    return ok


def describe(tr):
    parts = ['%s:%d' % (l, len(k)) for l, k in tr.keys_rot]
    if tr.ik is not None:
        parts.append('b0-IK:%d' % len(tr.ik))
    if tr.keys_pos is not None:
        parts.append('pos:const' if tr.keys_pos[0] == 'const' else 'pos:%d' % len(tr.keys_pos))
    return ' '.join(parts)


def _main(argv):
    if not argv:
        for pn in ('mot00', 'mot01', 'mot02', 'mot03', 'mot07', 'mot08'):
            d = open_pac(pn)
            print(pn, [(n, sz // 1024) for n, o, sz in banks(d)])
        return 0
    pn = argv[0]
    d = open_pac(pn)
    if len(argv) == 1:
        print(banks(d))
        return 0
    bank = argv[1]
    if len(argv) == 2:
        seen = {}
        for top, mid, half, act, o, s in iter_acts(d, bank):
            seen.setdefault((top, act), []).append(half)
        for (top, act), h in sorted(seen.items()):
            print('top %d act %d halves %s' % (top, act, '+'.join(h)))
        return 0
    top, act = int(argv[2]), int(argv[3])
    half = 'zlib'
    names = []
    frames = []
    ikfill = False
    for a in argv[4:]:
        if a in ('zlib', 'bpe'):
            half = a
        elif a == 'ikfill':
            ikfill = True
        elif a.startswith('f='):
            frames = [int(x) for x in a[2:].split(',')]
        else:
            names.append(a)
    layouts = image_layouts() if os.path.exists(IMAGE) else None
    for t, mid, h, a_id, o, s in iter_acts(d, bank):
        if (t, a_id, h) == (top, act, half):
            a = load_act(d, o, s)
            print('%s %s top %d act %d (%s): %d motions' % (pn, bank, top, act, half, len(a.yanm)))
            for i, p in enumerate(a.yanm):
                nm = a.motion_names[i] if i < len(a.motion_names) else '?'
                if names and nm not in names:
                    continue
                trs = parse_motion(a.blk, p, layouts)
                if ikfill and half == 'zlib':
                    for t2, m2, h2, a2, o2, s2 in iter_acts(d, bank):
                        if (t2, a2, h2) == (top, act, 'bpe'):
                            b = load_act(d, o2, s2)
                            if i < len(b.yanm):
                                print(' ikfill: %d b0 tracks took the BPE half rotation' %
                                      fill_ik_from_bpe(trs, parse_motion(b.blk, b.yanm[i], layouts)))
                print(' motion %s: %d tracks, %d frames' % (nm, len(trs), motion_length(trs)))
                if names:
                    for tr in trs:
                        print('   %-14s flags=0x%04x %s' % (hash_name(tr.hash) or hex(tr.hash), tr.flags, describe(tr)))
                    self_check(trs, nm)
                    for f in frames:
                        print('   frame %d' % f)
                        for tr in trs:
                            q = sample_rot(tr, f)
                            ps = sample_pos(tr, f)
                            print('     %-14s q=(%+.4f %+.4f %+.4f %+.4f)%s' % (
                                hash_name(tr.hash) or hex(tr.hash), q[0], q[1], q[2], q[3],
                                '' if ps is None else ' t=(%.3f %.3f %.3f)' % ps))
            return 0
    print('not found')
    return 1


if __name__ == '__main__':
    sys.exit(_main(sys.argv[1:]))
