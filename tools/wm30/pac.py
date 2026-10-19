"""Session 34: read a pac from the player's own game files (read-only), the way the game does.

    py tools\\wm30\\pac.py PAC [--dump DIR] [--depth N]

- XCTD-compressed pacs (first word 0x0FF512ED) are unpacked with out\\tools\\xctd_unpack.exe (built by
  tools\\wm30\\build.ps1 from src\\native_lzx.h) into a cache under out\\wm30\\cache (gitignored: game data).
- An `EPAC` pac: 16 KB header, then `PACH` archives (docs/systems/file_loading.md: 'PACH', LE32 count, count x LE32
  id / offset / size, data at 8 + 12 x count + offset), nested.
- An entry is either another PACH, a `'BPE '` block (src/native_bpe.h: expanded size LE32 at +12, Gage BPE stream from
  +16), a `'ZLIB'` block (LE32 expanded size at +4? read as: zlib stream found after the tag) or plain data.

--dump DIR writes every leaf as DIR/<path>.bin (keep DIR under out\\: game data, never in the repo).
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(REPO, 'game_data', 'extracted')
UNPACK = os.path.join(REPO, 'out', 'tools', 'xctd_unpack.exe')
CACHE = os.path.join(REPO, 'out', 'wm30', 'cache')


def bpe_decode(block):
    """Gage BPE, the way sub_82C41918 / src/native_bpe.h DecodeReference do it."""
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


def decode(data):
    """A leaf entry's bytes, decoded by its tag."""
    tag = data[:4]
    if tag == b'BPE ':
        return bpe_decode(data)
    if tag == b'ZLIB':
        for s in range(4, 32):
            if data[s] == 0x78:
                try:
                    return zlib.decompressobj().decompress(data[s:])
                except zlib.error:
                    pass
    return data


def load(path):
    """The pac's bytes, XCTD unpacked (cached under out\\wm30\\cache)."""
    with open(path, 'rb') as f:
        head = f.read(4)
    if head != b'\x0f\xf5\x12\xed':
        return open(path, 'rb').read()
    rel = os.path.relpath(path, GAME).replace(os.sep, '_').replace(':', '_')
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, rel + '.bin')
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(path):
        if not os.path.exists(UNPACK):
            raise SystemExit('missing %s: run pwsh -NoProfile -File tools\\wm30\\build.ps1' % UNPACK)
        subprocess.run([UNPACK, path, out], check=True, stdout=subprocess.DEVNULL)
    return open(out, 'rb').read()


def pach(data, base=0):
    """[(id, bytes)] of one PACH archive at data[base:]."""
    count = struct.unpack_from('<I', data, base + 4)[0]
    first = base + 8 + 12 * count
    out = []
    for i in range(count):
        eid, off, size = struct.unpack_from('<III', data, base + 8 + 12 * i)
        out.append((eid, data[first + off:first + off + size]))
    return out


def walk(data, prefix='', depth=8):
    """Every leaf (path, decoded bytes) under a PACH, nested PACHs followed (also when they are BPE / ZLIB packed)."""
    for eid, raw in pach(data):
        name = '%s/%d' % (prefix, eid) if prefix else str(eid)
        dec = decode(raw)
        if dec[:4] == b'PACH' and depth > 0:
            yield from walk(dec, name, depth - 1)
        else:
            yield name, dec


def entries(pac_bytes, depth=8):
    """Every leaf of an EPAC pac: the PACH archives that follow the 16 KB header, one after another (each padded to
    2 KB sectors)."""
    pos = 0x4000 if pac_bytes[:4] == b'EPAC' else 0
    k = 0
    while pos + 8 <= len(pac_bytes):
        if pac_bytes[pos:pos + 4] != b'PACH':
            pos += 0x800
            continue
        count = struct.unpack_from('<I', pac_bytes, pos + 4)[0]
        first = pos + 8 + 12 * count
        end = first
        for i in range(count):
            _, off, size = struct.unpack_from('<III', pac_bytes, pos + 8 + 12 * i)
            end = max(end, first + off + size)
        for name, dec in walk(pac_bytes[pos:end], 'p%d' % k, depth):
            yield name, dec
        k += 1
        pos = (end + 0x7FF) & ~0x7FF


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pac')
    ap.add_argument('--dump')
    ap.add_argument('--depth', type=int, default=8)
    a = ap.parse_args()
    path = a.pac if os.path.exists(a.pac) else os.path.join(GAME, 'pac', a.pac)
    data = load(path)
    for name, dec in entries(data, a.depth):
        print('%-20s %8d  %r' % (name, len(dec), dec[:16]))
        if a.dump:
            out = os.path.join(a.dump, name.replace('/', os.sep) + '.bin')
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, 'wb').write(dec)
    return 0


if __name__ == '__main__':
    sys.exit(main())
