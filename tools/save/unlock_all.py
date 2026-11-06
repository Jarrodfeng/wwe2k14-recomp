"""Session 91: make an "everything unlocked" copy of a frozen test save (the game atlas's second mirror).

  py tools/save/unlock_all.py [--src out/bench/userdata_frozen] [--dst out/bench/userdata_unlocked_s91]
                              [--ids-from out/bench/userdata_unlocked] [--force]

Copies the whole user-data folder SRC to DST (DST must not exist unless --force), then edits DST's
`<profile>/545408B2/00000001/SaveData.dat/SaveData.Dat` in place:
  1. the file = a 0x2CA000-byte prefix (kept byte for byte) + one chunk: 'ZLIB', 12 30 00 00, csize LE, usize LE, a
     zlib stream (sub_8290CBC0 reads it; sub_8290C9E8 writes it);
  2. the payload's 28-byte big-endian header: block A size, block B size, 48, 55, 42, 2, then a 32-bit sum of every
     byte from +28 to the end (checked before and recomputed after);
  3. P = 0x83891878 is loaded from payload +28 + size(A); the per-item unlock flags are bytes P+0x80394+id
     (1 = unlocked; the game's setter sub_822BCB68 writes 1, the test sub_822BCE48 reads it for ids 3..195 except 165),
     so payload offset = 0x1C + size(A) + 0x80394 + id;
  3b. the same ids' "seen" bits in the set at P+0x80378 (bit i = word i>>5, bit i&31 from the LSB; read by the
     Unlockables panel sub_82B104F8: clear = NEW; run 5894 without them: the game opened UNLOCKABLES at boot with all
     164 marked NEW);
  4. which ids: those the game marked in the Accelerator-unlocked save's 196-bit set at P+0x80378 (bit i = word i>>5,
     bit i&31 from the LSB; bits 1..195 = items the game itself listed as unlocked), plus 166..169 (arena-tab
     candidates the Accelerator does not cover); never 0, 1, 2, 165 (entitlement-only, the flag is not read).
Evidence: out/harness/atlas/readers/h_unlocks.md and h_review.md (Session 91 readers; listings cited there).
Our test copies only: never point it at Jarrod's real save; the result is game-derived data (never committed / shipped).
"""
import argparse
import os
import shutil
import struct
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PREFIX = 0x2CA000
REL = os.path.join('545408B2', '00000001', 'SaveData.dat', 'SaveData.Dat')
NEVER = {0, 1, 2, 165}
EXTRA = {166, 167, 168, 169}


def find_save(root):
    for prof in os.listdir(root):
        p = os.path.join(root, prof, REL)
        if os.path.isfile(p):
            return p
    raise SystemExit('no SaveData.Dat under %s' % root)


def read_payload(path):
    data = open(path, 'rb').read()
    if data[PREFIX:PREFIX + 4] != b'ZLIB':
        raise SystemExit('%s: no ZLIB tag at 0x%X' % (path, PREFIX))
    word, csize, usize = struct.unpack_from('<III', data, PREFIX + 4)
    payload = zlib.decompress(data[PREFIX + 16:PREFIX + 16 + csize])
    if len(payload) != usize:
        raise SystemExit('%s: inflated %d bytes, header says %d' % (path, len(payload), usize))
    hdr = struct.unpack_from('>7I', payload, 0)
    if hdr[2:6] != (48, 55, 42, 2) or hdr[0] + hdr[1] + 28 + 48 != len(payload):
        raise SystemExit('%s: unexpected payload header %s (len %d)' % (path, hdr, len(payload)))
    if sum(payload[28:]) & 0xFFFFFFFF != hdr[6]:
        raise SystemExit('%s: payload sum does not match its header' % path)
    return data, word, payload, hdr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=os.path.join(REPO, 'out', 'bench', 'userdata_frozen'))
    ap.add_argument('--dst', default=os.path.join(REPO, 'out', 'bench', 'userdata_unlocked_s91'))
    ap.add_argument('--ids-from', default=os.path.join(REPO, 'out', 'bench', 'userdata_unlocked'))
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    if 'appdata' in os.path.abspath(a.dst).lower() or not os.path.abspath(a.dst).startswith(os.path.join(REPO, 'out')):
        raise SystemExit('refused: DST must be a test copy under out\\')
    # the ids: the Accelerator save's set bits 1..195
    _, _, ref, rhdr = read_payload(find_save(a.ids_from))
    base = 0x1C + rhdr[0]
    words = struct.unpack_from('>7I', ref, base + 0x80378)
    ids = {i for i in range(1, 196) if words[i >> 5] >> (i & 31) & 1}
    ids = (ids | EXTRA) - NEVER
    if os.path.exists(a.dst):
        if not a.force:
            raise SystemExit('%s exists (use --force)' % a.dst)
        shutil.rmtree(a.dst)
    shutil.copytree(a.src, a.dst)
    path = find_save(a.dst)
    data, word, payload, hdr = read_payload(path)
    p = bytearray(payload)
    flags = 0x1C + hdr[0] + 0x80394
    before = sum(1 for i in range(196) if p[flags + i])
    for i in ids:
        p[flags + i] = 1
    # the "seen" marks (bits 1..195 of the set at P+0x80378): without them every unlock shows NEW and the game opens
    # UNLOCKABLES by itself at boot (run 5894). Bit 0 (the Accelerator master switch) stays as it was.
    seen = 0x1C + hdr[0] + 0x80378
    w = list(struct.unpack_from('>7I', p, seen))
    for i in ids:
        w[i >> 5] |= 1 << (i & 31)
    struct.pack_into('>7I', p, seen, *w)
    struct.pack_into('>I', p, 24, sum(p[28:]) & 0xFFFFFFFF)
    stream = zlib.compress(bytes(p), 9)
    out = data[:PREFIX] + b'ZLIB' + struct.pack('<III', word, len(stream), len(p)) + stream
    with open(path, 'wb') as f:
        f.write(out)
    read_payload(path)  # re-check what we wrote
    print('%s: %d unlock flags set (%d before), ids %d..%d; save %d -> %d bytes' % (
        path, len(ids), before, min(ids), max(ids), len(data), len(out)))


if __name__ == '__main__':
    sys.exit(main())
