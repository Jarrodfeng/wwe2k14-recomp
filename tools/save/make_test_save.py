"""S105 T4: make a TEST user-data mirror with a Threads recolour set written into SaveData.Dat block B.

  py -I tools/save/make_test_save.py [--src out/bench/userdata_frozen] [--dst out/bench/userdata_threads_s105]
         [--threads CHAR:SET:SLOT] [--palette magenta|black|FILE.json] [--name TEXT] [--force]
  py -I tools/save/make_test_save_check.py     (the unit and the planted twins; no game, no real save)

Default: CHAR 139 (Cena), SET 0, base attire slot 0 (key 139*100 + 0*10 + 2 = 13902, the key entry 0 of the s71_cena_threads capture names).
Copies the whole SRC folder to DST, then edits only DST's SaveData.Dat (layout: tools/save/unlock_all.py docstring, docs\\research\\s105\\T4_test_save.md):
set n of stock superstar c is the 132 bytes at payload  0x1C + sizeA + 4 + 1272 (c - 100) + 860 + 132 n :
  +0 u32 BE base attire slot (-1 = no set), +4 16 RGB triples (step s, channel k at 4 s + k), +52 4 RGB triples (left zero, meaning [U]),
  +64 a 68-byte NUL-padded name. Then the payload byte sum (bytes 28..end, header word 24) is recomputed exactly as unlock_all.py does.
Never touches 00PaintTool.pt or any other file. Refuses a DST that is the frozen mirror, the source, holds the source, is under game_data or appdata,
or is outside out\\. Test mirrors only: never point it at a real save. The result is game-derived data (never committed / shipped).
"""
import argparse
import json
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import unlock_all as U  # noqa: E402  (read_payload / find_save / PREFIX / REPO)

REPO = U.REPO
PREFIX = U.PREFIX
REC_STRIDE = 1272
SET_BASE = 860
SET_STRIDE = 132
SET_LEN = 132


def set_offset(size_a, c, n):
    """Payload offset of set n of stock superstar c (c 100..499)."""
    return 0x1C + size_a + 4 + REC_STRIDE * (c - 100) + SET_BASE + SET_STRIDE * n


def palette_bytes(pal):
    if len(pal) != 16 or any(len(t) != 3 or any(not 0 <= v <= 255 for v in t) for t in pal):
        raise SystemExit('palette must be 16 RGB triples of 0..255')
    return bytes(v for t in pal for v in t)


def make_set(slot, pal, name):
    nb = name.encode('ascii')
    if len(nb) > 67:
        raise SystemExit('name longer than 67 bytes')
    return struct.pack('>i', slot) + palette_bytes(pal) + bytes(12) + nb.ljust(68, b'\0')


def write_set(p, size_a, size_b, c, n, slot, pal, name, force=False, off=None):
    """Writes the set into the bytearray p (payload). off: planted-twin override only."""
    if not 100 <= c <= 499:
        raise SystemExit('CHAR must be 100..499 (stock superstar records; 0..99 and CAWs are a different table)')
    if not 0 <= n <= 2:
        raise SystemExit('SET must be 0..2')
    if not 0 <= slot <= 9:
        raise SystemExit('SLOT must be 0..9')
    o = set_offset(size_a, c, n) if off is None else off
    if o + SET_LEN > 0x1C + size_a + size_b:
        raise SystemExit('set 0x%X..0x%X falls outside block B' % (o, o + SET_LEN))
    cur = struct.unpack_from('>i', p, o)[0]
    if cur != -1 and not force:
        raise SystemExit('target set word is %d, not -1 (the source already has a set there; --force to overwrite)' % cur)
    p[o:o + SET_LEN] = make_set(slot, pal, name)
    return o


MASK_IN_B = 0x81168  # S105 IMP: block B's saved DLC pack mask (guest 0x839129E0 = 0x83891878 + 0x81168; docs/research/s105/IMP_ADV.md #19)


def mask_offset(size_a):
    """Payload offset of the saved DLC pack mask (a big-endian u32 in block B): 0x1C + sizeA + 0x81168 (= 0x1A3630 on the frozen save)."""
    return 0x1C + size_a + MASK_IN_B


def write_mask(p, size_a, size_b, mask, off=None):
    """S105 IMP: sets the saved DLC pack mask (bit N = pack N was offered when the save was written). off: planted-twin override only."""
    if not 0 <= mask <= 0xFFFFFFFF:
        raise SystemExit('--mask must fit 32 bits')
    o = mask_offset(size_a) if off is None else off
    if o + 4 > 0x1C + size_a + size_b:
        raise SystemExit('mask word 0x%X falls outside block B' % o)
    struct.pack_into('>I', p, o, mask)
    return o


def finish(p, recompute=True):
    if recompute:
        struct.pack_into('>I', p, 24, sum(p[28:]) & 0xFFFFFFFF)


