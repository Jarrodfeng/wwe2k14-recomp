r"""Lane RJ (S101, docs\renderer\RING_PROMOTION_MAP.md section 7): the ring's meshes read straight from the player's own bgNN.pac files, in Python.
Read-only, 0 game runs. Mirrors tools\arena\arena_files.py (which it uses for the JBOY header, the strips and the DDS directories) and adds what the ring has
and the stage slots do not: the ring entries (950-999), the S1 skin pairs, the S2 UV, the S3 tangent, the bone-palette count.

  ring = load_ring(4)        # Ring: .entries {E: [RingMesh]}, .textures {name: Tex}, .ents (every PACH entry id), .pac_path
  mesh.positions / .normal / .colour_file   # file space as stored (BE f32 x3, BE f32 x3, 4 colour bytes in file order)
  mesh.skin_slot, mesh.skin_weight          # S1: one (LE u32 palette slot, BE f32 weight) pair per vertex (stride 8 only; else None)
  mesh.uv                                   # S2: BE f32 x2 (signed u as stored)
  mesh.tangent                              # S3: BE f32 x3, or None when the stream is absent
  mesh.palette                              # the mesh's bone palette (node indices), count at record +0x08
Nothing read here is ever written into the repo: callers keep results in memory or under out\."""
import collections
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import arena_files as af  # noqa: E402
import pac  # noqa: E402

RING_FIRST, RING_LAST = 950, 999
RING_SET = (951, 952, 953, 956, 960, 964, 973, 974, 980, 981, 982)  # the entries of a ring arena (RING_PROMOTION_MAP.md 1.1)
MESH_REC = 0xB8
be32 = af.be32


class RingMesh:
    __slots__ = ('entry', 'index', 'n', 'positions', 'normal', 'colour_file', 'skin_slot', 'skin_weight', 'uv', 'tangent', 'palette', 'effect', 'samplers',
                 'indices', 'group_counts', 'stream_index_count', 'stride_s1', 'stride_s3', 'ok')

    def __repr__(self):
        return '<RingMesh %d/%d n=%d idx=%d %s>' % (self.entry, self.index, self.n, self.stream_index_count, self.effect)

    @property
    def diffuse(self):
        for k in af.DIFFUSE_NAMES:
            if k in self.samplers:
                return self.samplers[k]
        return None


class Ring:
    def __init__(self, arena_id):
        self.id = arena_id
        self.entries = {}
        self.textures = {}
        self.ents = set()
        self.pac_path = ''

    def meshes(self):
        return [m for ms in self.entries.values() for m in ms]


def parse_ring_entry(entry, dec):
    """One JBOY leaf -> [RingMesh]. The common part (counts, strips, samplers, S0) is arena_files.parse_jboy; S1-S3 and the palette are read here."""
    base = af.parse_jboy(entry, dec)
    body = dec[8:8 + be32(dec, 4)]
    mtab = be32(body, 0x14)
    out = []
    for b in base:
        rec = mtab + b.index * MESH_REC
        n = b.n
        m = RingMesh()
        m.entry, m.index, m.n = entry, b.index, n
        m.effect, m.samplers, m.indices, m.group_counts, m.stream_index_count = b.effect, b.samplers, b.indices, b.group_counts, b.stream_index_count
        npal = be32(body, rec + 0x08)
        m.palette = [be32(body, rec + 0x0C + 4 * k) for k in range(min(npal, 20))]
        m.ok = bool(b.tiles and b.positions is not None)
        m.positions = b.positions
        m.normal = b.normal
        m.colour_file = b.colour
        m.skin_slot = m.skin_weight = m.uv = m.tangent = None
        m.stride_s1 = m.stride_s3 = 0
        if m.ok:
            s1, s2, s3, ib = (be32(body, rec + o) for o in (0x6C, 0x70, 0x74, 0x9C))
            m.stride_s1, m.stride_s3 = (s2 - s1) // n, (ib - s3) // n
            if m.stride_s1 in (8, 16, 24, 32):
                k = m.stride_s1 // 8
                sk = np.frombuffer(body, dtype=np.dtype([('s', '<u4'), ('w', '>f4')]), count=n * k, offset=s1)
                m.skin_slot, m.skin_weight = sk['s'].reshape(n, k).copy(), sk['w'].astype(np.float32).reshape(n, k)
            m.uv = np.frombuffer(body, dtype='>f4', count=2 * n, offset=s2).reshape(n, 2).astype(np.float32)
            if m.stride_s3 == 12:
                m.tangent = np.frombuffer(body, dtype='>f4', count=3 * n, offset=s3).reshape(n, 3).astype(np.float32)
        out.append(m)
    return out


