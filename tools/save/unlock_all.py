"""Session 91: make an "everything unlocked" copy of a frozen test save (the game atlas's second mirror).

  py tools/save/unlock_all.py [--src out/bench/userdata_frozen] [--dst out/bench/userdata_unlocked_s91]
                              [--ids-from out/bench/userdata_unlocked] [--force] [--wm30-open]

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
5. (S101, --wm30-open, off by default) every 30 Years of WrestleMania match open: profile word 25+n = 1 for n = 0..45, big-endian u32 at
   payload 0x75EC4 + 4*n (block A, absolute payload offsets, not relative to block B), before the sum is recomputed. n is field 3 of the
   match index row, a permutation of 0..45. The cleared / completed arrays (words 105+n, 185+n) stay 0. Evidence:
   docs/research/s101/N0-6b.md; [U] until one game check (the timeline icons all unlocked; a late match pickable).
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
WM30_OPEN = 0x75EC4  # S101: payload offset of profile word 25 (match slot 0's open flag); slot n is at + 4*n
WM30_SLOTS = 46


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
    ap.add_argument('--wm30-open', action='store_true', help='S101: also open all 46 30 Years of WrestleMania matches (docs/research/s101/N0-6b.md)')
    a = ap.parse_args()
    # S101: also the main checkout's out\ when this runs from a git worktree (<main>/.claude/worktrees/<name>). Only a worktree
    # layout (<main>/.claude/worktrees/<name>) may name the checkout above; anywhere else that path would be C:\out or similar.
    outs = {os.path.realpath(os.path.join(REPO, 'out'))}
    up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
    if os.path.basename(os.path.dirname(REPO)) == 'worktrees' and os.path.basename(os.path.dirname(os.path.dirname(REPO))) == '.claude' \
            and os.path.isdir(os.path.join(up, 'game_data')):
        outs.add(os.path.realpath(os.path.join(up, 'out')))
    dst_real, src_real = os.path.realpath(a.dst), os.path.realpath(a.src)
    if 'appdata' in dst_real.lower() or not any(dst_real.startswith(o + os.sep) for o in outs):
        raise SystemExit('refused: DST must be a test copy under out\\ (%s)' % ', '.join(sorted(outs)))
    # --force deletes DST: never the source, never the frozen mirror the test runs read, never a folder holding the source
    frozen = {os.path.realpath(os.path.join(o, 'bench', 'userdata_frozen')) for o in outs}
    if dst_real == src_real or dst_real in frozen or src_real.startswith(dst_real + os.sep):
        raise SystemExit('refused: DST %s is the source, the frozen mirror, or contains the source' % dst_real)
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
    if a.wm30_open:  # S101: before the sum recompute below
        # the edit must stay in block A (payload 0x1C .. 0x1C + size(A)), and the layout must be the one N0-6b read: word 25 (match
        # slot 0, always open) is 1 in the file we start from; anything else means this is not the save the offsets were read from
        if not (0x1C <= WM30_OPEN and WM30_OPEN + 4 * WM30_SLOTS <= 0x1C + hdr[0]):
            raise SystemExit('wm30 open words 0x%X..0x%X fall outside block A (0x1C..0x%X): layout differs from N0-6b' % (
                WM30_OPEN, WM30_OPEN + 4 * WM30_SLOTS, 0x1C + hdr[0]))
        if struct.unpack_from('>I', p, WM30_OPEN)[0] != 1:
            raise SystemExit('wm30 open word 25 is %d, not 1 (match slot 0 is open in every save N0-6b read): wrong offset or an '
                             'unexpected save; nothing written' % struct.unpack_from('>I', p, WM30_OPEN)[0])
        for n in range(WM30_SLOTS):
            struct.pack_into('>I', p, WM30_OPEN + 4 * n, 1)
    struct.pack_into('>I', p, 24, sum(p[28:]) & 0xFFFFFFFF)
    stream = zlib.compress(bytes(p), 9)
    out = data[:PREFIX] + b'ZLIB' + struct.pack('<III', word, len(stream), len(p)) + stream
    with open(path, 'wb') as f:
        f.write(out)
    read_payload(path)  # re-check what we wrote
    print('%s: %d unlock flags set (%d before), ids %d..%d; save %d -> %d bytes' % (
        path, len(ids), before, min(ids), max(ids), len(data), len(out)))
    if a.wm30_open:
        _, _, chk, _ = read_payload(path)
        opened = sum(1 for n in range(WM30_SLOTS) if struct.unpack_from('>I', chk, WM30_OPEN + 4 * n)[0] == 1)
        print('%d of %d 30 Years match-open words set' % (opened, WM30_SLOTS))
        if opened != WM30_SLOTS:
            raise SystemExit('wm30 open words did not read back')


if __name__ == '__main__':
    sys.exit(main())
