r"""Lane 39 (S101): the arena's meshes and textures read straight from the player's own pac files, in Python (read-only, 0 game runs).

An independent second reader of `pac\bg\bgNN.pac` (the C++ loader is src\assets\jboy.h; the layout comments there are the source of this reader, re-checked by
the census against the capture): per stage slot (PACH entries 0..399) the meshes in file space (positions as stored, no Y mirror), counts, the index-group rule,
the material class and the sampler names; the texture directories (entries 400 / 402; 401 holds the cube maps) as the plain DDS blobs the game memcpys.
Nothing read here is ever written into the repo: callers keep results in memory or under out\.

  arena = load_arena(4)            # Arena: .slots {slot: [FileMesh]}, .textures {name: Tex}, .flags {slot: set(tokens)}, .ents (entry ids)
  mesh.positions                   # float32 (N, 3), file space
  mesh.stream_index_count          # what the game's draw carries: sum(group counts) + (groups - 1)   [rule under test, see join_census.py]
"""
import hashlib
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'filecensus'))
import pac  # noqa: E402  (tools\wm30\pac.py: XCTD unpack, PACH walk, BPE / ZLIB decode)
import filecensus_flags  # noqa: E402  (parse_slot_flags: the 50001 text)

MESH_REC = 0xB8
DIFFUSE_NAMES = ('g_mDiffusePrm', 'sampDiffusePrm')
LIGHTMAP_NAME = 'sampLightMapPrm'


class FileReadError(Exception):
    """A pac / entry that cannot be read. Always loud: a census that reads nothing must fail."""


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _cstr(b):
    return bytes(b).split(b'\0')[0].decode('latin1')


class FileMesh:
    __slots__ = ('slot', 'index', 'n', 'positions', 'index_groups', 'group_counts', 'stream_index_count', 'effect', 'samplers', 'tiles', 'params', 'indices', 'stream_hash', 'uv0', 'uv1', 'normal', 'colour')

    def __repr__(self):
        return '<FileMesh slot %d mesh %d n=%d idx=%d %s>' % (self.slot, self.index, self.n, self.stream_index_count, self.effect)

    @property
    def diffuse(self):
        for k in DIFFUSE_NAMES:
            if k in self.samplers:
                return self.samplers[k]
        return None

    @property
    def lightmap(self):
        return self.samplers.get(LIGHTMAP_NAME)

    @property
    def is_lm(self):
        return self.effect.endswith('LM')


class Tex:
    __slots__ = ('name', 'blob', 'width', 'height', 'mips', 'fourcc', 'dir')

    @property
    def payload(self):
        return self.blob[128:]


class Arena:
    def __init__(self, arena_id):
        self.id = arena_id
        self.slots = {}
        self.textures = {}
        self.flags = {}
        self.ents = set()
        self.pac_path = ''