_CACHE = {}


def pach_entries(arena_id):
    """-> (path, {entry id: raw bytes}) of bgNN.pac."""
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    if not os.path.exists(path):
        raise af.FileReadError('cannot read %s' % path)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    if data[p:p + 4] != b'PACH':
        raise af.FileReadError('no PACH in %s' % path)
    raw = {}
    for eid, b in pac.pach(data, p):
        raw.setdefault(eid, b)
    return path, raw


def load_ring(arena_id, textures=True):
    key = (arena_id, textures)
    if key in _CACHE:
        return _CACHE[key]
    path, raw = pach_entries(arena_id)
    r = Ring(arena_id)
    r.pac_path = path
    r.ents = set(raw)
    for eid in sorted(raw):
        if RING_FIRST <= eid <= RING_LAST:
            dec = pac.decode(raw[eid])
            if dec[:4] == b'JBOY':
                r.entries[eid] = parse_ring_entry(eid, dec)
    if textures:
        for dirid in (400, 402):
            if dirid in raw:
                for t in af.parse_tex_dir(pac.decode(raw[dirid]), dirid):
                    r.textures.setdefault(t.name.lower(), t)
    _CACHE[key] = r
    return r


def ring_census_all(ids=None):
    """The files-only census over every bgNN.pac: per arena the ring entry ids, a content hash per entry (blake2b of the decoded entry blob), the skin shape of every
    mesh (pairs per vertex; whether every used slot is below the palette count; whether the weights of a vertex sum to 1) and the meshes with a negative u. -> {arena: {...}}."""
    import hashlib
    out = {}
    for aid in (ids if ids is not None else af.arena_ids()):
        path, raw = pach_entries(aid)
        r = load_ring(aid, textures=False)
        row = {'entries': sorted(r.entries), 'content': {}, 'neg_u': {}, 'meshes': 0, 'unparsed': 0, 'pairs': {}, 'slot_over_palette': 0, 'weight_sum_bad': 0, 'two_pair': [], 'verts': 0, 'nongrey': 0, 'alpha_not_255': 0}
        for e, ms in r.entries.items():
            row['content'][e] = hashlib.blake2b(pac.decode(raw[e]), digest_size=10).hexdigest()
            for m in ms:
                row['meshes'] += 1
                if not m.ok or m.skin_slot is None:
                    row['unparsed'] += 1
                    continue
                k = m.skin_slot.shape[1]
                if k >= 2:
                    row['two_pair'].append((e, m.index))
                c = m.colour_file
                row['verts'] += len(c)
                row['nongrey'] += int(((c[:, 1] != c[:, 2]) | (c[:, 2] != c[:, 3])).sum())
                row['alpha_not_255'] += int((c[:, 0] != 255).sum())
                row['pairs'][k] = row['pairs'].get(k, 0) + 1
                used = m.skin_slot != 255
                if int(m.skin_slot[used].max(initial=0)) >= max(len(m.palette), 1):
                    row['slot_over_palette'] += 1
                if not np.allclose((m.skin_weight * used).sum(axis=1), 1.0, atol=1e-4):
                    row['weight_sum_bad'] += 1
                neg = int((m.uv[:, 0] < 0).sum())
                if neg:
                    row['neg_u'].setdefault(e, []).append((m.index, neg, m.n))
        out[aid] = row
    return out
