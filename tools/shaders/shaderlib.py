"""Session 61: the game's shader containers, read from the player's own files (docs\\systems\\shaders.md).

Nothing here or in what it prints is game data beyond names, counts, offsets and hashes; the microcode itself stays in
memory or under out\\ (gitignored).

Container (big-endian words, as Session 61 measured on pac\\_startup.pac key 0 against our shader storage):
  +0  magic 0x102A1100 (pixel) / 0x102A1101 (vertex)
  +4  virtual size (header, constant table, definitions, shader descriptor)
  +8  physical size (literal constants, then the microcode)
  +24 shader descriptor offset (from the container start)
  descriptor +0 microcode offset in the physical part, +4 microcode size in bytes
  microcode = container + virtual size + descriptor[0], descriptor[1] bytes.
"""

import ctypes
import os
import struct

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(REPO, 'game_data', 'extracted')
XXH3_DLL = os.path.join(REPO, 'out', 'tools', 'xxh3.dll')

PS_MAGIC = 0x102A1100
VS_MAGIC = 0x102A1101
MAGICS = {PS_MAGIC: 'PS', VS_MAGIC: 'VS'}

_xxh3 = None


def xxh3(data):
    """XXH3_64bits, as the SDK's shader storage hashes microcode (guest order)."""
    global _xxh3
    if _xxh3 is None:
        if not os.path.exists(XXH3_DLL):
            raise SystemExit('missing %s: run pwsh -NoProfile -File tools\\shaders\\build.ps1' % XXH3_DLL)
        lib = ctypes.CDLL(XXH3_DLL)
        lib.xxh3_64.restype = ctypes.c_uint64
        lib.xxh3_64.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _xxh3 = lib.xxh3_64
    return _xxh3(bytes(data), len(data))


def containers(data):
    """Every valid shader container in a decoded blob: [(offset, 'VS' | 'PS', microcode offset, microcode bytes)]."""
    out = []
    n = len(data)
    for magic, kind in MAGICS.items():
        tag = struct.pack('>I', magic)
        p = data.find(tag)
        while p >= 0:
            if p + 36 <= n:
                vsize, psize = struct.unpack_from('>II', data, p + 4)
                desc = struct.unpack_from('>I', data, p + 24)[0]
                if 28 <= desc < vsize and p + desc + 8 <= n and p + vsize + psize <= n:
                    moff, msize = struct.unpack_from('>II', data, p + desc)
                    if msize and msize % 4 == 0 and moff + msize <= psize:
                        code = p + vsize + moff
                        out.append((p, kind, code, data[code:code + msize]))
            p = data.find(tag, p + 4)
    out.sort()
    return out


def effect_table(data):
    """The effect table of pac\\_startup.pac key 0: [(name, offset, size)] (LE32 count at +0, 16-byte header, then
    32-byte entries: 16-byte name, 'obj\\0', LE32 size, LE32 offset, 4 zero bytes)."""
    count = struct.unpack_from('<I', data, 0)[0]
    out = []
    for i in range(count):
        e = data[16 + 32 * i:48 + 32 * i]
        if e[16:20] != b'obj\0':
            return None
        size, off = struct.unpack_from('<II', e, 20)
        out.append((e[:16].rstrip(b'\0').decode('ascii', 'replace'), off, size))
    return out
