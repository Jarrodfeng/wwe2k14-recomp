r"""Lane P0 (S101): a small lazy reader for capture frames, for the particles census. Reads a case zip or a capture folder; game bytes are only read, never written.
Layout = src\renderer_adapter\adapter_frame.h + names.h (same dtypes as tools\solver\capread.py, which this reuses). Banks are materialised on demand (a frame holds up to
~5,000 of them; the census needs a few hundred)."""
import json
import os
import re
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import capread  # noqa: E402

def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        import subprocess
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()
SEC = capread.SEC
OP_DRAW = 2  # adapter_frame.h OpKind::Draw
LAY_DT = np.dtype([('first', '<u4'), ('count', '<u4'), ('stride', '<u4'), ('gstride', '<u4')])
ELE_DT = np.dtype([('fmt', '<u2'), ('off', '<u2'), ('src', 'u1'), ('comp', 'u1'), ('flags', 'u1'), ('mini', 'u1'), ('goff', '<i4')])
RAW = {SEC['fixeds']: 72, SEC['dynamics']: 192, SEC['samplers']: 20, SEC['bool_loops']: 160}
CLASS_NAMES = ['unnamed', 'model_part', 'crowd', 'sprite', 'text', 'movie', 'particle', 'shadow', 'post', 'clear', 'other']
K_PARTICLE, K_MODEL_PART, K_SPRITE, K_POST, K_OTHER = 6, 1, 3, 8, 10
K_SHADOW = 7
# S101 lane PN: a post quad (op 68) recorded inside a post-effect scope carries the EFFECT's call site in call_site and the generic queue site
# (where the recorder was called) in part. These sites are the generic queue sites; old captures have part 0 and pass through.
GENERIC_QUEUE_SITES = (0x82C1F48C, 0x82C1F514, 0x82C1F674)


def queue_site(o):
    """The recorder's own call site of an origin record (or an array of them): `part` for an op-68 record whose part is a generic queue site, else
    call_site. Works on a structured scalar (returns int) or array (returns int64 array)."""
    cs = np.asarray(o['call_site']).astype(np.int64)
    pt = np.asarray(o['part']).astype(np.int64)
    named = (np.asarray(o['op']) == 68) & np.isin(pt, GENERIC_QUEUE_SITES)
    out = np.where(named, pt, cs)
    return int(out) if out.ndim == 0 else out


def queue_part(o):
    """`part` with the generic queue site of an op-68 record cleared (it is not a model part)."""
    pt = np.asarray(o['part']).astype(np.int64)
    named = (np.asarray(o['op']) == 68) & np.isin(pt, GENERIC_QUEUE_SITES)
    out = np.where(named, 0, pt)
    return int(out) if out.ndim == 0 else out
DRAW_INLINE = 1


class PError(Exception):
    """An input that cannot be read. Never a pass."""


def fx_hashes():
    """vs / ps hash (upper hex, 16 digits) -> 'Effect/variant', from the committed effect descriptions."""
    out = {}
    d = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
    for f in sorted(os.listdir(d)):
        if not f.endswith('.fx.json'):
            continue
        j = json.load(open(os.path.join(d, f), encoding='utf-8'))
        for stage in ('vertex', 'fragment'):
            for v in j.get(stage, []):
                for h in v.get('hashes', []):
                    out[h.upper()] = '%s/%s' % (j['effect'], v['name'])
    return out


FX = fx_hashes()


def effect_of(h):
    return FX.get('%016X' % int(h), '?')


class Capture:
    def __init__(self, path):
        self.path = path
        self.zf = None
        if str(path).lower().endswith('.zip'):
            self.zf = zipfile.ZipFile(path)
            names = self.zf.namelist()
            self.frames = sorted(n for n in names if re.search(r'(^|/)frames/frame_\d+\.bin$', n))
            self.geo_name = next((n for n in names if n.endswith('geometry.bin')), None)
        else:
            fd = os.path.join(path, 'frames')
            self.frames = sorted(os.path.join(fd, f) for f in os.listdir(fd) if f.startswith('frame_')) if os.path.isdir(fd) else []
            self.geo_name = os.path.join(path, 'geometry.bin')
        if not self.frames:
            raise PError('%s: no frame files (a census that reads nothing must fail)' % path)
        self._geo = None

    def read(self, name):
        return self.zf.read(name) if self.zf is not None else open(name, 'rb').read()

    @property
    def geo(self):
        if self._geo is None:
            if not self.geo_name:
                raise PError('%s: no geometry.bin' % self.path)
            self._geo = np.frombuffer(self.read(self.geo_name), np.uint8)
        return self._geo

    def images(self):
        """images.bin: image id -> (width, height, layers)."""
        if getattr(self, '_images', None) is None:
            name = next((n for n in self.zf.namelist() if n.endswith('images.bin')), None) if self.zf is not None else os.path.join(self.path, 'images.bin')
            if not name:
                raise PError('%s: no images.bin' % self.path)
            raw = self.read(name)
            magic, ver, count, _ = struct.unpack_from('<IIII', raw, 0)
            dt = np.dtype([('id', '<u4'), ('w', '<u2'), ('h', '<u2'), ('d', '<u2'), ('mips', 'u1'), ('layers', 'u1'), ('rest', 'u1', (12,))])
            if dt.itemsize != 24 or len(raw) < 16 + 24 * count:
                raise PError('%s: images.bin does not hold %d records of 24 bytes' % (self.path, count))
            self._images = {int(r['id']): (int(r['w']), int(r['h']), int(r['layers'])) for r in np.frombuffer(raw, dt, count, 16)}
        return self._images

    def frame(self, i):
        return FrameData(self.read(self.frames[i]), self, i)


