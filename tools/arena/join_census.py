r"""Lane 39 (S101, lane J of docs\renderer\ARENA_PROMOTION_MAP.md section 6): the arena join census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's sections 1-2 on case zips / capture folders, against the arena read straight from the player's own
files (tools\arena\arena_files.py): which stream draws are stage-slot draws, what object -> (slot, mesh) the content says, whether the join is unique, whether the
object address is stable, whether 0x82C0B94C is the mirrored copy, the index-group rule, the texture bytes, and how the scene record agrees.

  py -I tools\arena\join_census.py CAPTURE[:ARENA] [CAPTURE[:ARENA] ...] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--expect FILE] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder (frames\ beside geometry.bin). ARENA = the bg id (default: found by the join rate).
Checks (each prints PASS / FAIL with counts; the exit code is 1 when any FAILs or an input cannot be read):
  names          the draws carry their origin (version-2 names): stage-site model-part draws exist.
  part_is_mesh   the joined mesh index equals the draw's part index (every joined draw).
  group_rule     the draws a content scan (index count ignored) joins are exactly the draws the counts join with the index-group rule (a mesh of g groups draws
                 sum + (g - 1) indices); the rule explains every one.
  join_unique    per object: one slot after the parts are intersected and the diffuse texture bytes break ties; a left-over tie must be interchangeable meshes.
  index_content  the stream's index values equal the file's strips (groups joined by one restart marker).
  stability      one object address per slot per copy in every frame, the same address for the slot in every frame of the capture.
  mirror         the 0x82C0B94C draws: ViewProj with x negated against 0x82C0A950's (every frame), only `r` flagged slots, slots drawn through both are `r`.
  texture_bytes  each joined draw's slot-0 image content equals the file's diffuse DDS payload.
  record         (zips with scene records) every drawn stage slot is in the record; every mirrored draw's slot has its `:r` twin; the record's arena is the file's;
                 gaps are listed (slot missing, twin missing, world differs) and compared with the expected set in expect.json.
--plant (tests only): join_shift (counts-only join taking the next candidate), no_group_rule (index counts without the +g-1), no_mirror (both sites read as one),
no_names (origins stripped, a version-1 capture), no_flags (the `r` flags dropped from the file side).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import arena_files as af  # noqa: E402
import capread  # noqa: E402

SITE_A, SITE_B = 0x82C0A950, 0x82C0B94C
CLASS_MODEL_PART = 1
PLANTS = ('join_shift', 'no_group_rule', 'no_mirror', 'no_names', 'no_flags', 'tex_flip')

LAY_DT = np.dtype([('first', '<u4'), ('count', '<u4'), ('stride', '<u4'), ('gstride', '<u4')])
ELE_DT = np.dtype([('fmt', '<u2'), ('off', '<u2'), ('src', 'u1'), ('comp', 'u1'), ('flags', 'u1'), ('mini', 'u1'), ('goff', '<i4')])
CONT_DT = np.dtype([('image', '<u4'), ('pad', '<u4'), ('off', '<u8'), ('size', '<u8')])
SEC_LAYOUTS, SEC_ELEMENTS, SEC_CONTENTS = 11, 12, 15
VK_R32G32B32_SFLOAT, VK_R32G32B32A32_SFLOAT = 106, 109
DRAW_NO_GEOMETRY = 8  # adapter_frame.h kDrawNoGeometry


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()


class CensusError(Exception):
    """An input that cannot be read. Never a pass."""


# ---------------------------------------------------------------- the capture ------------------------------------------------------------------------
class Capture:
    def __init__(self, path):
        self.path = path
        self.zf = None
        if str(path).lower().endswith('.zip'):
            self.zf = zipfile.ZipFile(path)
            names = self.zf.namelist()
            self.frames = sorted(n for n in names if re.search(r'(^|/)frames/frame_\d+\.bin$', n))
            self.scenes = {int(re.search(r'frame_(\d+)', n).group(1)): n for n in names if n.endswith('.rsc')}
            self.geo_name = next((n for n in names if n.endswith('geometry.bin')), None)
            self.tex_name = next((n for n in names if n.endswith('textures.bin')), None)
        else:
            base = path
            self.frames = sorted(os.path.join(base, 'frames', f) for f in os.listdir(os.path.join(base, 'frames')) if f.startswith('frame_')) if os.path.isdir(os.path.join(base, 'frames')) else []
            sd = os.path.join(base, 'scene')
            self.scenes = {int(re.search(r'frame_(\d+)', f).group(1)): os.path.join(sd, f) for f in os.listdir(sd) if f.endswith('.rsc')} if os.path.isdir(sd) else {}
            self.geo_name, self.tex_name = os.path.join(base, 'geometry.bin'), os.path.join(base, 'textures.bin')
        if not self.frames:
            raise CensusError('%s: no frame files (a census that reads nothing must fail)' % path)
        self._geo = self._tex = None
        self.contents = {}  # image id -> (offset, size) in textures.bin, the latest content seen so far (frames are visited in order)
        self._seen_to = -1

    def read(self, name):
        return self.zf.read(name) if self.zf is not None else open(name, 'rb').read()

    @property
    def geo(self):
        if self._geo is None:
            if not self.geo_name:
                raise CensusError('%s: no geometry.bin' % self.path)
            self._geo = np.frombuffer(self.read(self.geo_name), np.uint8)
        return self._geo

    @property
    def tex(self):
        if self._tex is None:
            if not self.tex_name:
                raise CensusError('%s: no textures.bin' % self.path)
            self._tex = np.frombuffer(self.read(self.tex_name), np.uint8)
        return self._tex

    def frame_number(self, i):
        return int(re.search(r'frame_(\d+)', self.frames[i]).group(1))

    def frame(self, i, plant=None):
        buf = self.read(self.frames[i])
        f = capread.Frame(buf)
        magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
        secs = {}
        for k in range(nsec):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
            secs[sid] = (es, cnt, off)

        def tab(sid, dt):
            if sid not in secs or secs[sid][0] != dt.itemsize:
                return np.zeros(0, dt)
            es, cnt, off = secs[sid]
            return np.frombuffer(buf, dt, cnt, off)
        f.layouts, f.elements, f.contents_tab = tab(SEC_LAYOUTS, LAY_DT), tab(SEC_ELEMENTS, ELE_DT), tab(SEC_CONTENTS, CONT_DT)
        f.number = self.frame_number(i)
        if plant == 'no_names':
            f.has_origins, f.origins = False, None
        return f

    def _apply(self, buf):
        magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
        for k in range(nsec):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
            if sid == SEC_CONTENTS and es == CONT_DT.itemsize:
                for c in np.frombuffer(buf, CONT_DT, cnt, off):
                    self.contents[int(c['image'])] = (int(c['off']), int(c['size']))

    def catch_up(self, upto):
        """A sampled run (--frames) still sees the contents the skipped frames uploaded: apply them first."""
        for j in range(self._seen_to + 1, upto):
            self._apply(self.read(self.frames[j]))
        self._seen_to = max(self._seen_to, upto - 1)

    def advance_contents(self, f, idx):
        for c in f.contents_tab:
            self.contents[int(c['image'])] = (int(c['off']), int(c['size']))
        self._seen_to = idx

    def image_data(self, image):
        """The texel data of an image's latest content blob (textures.bin: RCTX header, subresource table, data), or None."""
        c = self.contents.get(image)
        if c is None:
            return None
        blob = self.tex[c[0]:c[0] + c[1]]
        if blob.size < 24 or bytes(blob[:4]) != b'RCTX':
            return None
        _, _, cnt, _, db = struct.unpack_from('<IIIIQ', blob.tobytes()[:24], 0)
        start = 24 + 32 * cnt
        subs = [struct.unpack_from('<IIIIIIQ', blob.tobytes()[24 + 32 * k:24 + 32 * k + 32]) for k in range(cnt)]  # mip, layer, w, h, row_bytes, rows, offset
        return TexData(bytes(blob[start:start + db]), subs)