def parse_jboy(slot, blob, with_positions=True):
    """One JBOY leaf -> [FileMesh]. Layout: src\\assets\\jboy.h header comment. Meshes whose streams do not tile are returned with tiles=False and no positions."""
    if blob[:4] != b'JBOY':
        raise FileReadError('slot %d: not a JBOY' % slot)
    body = blob[8:8 + be32(blob, 4)]
    nmesh, mtab, ntex, ttab = be32(body, 0x10), be32(body, 0x14), be32(body, 0x1C), be32(body, 0x24)
    out = []
    for mi in range(nmesh):
        rec = mtab + mi * MESH_REC
        m = FileMesh()
        m.slot, m.index = slot, mi
        n = be32(body, rec)
        m.n = n
        s0, s1, s2, s3, ib = (be32(body, rec + o) for o in (0x68, 0x6C, 0x70, 0x74, 0x9C))
        d1, d2, d3 = s2 - s1, s3 - s2, ib - s3
        m.tiles = bool(n and s0 + 4 + 28 * n == s1 and d1 % n == 0 and d2 % n == 0 and d3 % n == 0 and d1 // n in (8, 16, 24, 32) and d2 // n == 8 and d3 // n in (0, 12))
        m.effect = _cstr(body[rec + 0x7C:rec + 0x7C + 16])
        m.samplers = {}
        m.params = {}
        nparam, plist = be32(body, rec + 0x94), be32(body, rec + 0x98)
        for k in range(min(nparam, 512)):
            po = be32(body, plist + 4 * k)
            pname = _cstr(body[po:po + 16])
            tw = be32(body, po + 16)
            if tw >> 16 == 0x0F:
                ti = be32(body, po + 20)
                if ti < ntex:
                    m.samplers[pname] = _cstr(body[ttab + 16 * ti:ttab + 16 * ti + 16])
            elif tw >> 16 == 0x0D:
                m.params[pname] = struct.unpack_from('>4f', body, po + 20)
            elif tw >> 16 == 0x0A:
                m.params[pname] = (struct.unpack_from('>f', body, po + 20)[0],)
        # index block: groups of {type (6 = strip), count, offset}, 12 bytes each, back to back until the first group's data offset
        first_ptr = be32(body, ib + 8)
        ng = (first_ptr - ib) // 12
        m.index_groups = ng
        m.group_counts = [be32(body, ib + 12 * g + 4) for g in range(ng)]
        m.stream_index_count = sum(m.group_counts) + max(ng - 1, 0)
        m.positions = None
        # the strips as the game draws them: BE u16, groups joined by one 0xFFFF restart marker (the rule under test in join_census.py)
        parts = []
        for g in range(ng):
            gp = be32(body, ib + 12 * g + 8)
            parts.append(np.frombuffer(body, dtype='>u2', count=m.group_counts[g], offset=gp).astype(np.uint32))
            if g + 1 < ng:
                parts.append(np.array([0xFFFF], np.uint32))
        m.indices = np.concatenate(parts) if parts else np.zeros(0, np.uint32)
        m.stream_hash, m.uv0, m.uv1, m.normal, m.colour = None, None, None, None, None
        if with_positions and m.tiles:
            # every non-position vertex byte the file holds (S0 normals + colour, S1, S2, S3), hashed: part of the interchangeability key
            h = hashlib.blake2b(digest_size=12)
            h.update(bytes(body[s0 + 4:s0 + 4 + 28 * n]))
            h.update(bytes(body[s1:s1 + d1 + d2 + d3]))
            m.stream_hash = h.hexdigest()
            st1 = d1 // n
            m.uv0 = np.frombuffer(body, dtype=np.dtype([('u', '>f4', 2), ('r', 'V%d' % (st1 - 8))]) if st1 > 8 else np.dtype([('u', '>f4', 2)]), count=n, offset=s1)['u'].astype(np.float32)
            m.uv1 = np.frombuffer(body, dtype=np.dtype('>f4'), count=2 * n, offset=s2).reshape(n, 2).astype(np.float32)
            m.normal = np.frombuffer(body, dtype=np.dtype([('p', '>f4', 3), ('n', '>f4', 3), ('r', 'V4')]), count=n, offset=s0 + 4)['n'].astype(np.float32)
            m.colour = np.frombuffer(body, dtype=np.dtype([('r', 'V24'), ('c', 'u1', 4)]), count=n, offset=s0 + 4)['c'].copy()
            a = np.frombuffer(body, dtype=np.dtype([('p', '>f4', 3), ('r', 'V16')]), count=n, offset=s0 + 4)
            m.positions = a['p'].astype(np.float32)
        out.append(m)
    return out


def parse_tex_dir(blob, directory):
    """A texture directory: LE32 count, 12 header bytes, count x 32-byte records {name[16], 'dds\\0', LE32 size, LE32 offset, 0}; each blob a plain LE DDS."""
    cnt = struct.unpack_from('<I', blob, 0)[0]
    out = []
    for i in range(cnt):
        r = 16 + 32 * i
        name = _cstr(blob[r:r + 16])
        size, off = struct.unpack_from('<II', blob, r + 20)
        data = bytes(blob[off:off + size])
        t = Tex()
        t.name, t.blob, t.dir = name, data, directory
        if data[:4] != b'DDS ':
            raise FileReadError('texture %s: not a DDS' % name)
        t.height, t.width = struct.unpack_from('<II', data, 12)
        t.mips = struct.unpack_from('<I', data, 28)[0] if struct.unpack_from('<I', data, 8)[0] & 0x20000 else 1
        t.fourcc = data[84:88]
        out.append(t)
    return out


_CACHE = {}


def arena_ids():
    d = os.path.join(pac.GAME, 'pac', 'bg')
    ids = []
    for f in sorted(os.listdir(d)):
        if f.startswith('bg') and f.endswith('.pac') and f[2:-4].isdigit():
            ids.append(int(f[2:-4]))
    if not ids:
        raise FileReadError('no bgNN.pac under %s (a census that reads nothing must fail)' % d)
    return ids


def load_arena(arena_id, with_positions=True):
    key = (arena_id, with_positions)
    if key in _CACHE:
        return _CACHE[key]
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    if not os.path.exists(path):
        raise FileReadError('cannot read %s' % path)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    if data[p:p + 4] != b'PACH':
        raise FileReadError('no PACH in %s' % path)
    a = Arena(arena_id)
    a.pac_path = path
    raw = {}
    for eid, b in pac.pach(data, p):
        raw.setdefault(eid, b)
    a.ents = set(raw)
    for eid in sorted(raw):
        if 0 <= eid < 400:
            dec = pac.decode(raw[eid])
            if dec[:4] == b'JBOY':
                a.slots[eid] = parse_jboy(eid, dec, with_positions)
    for dirid in (400, 402):
        if dirid in raw:
            for t in parse_tex_dir(pac.decode(raw[dirid]), dirid):
                a.textures.setdefault(t.name.lower(), t)
    if 50001 in raw:
        a.flags = filecensus_flags.parse_slot_flags(pac.decode(raw[50001]).decode('latin1'))
    if not a.slots:
        raise FileReadError('%s: no stage slot parsed' % path)
    _CACHE[key] = a
    return a
