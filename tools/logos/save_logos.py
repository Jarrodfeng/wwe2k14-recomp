"""Read the Create-a-Logo save (00PaintTool.pt\\SaveData.Dat), read-only. Session 25; format in docs\\systems\\save_logos.md.

  py tools\\logos\\save_logos.py list SAVE          slots: used / free, size, date
  py tools\\logos\\save_logos.py png SAVE OUTDIR    each used slot's picture as OUTDIR\\slot_NN.png (RGBA)
  py tools\\logos\\save_logos.py diff SAVE_A SAVE_B which slots differ

SAVE is the SaveData.Dat file or a user data root (the file is found under it).
"""
import struct
import sys
import zlib
from pathlib import Path

SLOTS = 20
SLOT = 0x50468  # bytes per slot
HEADER = 8      # before slot 0
FIELD_USED = 0x00     # u32 BE, 1 = used
FIELD_W = 0x40        # u32 BE
FIELD_H = 0x44        # u32 BE
FIELD_PIXELS = 0x48   # w * h ARGB words, big-endian
FIELD_DATE = 0x5045C  # u16 year, u8 month, day, hour, minute, second, u8 (?)


def find_save(p):
    p = Path(p)
    if p.is_file():
        return p
    hits = sorted(p.rglob('00PaintTool.pt/SaveData.Dat'))
    if not hits:
        sys.exit(f'no 00PaintTool.pt\\SaveData.Dat under {p}')
    return hits[0]


def blob(path):
    d = path.read_bytes()
    out, off = [], 0
    while off + 16 <= len(d) and d[off:off + 4] == b'ZLIB':
        _ver, csize, usize = struct.unpack('<III', d[off + 4:off + 16])
        raw = zlib.decompress(d[off + 16:off + 16 + csize])
        if len(raw) != usize:
            sys.exit(f'{path}: block at {off:#x} is {len(raw)} bytes, header says {usize}')
        out.append(raw)
        off += 16 + csize
    b = b''.join(out)
    if len(b) != HEADER + SLOTS * SLOT + 4:
        sys.exit(f'{path}: {len(b)} bytes after inflate, expected {HEADER + SLOTS * SLOT + 4} (unknown layout)')
    return b


def slot(b, i):
    base = HEADER + i * SLOT
    u32 = lambda o: struct.unpack('>I', b[base + o:base + o + 4])[0]
    used, w, h = u32(FIELD_USED), u32(FIELD_W), u32(FIELD_H)
    y, mo, d, hh, mi, ss = struct.unpack('>HBBBBB', b[base + FIELD_DATE:base + FIELD_DATE + 7])
    return {'index': i, 'used': used == 1, 'w': w, 'h': h, 'base': base,
            'date': f'{y:04d}-{mo:02d}-{d:02d} {hh:02d}:{mi:02d}:{ss:02d} UTC' if y else ''}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'list':
        b = blob(find_save(sys.argv[2]))
        for i in range(SLOTS):
            s = slot(b, i)
            state = f"used  {s['w']}x{s['h']}  saved {s['date']}" if s['used'] else 'free'
            print(f"slot {i + 1:2d} (page {i // 10 + 1}, row {i % 10 // 5 + 1}, col {i % 5 + 1}): {state}")
    elif cmd == 'png':
        from PIL import Image
        b = blob(find_save(sys.argv[2]))
        out = Path(sys.argv[3])
        out.mkdir(parents=True, exist_ok=True)
        for i in range(SLOTS):
            s = slot(b, i)
            if not s['used'] or not (0 < s['w'] <= 256 and 0 < s['h'] <= 256):
                continue
            px = b[s['base'] + FIELD_PIXELS:s['base'] + FIELD_PIXELS + s['w'] * s['h'] * 4]
            Image.frombytes('RGBA', (s['w'], s['h']), px, 'raw', 'ARGB').save(out / f'slot_{i + 1:02d}.png')
            print(out / f'slot_{i + 1:02d}.png')
    elif cmd == 'diff':
        a, c = blob(find_save(sys.argv[2])), blob(find_save(sys.argv[3]))
        for i in range(SLOTS):
            lo, hi = HEADER + i * SLOT, HEADER + (i + 1) * SLOT
            if a[lo:hi] != c[lo:hi]:
                sa, sc = slot(a, i), slot(c, i)
                print(f"slot {i + 1:2d}: {'used' if sa['used'] else 'free'} -> {'used' if sc['used'] else 'free'}"
                      f" {sc['w']}x{sc['h']}")
        print('header', 'same' if a[:HEADER] == c[:HEADER] else 'differs', '| final word',
              a[-4:].hex(), '->', c[-4:].hex())
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