class TexData(bytes):
    """The capture's texel data of one image (a bytes object) plus its subresource table."""

    def __new__(cls, data, subs):
        o = super().__new__(cls, data)
        o.subs = subs
        return o


BLOCK_BYTES = {b'DXT1': 8, b'DXT3': 16, b'DXT5': 16}


def compare_texture(got, tex):
    """The capture's image data against the file's DDS payload, per mip: 'equal' (every captured mip equal), 'tail' (only mips of at most 8 texels, never mip 0, differ:
    the game's small-mip layout, not understood [U]), 'padded' (the captured image is bigger than the file's AND every overlapping block row equals the file's),
    'padded_differ' (bigger image whose overlapping rows do NOT equal the file's: a different picture, cause unknown [U]), 'differ' (anything else)."""
    bb = BLOCK_BYTES.get(tex.fourcc)
    if bb is None:
        return 'equal' if tex.payload[:len(got)] == got else 'differ'
    offs, o = [], 0
    for k in range(tex.mips):
        w, h = max(tex.width >> k, 1), max(tex.height >> k, 1)
        offs.append((o, w, h))
        o += ((w + 3) // 4) * ((h + 3) // 4) * bb
    bad, padded = [], False
    for (mip, layer, w, h, rb, rows, off) in got.subs:
        if mip >= len(offs):
            return 'differ'
        fo, fw, fh = offs[mip]
        if (w, h) != (fw, fh):
            frb, frows = ((fw + 3) // 4) * bb, (fh + 3) // 4
            for r in range(min(rows, frows)):
                n = min(rb, frb)
                if tex.payload[fo + r * frb:fo + r * frb + n] != got[off + r * rb:off + r * rb + n]:
                    return 'padded_differ'
            padded = True
            continue
        size = rb * rows
        if tex.payload[fo:fo + size] != got[off:off + size]:
            bad.append((mip, max(w, h)))
    if bad:
        return 'tail' if all(m > 0 and big <= 8 for m, big in bad) and not padded else 'differ'
    return 'padded' if padded else 'equal'


def draw_float_elements(cap, f, i):
    """The stream draw's float3 / float4 vertex elements as [(element index, (n, 3) float32)], and the index values (uint32, restart -> 0xFFFF; None if not indexed).
    Which element is the position is not assumed: the census finds the one that equals a file mesh (the map says element 0 [L])."""
    d = f.draws[i]
    L = f.layouts[int(d['layout'])]
    n, stride = int(d['vertex_count']), int(L['stride'])
    raw = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
    out = []
    if raw.size >= n * stride and n:
        rows = raw[:n * stride].reshape(n, stride)
        for k in range(int(L['count'])):
            e = f.elements[int(L['first']) + k]
            if int(e['fmt']) in (VK_R32G32B32_SFLOAT, VK_R32G32B32A32_SFLOAT):
                out.append((k, np.ascontiguousarray(rows[:, int(e['off']):int(e['off']) + 12]).view('<f4').reshape(n, 3)))
    idx = None
    if int(d['index_bytes']):
        dt = np.dtype('<u4') if int(d['index_32bit']) else np.dtype('<u2')
        idx = cap.geo[int(d['index_offset']):int(d['index_offset']) + int(d['index_count']) * dt.itemsize].view(dt).astype(np.uint32)
        idx[idx == 0xFFFFFFFF] = 0xFFFF
    return out, idx


def draw_attr_arrays(cap, f, i):
    """Every vertex element of the draw as an (n, 2) float array of its first two components (half, 16-bit snorm and float elements), for tie-breaking by the file's UV sets."""
    d = f.draws[i]
    L = f.layouts[int(d['layout'])]
    n, stride = int(d['vertex_count']), int(L['stride'])
    raw = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
    out = []
    if raw.size < n * stride or not n:
        return out
    rows = raw[:n * stride].reshape(n, stride)
    for k in range(int(L['count'])):
        e = f.elements[int(L['first']) + k]
        fmt, off = int(e['fmt']), int(e['off'])
        if fmt == 83:
            out.append(np.ascontiguousarray(rows[:, off:off + 4]).view('<f2').reshape(n, 2).astype(np.float32))
        elif fmt == 92:
            out.append(np.ascontiguousarray(rows[:, off:off + 8]).view('<i2').reshape(n, 4)[:, :2].astype(np.float32) / 32767.0)
        elif fmt in (VK_R32G32B32_SFLOAT, VK_R32G32B32A32_SFLOAT):
            out.append(np.ascontiguousarray(rows[:, off:off + 8]).view('<f4').reshape(n, 2))
    return out


def draw_colour_arrays(cap, f, i):
    """Every 4-byte (8888) element of the draw as (n, 4) uint8 (component order not assumed by the caller)."""
    d = f.draws[i]
    L = f.layouts[int(d['layout'])]
    n, stride = int(d['vertex_count']), int(L['stride'])
    raw = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
    out = []
    if raw.size < n * stride or not n:
        return out
    rows = raw[:n * stride].reshape(n, stride)
    for k in range(int(L['count'])):
        e = f.elements[int(L['first']) + k]
        if int(e['fmt']) in (37, 38, 41, 42):
            out.append(np.ascontiguousarray(rows[:, int(e['off']):int(e['off']) + 4]))
    return out


def colour_matches(mesh, arrays):
    """The mesh's vertex colour word equals some 8888 element of the draw up to byte order (each vertex's four bytes as a multiset)."""
    if mesh.colour is None:
        return False
    want = np.sort(mesh.colour, axis=1)
    return any(a.shape == want.shape and np.array_equal(np.sort(a, axis=1), want) for a in arrays)


def uv_matches(mesh, arrays):
    """The mesh's two UV sets each equal (half precision) the first two components of some element of the draw."""
    for uv in (mesh.uv0, mesh.uv1):
        if uv is None or not any(a.shape == uv.shape and np.allclose(a, uv, atol=2e-2, rtol=2e-3) for a in arrays):
            return False
    return True


# ---------------------------------------------------------------- the file side ----------------------------------------------------------------------
class FileIndex:
    """Count -> meshes of one arena, with the index-group rule on or off; content fingerprints for interchangeability."""

    def __init__(self, arena, plant=None):
        self.arena = arena
        self.rule = plant != 'no_group_rule'
        self.by_n = collections.defaultdict(list)       # vertex count -> meshes
        self.by_counts = collections.defaultdict(list)  # (n, index count as drawn) -> meshes
        for k, ms in arena.slots.items():
            for m in ms:
                if m.positions is None:
                    continue
                self.by_n[m.n].append(m)
                ic = m.stream_index_count if self.rule else sum(m.group_counts)
                self.by_counts[(m.n, ic)].append(m)
        self.fp = {}

    def fingerprint(self, m):
        k = (m.slot, m.index)
        if k not in self.fp:
            h = hashlib.blake2b(digest_size=12)
            h.update(m.positions.tobytes())
            h.update(m.indices.tobytes())
            h.update(m.effect.encode())
            h.update((m.stream_hash or '').encode())
            h.update(repr(sorted(m.params.items())).encode())
            for key in sorted(m.samplers):
                t = self.arena.textures.get(m.samplers[key].lower())
                h.update(key.encode() + b'=' + (hashlib.blake2b(t.blob, digest_size=8).digest() if t else m.samplers[key].encode()))
            self.fp[k] = h.hexdigest()
        return self.fp[k]


def diffuse_tex(arena, m):
    n = m.diffuse
    return arena.textures.get(n.lower()) if n else None


def diffuse_payload(arena, m):
    n = m.diffuse
    t = arena.textures.get(n.lower()) if n else None
    return t.payload if t else None


def match_draw(cap, f, i, fi, cache):
    """-> dict: cands (meshes with the drawn vertex / index counts, index-group rule per fi.rule), hits (cands whose positions equal one float element of the draw),
    scan (meshes with the vertex count whose positions equal one element, index count ignored), p (the matched element's positions, else None), elem (its index)."""
    d = f.draws[i]
    L = f.layouts[int(d['layout'])]
    sig = tuple((int(e['fmt']), int(e['off'])) for e in f.elements[int(L['first']):int(L['first']) + int(L['count'])])
    key = (int(d['vertex_offset']), int(d['vertex_bytes']), int(d['index_offset']), int(d['index_count']), int(d['vertex_count']), int(L['stride']), sig)
    if key in cache:
        return cache[key]
    n, ic = int(d['vertex_count']), int(d['index_count'])
    if int(d['flags']) & DRAW_NO_GEOMETRY:  # the capture holds no vertex bytes for this draw (its bytes are not the game's): never read them
        res = {'p': None, 'elem': None, 'idx': None, 'cands': [], 'hits': [], 'scan': [], 'maxdiff': 0.0, 'nogeo': True}
        cache[key] = res
        return res
    els, idx = draw_float_elements(cap, f, i)
    res = {'p': None, 'elem': None, 'idx': idx, 'cands': list(fi.by_counts.get((n, ic), [])), 'hits': [], 'scan': [], 'maxdiff': 0.0}

    def equal(m, p):
        return m.positions.shape == p.shape and np.array_equal(m.positions, p)
    for k, p in els:
        hits = [m for m in res['cands'] if equal(m, p)]
        scan = [m for m in fi.by_n.get(n, []) if equal(m, p)]
        if hits or scan:
            res['p'], res['elem'], res['hits'], res['scan'] = p, k, hits, scan
            res['maxdiff'] = max([float(np.abs(m.positions - p).max()) for m in hits] or [0.0])
            assert res['maxdiff'] == 0.0
            break
    cache[key] = res
    return res


# ---------------------------------------------------------------- the scene record --------------------------------------------------------------------
RSC_OBJ = re.compile(r'^\s+object \d+: id "arena:slot:(\d+)(:r)?" asset (\d+) class (\d+) flags (0x[0-9a-f]+) pos \(([-0-9.e+]+) ([-0-9.e+]+) ([-0-9.e+]+)\)')
RSC_ASSET = re.compile(r'^\s+asset (\d+): kind (\d+) pac "([^"]*)" entry "([^"]*)"')
RSC_HEAD = re.compile(r'guest_frame (\d+)')


def rsc_dump_exe():
    for base in (ROOT, MAIN):
        p = os.path.join(base, 'out', 'tools', 'rsc_dump.exe')
        if os.path.isfile(p):
            return p
    raise CensusError('out\\tools\\rsc_dump.exe not found (build: pwsh -NoProfile -File tools\\scene\\build_rsc.ps1)')


def read_record(cap, number, tmpdir):
    name = cap.scenes.get(number)
    if name is None:
        return None
    path = os.path.join(tmpdir, 'f%06d.rsc' % number)
    with open(path, 'wb') as fh:
        fh.write(cap.read(name))
    p = subprocess.run([rsc_dump_exe(), path], capture_output=True, text=True, timeout=120)
    os.remove(path)
    if p.returncode != 0:
        raise CensusError('rsc_dump failed on frame %d: %s' % (number, p.stderr.strip()[:200]))
    rec = {'slots': {}, 'twins': {}, 'guest': None, 'pacs': set()}
    assets = {}
    for line in p.stdout.splitlines():
        m = RSC_ASSET.match(line)
        if m:
            assets[int(m.group(1))] = (m.group(3), m.group(4))
            continue
        m = RSC_HEAD.search(line)
        if m and rec['guest'] is None and 'version' in line:
            rec['guest'] = int(m.group(1))
        m = RSC_OBJ.match(line)
        if m:
            k = int(m.group(1))
            pos = tuple(float(m.group(j)) for j in (6, 7, 8))
            (rec['twins'] if m.group(2) else rec['slots'])[k] = pos
            if int(m.group(3)) in assets:
                rec['pacs'].add(assets[int(m.group(3))][0])
    return rec


# ---------------------------------------------------------------- the census -------------------------------------------------------------------------
class Result:
    def __init__(self):
        self.checks = collections.OrderedDict()   # name -> [ok, detail]
        self.numbers = collections.OrderedDict()
        self.lists = collections.OrderedDict()

    def check(self, name, ok, detail):
        prev = self.checks.get(name)
        if prev and not prev[0]:
            return
        self.checks[name] = [bool(ok), detail]


def detect_arena(cap, frames, plant, verbose):
    """The arena whose meshes the stage draws join best (counts + positions) on a few frames of the capture."""
    best = (0, None)
    sample = frames[::max(1, len(frames) // 4)][:4]
    cands = []
    for aid in af.arena_ids():
        try:
            cands.append((aid, FileIndex(af.load_arena(aid), plant)))
        except Exception as e:  # an arena that does not read is skipped here, the census of the chosen one is loud
            if verbose:
                print('  detect: bg%02d unreadable (%s)' % (aid, e))
    for aid, fi in cands:
        joined = 0
        cache = {}
        for fn in sample:
            f = cap.frame(fn, plant)
            if not f.has_origins:
                continue
            for i in range(len(f.draws)):
                o = f.origins[i]
                if o['klass'] == CLASS_MODEL_PART and int(o['pass_site']) in (SITE_A, SITE_B) and match_draw(cap, f, i, fi, cache)['hits']:
                    joined += 1
        if joined > best[0]:
            best = (joined, aid)
    return best[1]


def census(cap, arena_id, frame_sel, plant, verbose, expect):
    R = Result()
    t0 = time.time()
    if arena_id is None:
        arena_id = detect_arena(cap, frame_sel, plant, verbose)
        if arena_id is None:
            raise CensusError('%s: no arena joins any stage draw (names missing, or the capture is not a match)' % cap.path)
    arena = af.load_arena(arena_id)
    if plant == 'no_flags':
        arena.flags = {}
    if plant == 'tex_flip':
        import copy
        arena = copy.copy(arena)
        arena.textures = dict(arena.textures)
        for nm, t in list(arena.textures.items()):
            t2 = copy.copy(t)
            bl = bytearray(t.blob)
            bl[128] ^= 0xFF  # first byte of mip 0
            t2.blob = bytes(bl)
            arena.textures[nm] = t2
    fi = FileIndex(arena, plant)
    R.numbers['arena'] = arena_id
    R.numbers['frames'] = len(frame_sel)
    cache = {}
    tmpdir = tempfile.mkdtemp(prefix='arena_join_l39_')

    C = collections.Counter()
    maxdiff = 0.0
    obj_part = {}                       # (obj, site) -> {part: running intersection of the slot sets over the capture}
    obj_fp = collections.defaultdict(dict)   # (obj, site) -> {part: {slot: fingerprint}}
    obj_reused = set()                  # an address whose slot set became empty: reused for another object
    frame_objs = {}                     # frame number -> {(obj, site): world translation (x, y, z) or None}
    mirror_fail, scan_mismatch, content_bad = [], [], []
    mirror_frames = 0
    records = {}                        # frame number -> record dict
    no_names_frame = None

    for fn in frame_sel:
        cap.catch_up(fn)
        f = cap.frame(fn, plant)
        cap.advance_contents(f, fn)
        if not f.has_origins:
            no_names_frame = f.number
            break
        o, d = f.origins, f.draws
        stage = [i for i in range(len(d)) if o[i]['klass'] == CLASS_MODEL_PART and int(o[i]['pass_site']) in (SITE_A, SITE_B)]
        C['stage_site_draws'] += len(stage)
        fo = frame_objs[f.number] = {}
        vps = collections.defaultdict(dict)
        for i in stage:
            site = SITE_A if plant == 'no_mirror' else int(o[i]['pass_site'])
            obj, part = int(o[i]['object']), int(o[i]['part'])
            vps[site][f.reg(int(d[i]['vs_bank']), 56, 4).tobytes()] = f.reg(int(d[i]['vs_bank']), 56, 4).copy()
            m = match_draw(cap, f, i, fi, cache)
            if m.get('nogeo'):
                C['draws_no_geometry_captured'] += 1
                continue
            if {(x.slot, x.index) for x in m['scan']} != {(x.slot, x.index) for x in m['hits']}:
                scan_mismatch.append((f.number, hex(obj), part))
            if not m['cands']:
                C['draws_no_count_candidate'] += 1
                continue
            hits = m['hits']
            if plant == 'join_shift':
                # the counts-only join, taking the next candidate: the mesh drawn is not looked at
                k0 = m['cands'].index(hits[0]) if hits else -1
                hits = [m['cands'][(k0 + 1) % len(m['cands'])]]
            if not hits:
                C['draws_counts_but_no_content'] += 1
                continue
            C['draws_joined'] += 1
            C['position_element_%s' % ('0' if m['elem'] == 0 else 'other')] += 1
            hp = [x for x in hits if x.index == part]
            if not hp:
                C['part_is_not_mesh'] += 1
                continue
            hits = hp
            tex_img = next((int(s['image']) for s in f.slots[int(d[i]['tex_first']):int(d[i]['tex_first']) + int(d[i]['tex_count'])] if int(s['slot']) == 0), 0)
            chosen = hits
            if len(hits) > 1 and tex_img:
                payload = cap.image_data(tex_img)
                if payload is not None:
                    keep = [x for x in hits if diffuse_tex(arena, x) is not None and compare_texture(payload, diffuse_tex(arena, x)) in ('equal', 'tail', 'padded')]
                    if keep and len(keep) < len(hits):
                        chosen = keep
                        C['ties_resolved_by_texture'] += 1
            if len(chosen) > 1:  # still tied: the file's UV sets against the draw's other elements
                arrs = draw_attr_arrays(cap, f, i)
                keep = [y for y in chosen if uv_matches(y, arrs)]
                if keep and len(keep) < len(chosen):
                    chosen = keep
                    C['ties_resolved_by_uv_streams'] += 1
                if len(chosen) > 1:
                    cols = draw_colour_arrays(cap, f, i)
                    keep = [y for y in chosen if colour_matches(y, cols)]
                    if keep and len(keep) < len(chosen):
                        chosen = keep
                        C['ties_resolved_by_vertex_colour'] += 1
            x = chosen[0]
            if m['p'] is not None and not np.array_equal(m['p'], x.positions):
                content_bad.append((f.number, hex(obj), part))
            else:
                maxdiff = max(maxdiff, m['maxdiff'])
            if x.index_groups > 1:
                C['joined_draws_of_multi_group_meshes'] += 1
            if m['idx'] is not None:
                if m['idx'].shape == x.indices.shape and np.array_equal(m['idx'], x.indices):
                    C['index_equal'] += 1
                else:
                    C['index_differ'] += 1
            dt = diffuse_tex(arena, x)
            got = cap.image_data(tex_img) if tex_img else None
            if dt is None:
                C['tex_no_file_texture'] += 1
            elif got is None:
                C['tex_no_capture_content'] += 1
            else:
                C['tex_' + compare_texture(got, dt)] += 1
            key = (obj, site)
            sset = {y.slot for y in chosen}
            prev = obj_part.setdefault(key, {})
            if part in prev:
                prev[part] &= sset
                if not prev[part]:
                    obj_reused.add(key)
            else:
                prev[part] = set(sset)
            for y in chosen:
                obj_fp[key].setdefault(part, {})[y.slot] = fi.fingerprint(y)
            if key not in fo:
                w = f.reg(int(d[i]['vs_bank']), 76, 3)
                fo[key] = (float(w[0, 3]), float(w[1, 3]), float(w[2, 3]))
        # the two sites' ViewProj
        if plant == 'no_mirror':
            merged = {}
            for v in vps.values():
                merged.update(v)
            if len(merged) > 1:
                mirror_fail.append((f.number, 'one site holds %d different ViewProj' % len(merged)))
            mirror_frames += 1 if len(merged) > 1 else 0
        elif SITE_A in vps and SITE_B in vps:
            mirror_frames += 1
            if len(vps[SITE_A]) != 1 or len(vps[SITE_B]) != 1:
                mirror_fail.append((f.number, 'ViewProj not unique per site: %d / %d' % (len(vps[SITE_A]), len(vps[SITE_B]))))
            else:
                a, b = next(iter(vps[SITE_A].values())), next(iter(vps[SITE_B].values()))
                if not (np.array_equal(b[:, 0], -a[:, 0]) and np.array_equal(b[:, 1:], a[:, 1:])):
                    mirror_fail.append((f.number, 'second site ViewProj is not the first with x negated'))
        if cap.scenes:
            rec = read_record(cap, f.number, tmpdir)
            if rec is not None:
                rec['guest_frame'] = f.guest_frame
                records[f.number] = rec
    try:
        os.rmdir(tmpdir)
    except OSError:
        pass

    if no_names_frame is not None or C['stage_site_draws'] == 0:
        R.check('names', False, ('frame %d has no origin section (a version-1 capture is unnamed)' % no_names_frame) if no_names_frame is not None
                else 'no stage-site model-part draws over %d frames' % len(frame_sel))
        return R, arena_id, time.time() - t0
    R.check('names', True, '%d stage-site model-part draws over %d frames' % (C['stage_site_draws'], len(frame_sel)))
    R.numbers.update(C)
    R.numbers['max_position_diff'] = maxdiff
    R.check('content_equal', not content_bad and C['draws_joined'] > 0, '%d joined draws whose mesh positions differ from the stream (of %d joined)' % (len(content_bad), C['draws_joined']))
    R.check('part_is_mesh', C['part_is_not_mesh'] == 0, '%d joined draws whose mesh index is not the draw part (of %d joined)' % (C['part_is_not_mesh'], C['draws_joined']))
    R.check('group_rule', not scan_mismatch and (C['joined_draws_of_multi_group_meshes'] > 0 or not any(m.index_groups > 1 for ms in arena.slots.values() for m in ms)),
            '%d draws joined by content (index count ignored) are not the draws the counts join (rule %s); %d joined draws are meshes of more than one index group'
            % (len(scan_mismatch), 'on' if fi.rule else 'OFF', C['joined_draws_of_multi_group_meshes']))
    R.lists['group_rule_mismatches'] = scan_mismatch[:10]

    # the join per object address: the slots every drawn part agrees on
    final = {}
    uniq = tied_eq = distinct = 0
    amb = []
    for key, pm in obj_part.items():
        inter = set.intersection(*pm.values()) if pm else set()
        final[key] = inter
        if key in obj_reused or not inter:
            distinct += 1
            amb.append((hex(key[0]), key[1] == SITE_B, 'no slot fits every part / address reused'))
        elif len(inter) == 1:
            uniq += 1
        else:
            same = all(len({obj_fp[key][p][s] for s in inter if s in obj_fp[key][p]}) <= 1 for p in pm)
            if same:
                tied_eq += 1
                amb.append((hex(key[0]), key[1] == SITE_B, 'interchangeable slots %s' % sorted(inter)))
            else:
                distinct += 1
                amb.append((hex(key[0]), key[1] == SITE_B, 'DISTINCT meshes tie: %s' % sorted(inter)))
    R.numbers.update(objects=len(obj_part), objects_unique=uniq, objects_tied_interchangeable=tied_eq, objects_tied_distinct_or_none=distinct)
    R.lists['tied_objects'] = amb[:30]
    R.check('join_unique', distinct == 0 and uniq > 0,
            '%d objects: %d join to exactly one slot, %d tie between slots whose meshes are identical (class only), %d not unique and not interchangeable'
            % (len(obj_part), uniq, tied_eq, distinct))
    R.check('index_content', C['index_differ'] == 0 and C['index_equal'] > 0, 'stream indices equal the file strips on %d joined draws, differ on %d' % (C['index_equal'], C['index_differ']))

    # stability
    slot_objs = collections.defaultdict(set)   # (slot, site) -> objects joined uniquely to it
    for key, inter in final.items():
        if len(inter) == 1 and key not in obj_reused:
            slot_objs[(next(iter(inter)), key[1])].add(key[0])
    multi_addr = {k: sorted(hex(a) for a in v) for k, v in slot_objs.items() if len(v) > 1}
    two_in_frame = []
    for fnum, fo in frame_objs.items():
        per = collections.defaultdict(set)
        for key in fo:
            inter = final.get(key, set())
            if len(inter) == 1 and key not in obj_reused:
                per[(next(iter(inter)), key[1])].add(key[0])
        for k, v in per.items():
            if len(v) > 1:
                two_in_frame.append((fnum, k[0], k[1] == SITE_B))
    R.numbers.update(slot_copies_joined_uniquely=len(slot_objs), slot_copies_with_several_addresses=len(multi_addr), addresses_reused=len(obj_reused),
                     frame_slot_copies_on_two_objects=len(two_in_frame))
    R.lists['slot_copies_with_several_addresses'] = {('%d%s' % (k[0], ':r' if k[1] == SITE_B else '')): v for k, v in list(multi_addr.items())[:20]}
    R.check('stability', not multi_addr and not obj_reused and not two_in_frame and len(slot_objs) > 0,
            '%d slot copies joined uniquely; %d with more than one object address over the capture; %d addresses changed slot; %d (frame, slot copy) on two objects'
            % (len(slot_objs), len(multi_addr), len(obj_reused), len(two_in_frame)))

    # the mirror
    r_viol = []
    both = set()
    a_k = {k[0] for k in slot_objs if k[1] == SITE_A}
    b_k = {k[0] for k in slot_objs if k[1] == SITE_B}
    for key, inter in final.items():
        if key[1] == SITE_B and inter and not any('r' in arena.flags.get(k, set()) for k in inter):
            r_viol.append(sorted(inter)[:3])
    both = a_k & b_k
    for k in both:
        if 'r' not in arena.flags.get(k, set()):
            r_viol.append([k])
    R.numbers.update(frames_with_both_sites=mirror_frames, slots_drawn_through_both_sites=len(both), mirrored_objects_without_r_flag=len(r_viol))
    R.check('mirror', mirror_frames > 0 and not mirror_fail and not r_viol,
            '%d frames show both sites; ViewProj failures %d %s; mirrored objects / both-site slots without the `r` flag: %d'
            % (mirror_frames, len(mirror_fail), mirror_fail[:2], len(r_viol)))
    exp_pad = ((expect or {}).get(str(arena_id)) or {}).get('texture_padded_differ', 0)
    R.check('texture_bytes', C['tex_differ'] == 0 and C['tex_equal'] > 0 and C['tex_padded_differ'] <= exp_pad,
            'slot-0 image == the file diffuse DDS, per mip: equal %d, equal but for mips of at most 8 texels %d, bigger image with equal overlapping rows %d, '
            'bigger image whose rows differ %d (at most %d, the measured finding), DIFFER %d; no file texture %d, no capture content %d'
            % (C['tex_equal'], C['tex_tail'], C['tex_padded'], C['tex_padded_differ'], exp_pad, C['tex_differ'], C['tex_no_file_texture'], C['tex_no_capture_content']))

    # the scene record
    if cap.scenes and records:
        want = 'pac\\bg\\bg%02d.pac' % arena_id
        gaps = {'slot_missing': set(), 'twin_missing': set(), 'world_differs': {}}
        arena_bad = guest_eq = 0
        class_world = set()
        for fnum, rec in records.items():
            arena_bad += 1 if rec['pacs'] and rec['pacs'] != {want} else 0
            guest_eq += 1 if rec['guest'] == rec['guest_frame'] else 0
            for key, tr in frame_objs[fnum].items():
                inter = final.get(key, set())
                if not inter or key in obj_reused:
                    continue
                tab = rec['twins'] if key[1] == SITE_B else rec['slots']
                if len(inter) == 1:
                    k = next(iter(inter))
                    if k not in tab:
                        gaps['twin_missing' if key[1] == SITE_B else 'slot_missing'].add(k)
                    elif max(abs(tr[j] - tab[k][j]) for j in range(3)) > 1e-3:
                        gaps['world_differs'][k] = (tr, tab[k])
                else:
                    if not (inter & set(tab)):
                        gaps['twin_missing' if key[1] == SITE_B else 'slot_missing'].add(min(inter))
                    if max(abs(x) for x in tr) > 1e-3:
                        class_world.add(min(inter))
        gaps_now = {'slot_missing': sorted(gaps['slot_missing']), 'twin_missing': sorted(gaps['twin_missing']), 'world_differs': sorted(gaps['world_differs'])}
        R.numbers.update(record_frames=len(records), record_guest_stamp_equals_capture_stamp=guest_eq, record_arena_id_differs=arena_bad,
                         class_objects_with_a_placed_world=len(class_world))
        R.lists['record_gaps'] = gaps_now
        R.lists['placed_slot_worlds'] = {str(k): {'stream_translation': [round(x, 1) for x in v[0]], 'record_pos': list(v[1])} for k, v in sorted(gaps['world_differs'].items())}
        R.lists['class_placed_world_first_slots'] = sorted(class_world)
        ok = arena_bad == 0
        why = '%d record frames (record guest frame == capture frame: %d); record arena differs %d; gaps: slots missing %s, twins missing %s, world differs %s' % (
            len(records), guest_eq, arena_bad, gaps_now['slot_missing'], gaps_now['twin_missing'], gaps_now['world_differs'])
        exp = (expect or {}).get(str(arena_id))
        if exp is not None:
            for k in ('slot_missing', 'twin_missing', 'world_differs'):
                if sorted(exp.get(k, [])) != gaps_now[k]:
                    ok = False
                    why += ' | EXPECTED %s %s' % (k, sorted(exp.get(k, [])))
        R.check('record', ok, why)
    else:
        R.check('record', True, 'no scene records in this capture: not measured (a gap, listed)')
        R.lists['record_gaps'] = {'record': 'not measured: this capture has no scene folder'}
    return R, arena_id, time.time() - t0


def parse_frames(spec, n):
    if not spec:
        return list(range(n))
    a = spec.split(':')
    lo = int(a[0]) if a[0] else 0
    hi = int(a[1]) if len(a) > 1 and a[1] else n
    step = int(a[2]) if len(a) > 2 and a[2] else 1
    return list(range(lo, min(hi, n), step))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    ap.add_argument('--json', default='')
    ap.add_argument('--expect', default=os.path.join(HERE, 'expect.json'))
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    expect = {}
    if os.path.isfile(a.expect):
        expect = json.load(open(a.expect, encoding='utf-8'))
    out = {}
    failed = 0
    for spec in a.captures:
        path, arena = spec, None
        m = re.match(r'^(.*):(\d+)$', spec)
        if m and not re.match(r'^[A-Za-z]$', m.group(1)):
            path, arena = m.group(1), int(m.group(2))
        if not os.path.exists(path) and os.path.exists(os.path.join(MAIN, path)):
            path = os.path.join(MAIN, path)  # gate lanes name captures relative to the main checkout (where out\ lives)
        try:
            cap = Capture(path)
            sel = parse_frames(a.frames, len(cap.frames))
            if not sel:
                raise CensusError('%s: --frames %s selects no frame' % (path, a.frames))
            R, aid, secs = census(cap, arena, sel, a.plant or None, not a.q, expect)
            if arena is not None and aid != arena:
                raise CensusError('arena %d expected, %d found' % (arena, aid))
        except (CensusError, af.FileReadError, OSError, zipfile.BadZipFile) as e:
            print('FAIL  %s: %s' % (os.path.basename(path), e))
            failed += 1
            continue
        print('== %s  arena bg%02d  %d frames  %.1f s%s' % (os.path.basename(path), aid, len(sel), secs, '  PLANT ' + a.plant if a.plant else ''))
        for k, v in R.numbers.items():
            if not a.q:
                print('   %-44s %s' % (k, v))
        for k, (ok, why) in R.checks.items():
            print('   %s  %-14s %s' % ('PASS' if ok else 'FAIL', k, why))
            failed += 0 if ok else 1
        if not a.q:
            for k, v in R.lists.items():
                if v:
                    print('   list %s: %s' % (k, json.dumps(v)[:600]))
        out[os.path.basename(path)] = {'arena': aid, 'numbers': R.numbers, 'checks': R.checks, 'lists': R.lists}
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(out, open(a.json, 'w'), indent=1, default=str)
    print('join_census: %s' % ('FAIL (%d)' % failed if failed else 'PASS'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
