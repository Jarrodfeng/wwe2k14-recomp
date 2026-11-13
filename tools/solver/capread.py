r"""Reads a capture frame (docs\renderer\CAPTURE_FORMAT.md, NAMES.md) from a case zip or a capture folder, in Python, with numpy.
Only the tables the constant solver needs. Layout = src\renderer_adapter\adapter_frame.h + names.h (struct sizes asserted below, so a
format change fails loudly instead of reading garbage). Game bytes are read in place and never written anywhere by this module.
"""
import struct
import zipfile
from pathlib import Path

import numpy as np

SEC = dict(ops=1, targets=2, clears=3, draws=4, copies=5, presents=6, fixeds=7, dynamics=8, samplers=9, texture_slots=10, layouts=11,
           elements=12, constant_banks=13, bool_loops=14, contents=15, copy_extras=16, bank_defs=17, bank_patches=18,
           origins=64, roles=65, provenance=66, strings=67, where=68)

DRAW_DT = np.dtype([('vs_hash', '<u8'), ('ps_hash', '<u8'), ('vs_patched', '<u8'), ('fixed', '<u4'), ('dynamic', '<u4'),
                    ('tex_first', '<u4'), ('tex_count', '<u4'), ('vs_bank', '<u4'), ('ps_bank', '<u4'), ('bool_loop', '<u4'),
                    ('layout', '<u4'), ('vertex_offset', '<u8'), ('index_offset', '<u8'), ('vertex_bytes', '<u4'),
                    ('index_bytes', '<u4'), ('vertex_count', '<u4'), ('index_count', '<u4'), ('index_32bit', 'u1'),
                    ('source_prim', 'u1'), ('flags', 'u1'), ('pad', 'u1', (5,))])
ORIGIN_DT = np.dtype([('klass', 'u1'), ('op', 'u1'), ('flags', '<u2'), ('call_site', '<u4'), ('pass_site', '<u4'), ('object', '<u4'),
                      ('part', '<u4'), ('technique', '<u4'), ('asset', '<u4'), ('widget', '<u4')])
PROV_DT = np.dtype([('image', '<u4'), ('source', 'u1'), ('flags', 'u1'), ('pad', '<u2'), ('file', '<u4'), ('file_name', '<u4'),
                    ('source_image', '<u4'), ('writer', '<u4'), ('writer_fn', '<u4'), ('pad2', '<u4'), ('last_write', '<u8')])
SLOT_DT = np.dtype([('image', '<u4'), ('sampler', '<u4'), ('slot', 'u1'), ('view_format', 'u1'), ('swizzle', 'u1', (4,)),
                    ('sign', 'u1', (4,)), ('dimension', 'u1'), ('min_level', 'u1'), ('max_level', 'u1'), ('t16', 'u1'),
                    ('padx', 'u1', (2,)), ('lod_bias', '<f4'), ('t21', '<i4'), ('w3exp', '<i4')])
BANKDEF_DT = np.dtype([('base', '<u4'), ('full', '<u4'), ('patch_first', '<u4'), ('patch_count', '<u4')])
BANKPATCH_DT = np.dtype([('block', '<u4'), ('dwords', '<u4', (16,))])
for dt, n in ((DRAW_DT, 96), (ORIGIN_DT, 32), (PROV_DT, 40), (SLOT_DT, 36), (BANKDEF_DT, 16), (BANKPATCH_DT, 68)):
    assert dt.itemsize == n, (dt, dt.itemsize, n)
NO_BASE = 0xFFFFFFFF


class FormatError(Exception):
    pass


class Source:
    """A case zip (capture\\ inside it) or a capture folder: frame file names and bytes."""

    def __init__(self, path):
        self.path = Path(path)
        if self.path.suffix.lower() == '.zip':
            self.zf = zipfile.ZipFile(self.path)
            names = self.zf.namelist()
            self.frames = sorted(n for n in names if '/frames/frame_' in n or n.startswith('frames/frame_'))
            self.manifest = None
            if 'manifest.json' in names:
                import json
                self.manifest = json.loads(self.zf.read('manifest.json'))
        else:
            self.zf = None
            self.frames = sorted(str(p) for p in (self.path / 'frames').glob('frame_*.bin'))
            self.manifest = None
        if not self.frames:
            raise FormatError(f'{self.path}: no frame files found (a checker that reads nothing must fail)')

    def read(self, name):
        if self.zf is not None:
            return self.zf.read(name)
        return Path(name).read_bytes()


def _table(buf, sec, dt, what):
    if sec is None:
        return np.zeros(0, dt)
    eid, esize, count, off = sec
    if esize != dt.itemsize:
        raise FormatError(f'{what}: element size {esize} in the file, {dt.itemsize} in capread.py')
    return np.frombuffer(buf, dt, count, off)


class Frame:
    def __init__(self, buf):
        magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
        if magic != 0x52464352:
            raise FormatError('not a frame file')
        self.capture_frame, self.guest_frame, self.version = cf, gf, ver
        secs = {}
        for i in range(nsec):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i)
            secs[sid] = (sid, es, cnt, off)
        self.buf = buf
        self.draws = _table(buf, secs.get(SEC['draws']), DRAW_DT, 'draws')
        self.slots = _table(buf, secs.get(SEC['texture_slots']), SLOT_DT, 'texture slots')
        self.has_origins = SEC['origins'] in secs
        self.origins = _table(buf, secs.get(SEC['origins']), ORIGIN_DT, 'origins') if self.has_origins else None
        self.provenance = _table(buf, secs.get(SEC['provenance']), PROV_DT, 'provenance')
        s = secs.get(SEC['strings'])
        self.strings = bytes(buf[s[3]:s[3] + s[2]]) if s else b''
        # banks: full banks, then defs applied
        full = _table(buf, secs.get(SEC['constant_banks']), np.dtype([('d', '<u4', (1024,))]), 'constant banks')
        defs = _table(buf, secs.get(SEC['bank_defs']), BANKDEF_DT, 'bank defs')
        patches = _table(buf, secs.get(SEC['bank_patches']), BANKPATCH_DT, 'bank patches')
        if len(defs):
            out = np.zeros((len(defs), 1024), '<u4')
            for i, d in enumerate(defs):
                if d['base'] == NO_BASE:
                    out[i] = full[d['full']]['d'] if d['full'] < len(full) else 0
                else:
                    out[i] = out[d['base']]
                for p in range(d['patch_count']):
                    pt = patches[d['patch_first'] + p]
                    b = int(pt['block'])
                    out[i, 16 * b:16 * b + 16] = pt['dwords']
            self.banks = out.view('<f4')
        else:
            self.banks = full['d'].view('<f4') if len(full) else np.zeros((0, 1024), '<f4')

    def string(self, off):
        if off == 0:
            return ''
        i = off - 1
        j = self.strings.find(b'\0', i)
        return self.strings[i:j if j >= 0 else len(self.strings)].decode('utf-8', 'replace')

    def reg(self, bank, register, count=1):
        """vec4 registers [register, register+count) of a bank as a (count, 4) float array."""
        return self.banks[bank, 4 * register:4 * (register + count)].reshape(count, 4)