class FrameData:
    def __init__(self, buf, cap, idx):
        self.buf, self.cap, self.idx = buf, cap, idx
        magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
        if magic != 0x52464352:
            raise PError('not a frame file: %s' % cap.frames[idx])
        self.version, self.capture_frame, self.guest_frame = ver, cf, gf
        secs = {}
        for k in range(nsec):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
            secs[sid] = (es, cnt, off)
        self.secs = secs

        def tab(sid, dt):
            if sid not in secs:
                return np.zeros(0, dt)
            es, cnt, off = secs[sid]
            if es != dt.itemsize:
                raise PError('section %d: element size %d in the file, %d here' % (sid, es, dt.itemsize))
            return np.frombuffer(buf, dt, cnt, off)
        self.ops = tab(SEC['ops'], np.dtype([('kind', '<u4'), ('index', '<u4')]))
        self.draws = tab(SEC['draws'], capread.DRAW_DT)
        self.slots = tab(SEC['texture_slots'], capread.SLOT_DT)
        self.layouts, self.elements = tab(SEC['layouts'], LAY_DT), tab(SEC['elements'], ELE_DT)
        self.has_origins = SEC['origins'] in secs
        self.origins = tab(SEC['origins'], capread.ORIGIN_DT) if self.has_origins else None
        self.raw = {}
        for sid, sz in RAW.items():
            if sid in secs:
                es, cnt, off = secs[sid]
                if es != sz:
                    raise PError('section %d: element size %d in the file, %d here' % (sid, es, sz))
                self.raw[sid] = [bytes(buf[off + sz * j:off + sz * (j + 1)]) for j in range(cnt)]
            else:
                self.raw[sid] = []
        self.full = tab(SEC['constant_banks'], np.dtype([('d', '<u4', (1024,))]))
        iref = [('image', '<u4'), ('mip', 'u1'), ('layer', 'u1'), ('pad', '<u2')]
        self.targets = tab(SEC['targets'], np.dtype([('color', iref, (4,)), ('depth', iref), ('count', '<u4')]))
        self.bdefs = tab(SEC['bank_defs'], capread.BANKDEF_DT)
        self.bpatches = tab(SEC['bank_patches'], capread.BANKPATCH_DT)
        self._banks = {}

    def draw_ops(self):
        """Draw indices in op-list order, with each one's position in the op list."""
        k = self.ops['kind'] == OP_DRAW
        return self.ops['index'][k], np.nonzero(k)[0]

    def bank(self, i):
        """One constant bank as 1024 u4 (bytes; never floats)."""
        b = self._banks.get(i)
        if b is not None:
            return b
        if not len(self.bdefs):
            b = np.array(self.full[i]['d'])
            self._banks[i] = b
            return b
        chain, j = [], i  # base chains run thousands deep: walk down to a cached or full bank, then apply upward
        while j not in self._banks and self.bdefs[j]['base'] != capread.NO_BASE:
            chain.append(j)
            j = int(self.bdefs[j]['base'])
        if j in self._banks:
            cur = self._banks[j]
        else:
            d = self.bdefs[j]
            cur = np.array(self.full[d['full']]['d']) if d['full'] < len(self.full) else np.zeros(1024, '<u4')
            cur = self._patch(cur, d)
            self._banks[j] = cur
        for j in reversed(chain):
            cur = self._patch(cur.copy(), self.bdefs[j])
            self._banks[j] = cur
        return cur

    def _patch(self, b, d):
        for p in range(d['patch_count']):
            pt = self.bpatches[d['patch_first'] + p]
            blk = int(pt['block'])
            b[16 * blk:16 * blk + 16] = pt['dwords']
        return b

    def reg(self, bank, r, n=1):
        return self.bank(bank)[4 * r:4 * (r + n)]

    def layout_bytes(self, li):
        lay = self.layouts[li]
        return lay.tobytes() + self.elements[lay['first']:lay['first'] + lay['count']].tobytes()

    def slot_bytes(self, d):
        """The draw's texture slots as bytes with the sampler index replaced by the sampler's own bytes (two equal samplers at different indices are equal)."""
        parts = []
        for s in self.slots[d['tex_first']:d['tex_first'] + d['tex_count']]:
            r = s.copy()
            sid = int(r['sampler'])
            r['sampler'] = 0
            if sid >= len(self.raw[SEC['samplers']]):
                raise PError('frame %d: texture slot names sampler %d but the frame holds %d samplers' % (self.idx, sid, len(self.raw[SEC['samplers']])))
            smp = self.raw[SEC['samplers']][sid]
            parts.append(r.tobytes() + smp)
        return b''.join(parts)

    def target_of(self, op_pos):
        """(width, height, layers) of colour target 0 (the depth target if none) bound by the last SetTargets op before the op at op_pos."""
        sets = np.nonzero(self.ops['kind'][:op_pos] == 0)[0]
        if not len(sets):
            raise PError('frame %d: a draw with no SetTargets op before it' % self.idx)
        t = self.targets[int(self.ops['index'][sets[-1]])]
        img = int(t['color'][0]['image']) if t['count'] else int(t['depth']['image'])
        r = self.cap.images().get(img)
        if r is None:
            raise PError('frame %d: target image %d is not in images.bin' % (self.idx, img))
        return r

    def vertices(self, d):
        o = int(d['vertex_offset'])
        return np.array(self.cap.geo[o:o + int(d['vertex_bytes'])])