def pack_save(data, word, p):
    stream = zlib.compress(bytes(p), 9)
    return data[:PREFIX] + b'ZLIB' + struct.pack('<III', word, len(stream), len(p)) + stream


def check_dst(dst, src, outs):
    dst_real, src_real = os.path.realpath(dst), os.path.realpath(src)
    low = dst_real.lower()
    if 'appdata' in low or not any(dst_real.startswith(o + os.sep) for o in outs):
        raise SystemExit('refused: DST must be a test copy under out\\ (%s)' % ', '.join(sorted(outs)))
    for g in {os.path.realpath(os.path.join(os.path.dirname(o), 'game_data')) for o in outs}:
        if dst_real == g or dst_real.startswith(g + os.sep):
            raise SystemExit('refused: DST is inside game_data')
    frozen = {os.path.realpath(os.path.join(o, 'bench', 'userdata_frozen')) for o in outs}
    if dst_real == src_real or dst_real in frozen or src_real.startswith(dst_real + os.sep) or dst_real.startswith(src_real + os.sep):
        raise SystemExit('refused: DST %s is the source, the frozen mirror, or nested with the source' % dst_real)


def allowed_outs():
    outs = {os.path.realpath(os.path.join(REPO, 'out'))}
    up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
    if os.path.basename(os.path.dirname(REPO)) == 'worktrees' and os.path.basename(os.path.dirname(os.path.dirname(REPO))) == '.claude' \
            and os.path.isdir(os.path.join(up, 'game_data')):
        outs.add(os.path.realpath(os.path.join(up, 'out')))
    return outs


def load_palette(arg):
    if arg == 'magenta':
        return [(230, 40, 200)] * 16
    if arg == 'black':
        return [(0, 0, 0)] * 16
    with open(arg) as f:
        return [tuple(t) for t in json.load(f)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=os.path.join(REPO, 'out', 'bench', 'userdata_frozen'))
    ap.add_argument('--dst', default=os.path.join(REPO, 'out', 'bench', 'userdata_threads_s105'))
    ap.add_argument('--threads', default='139:0:0', help='CHAR:SET:SLOT (default Cena, set 0, base attire slot 0)')
    ap.add_argument('--palette', default='magenta', help='magenta | black | FILE.json (16 RGB triples)')
    ap.add_argument('--name', default='S105 TEST')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--mask', default=None, help='S105 IMP: HEX, the saved DLC pack mask (bit N = pack N offered); e.g. 7C = packs 2-6. '
                    'Set it to the pack set the test boot offers EXACTLY (a superset gives the refusal notice, a subset hides the bug). '
                    'Without --threads none the default Cena threads set is planted too')
    a = ap.parse_args()
    if a.threads.lower() == 'none':
        if a.mask is None:
            raise SystemExit('--threads none needs --mask')
        c = n = slot = None
    else:
        try:
            c, n, slot = (int(x) for x in a.threads.split(':'))
        except ValueError:
            raise SystemExit('--threads wants CHAR:SET:SLOT (or none, with --mask)')
    mask = None
    if a.mask is not None:
        try:
            mask = int(a.mask, 16)
        except ValueError:
            raise SystemExit('--mask wants a hex number (7C)')
    pal = load_palette(a.palette)
    check_dst(a.dst, a.src, allowed_outs())
    if os.path.exists(a.dst):
        if not a.force:
            raise SystemExit('%s exists (use --force)' % a.dst)
        shutil.rmtree(a.dst)
    shutil.copytree(a.src, a.dst)
    path = U.find_save(a.dst)
    data, word, payload, hdr = U.read_payload(path)
    p = bytearray(payload)
    o = write_set(p, hdr[0], hdr[1], c, n, slot, pal, a.name, a.force) if c is not None else None
    mo = write_mask(p, hdr[0], hdr[1], mask) if mask is not None else None
    finish(p)
    out = pack_save(data, word, p)
    with open(path, 'wb') as f:
        f.write(out)
    _, _, chk, h2 = U.read_payload(path)  # re-check what we wrote (sum, sizes, magic)
    if (o is not None and chk[o:o + SET_LEN] != make_set(slot, pal, a.name)) or h2[:2] != hdr[:2] \
            or (mo is not None and struct.unpack_from('>I', chk, mo)[0] != mask):
        raise SystemExit('read-back differs from what was written')
    if mo is not None:
        print('%s: saved DLC pack mask = 0x%08X at payload 0x%X; save %d -> %d bytes' % (path, mask, mo, len(data), len(out)))
    if c is None:
        return
    print('%s: set %d of CHAR %d at payload 0x%X (base slot %d); save %d -> %d bytes' % (path, n, c, o, slot, len(data), len(out)))
    print('key %d; launch: -Data %s --wwe2k14_launch=s71_cena_threads (only for the default 139:0:0; another wrestler needs a capture whose entry holds that key, +22 = 1, +23 = SET)'
          % (c * 100 + slot * 10 + 2, os.path.basename(a.dst.rstrip('\\/'))))


if __name__ == '__main__':
    main()
