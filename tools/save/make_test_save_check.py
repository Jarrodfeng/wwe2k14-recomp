"""S105 T4 unit for tools/save/make_test_save.py: a synthetic SaveData.Dat built here (never a real save), the tool's pieces run on it, an independent
verifier, and three planted twins that must each be caught (sum not recomputed, superstar index c - 99, set stride 128).

  py -I tools/save/make_test_save_check.py      exit 0 = all pass
"""
import os
import random
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_test_save as M  # noqa: E402
import unlock_all as U  # noqa: E402

SIZE_A, SIZE_B = 0x1000, 0x20000
C, N, SLOT = 139, 2, 3
PAL = [(i * 15 % 256, 255 - i * 9, 7 * i + 1) for i in range(16)]
NAME = 'S105 TEST'
WORD = 0x3012


def synth():
    rnd = random.Random(105)
    body = bytearray(rnd.randrange(256) for _ in range(SIZE_A + SIZE_B + 48))
    p = bytearray(28) + body
    struct.pack_into('>7I', p, 0, SIZE_A, SIZE_B, 48, 55, 42, 2, 0)
    for c in (C - 1, C, C + 1):  # every stock record's sets start at -1, as in the frozen save
        for n in range(3):
            struct.pack_into('>i', p, M.set_offset(SIZE_A, c, n), -1)
    struct.pack_into('>I', p, 24, sum(p[28:]) & 0xFFFFFFFF)
    stream = zlib.compress(bytes(p), 9)
    raw = bytes(U.PREFIX) + b'ZLIB' + struct.pack('<III', WORD, len(stream), len(p)) + stream
    return raw, p


def expected_off(c, n):  # the formula from the note, written out again (not the tool's function)
    return 0x1C + SIZE_A + 4 + 1272 * (c - 100) + 860 + 132 * n


def verify(path, src_payload, c=C, n=N):
    """Independent check; returns a list of problems (empty = the file is what the game's rule and the note say)."""
    try:
        data, word, p, hdr = U.read_payload(path)  # sum, magic, sizes (SystemExit on a mismatch)
    except SystemExit as e:
        return ['read_payload: %s' % e]
    bad = []
    if sum(p[28:]) & 0xFFFFFFFF != hdr[6]:
        bad.append('sum')
    if len(p) != len(src_payload) or hdr[:6] != struct.unpack_from('>6I', src_payload, 0):
        bad.append('sizes / magic')
    o = expected_off(c, n)
    want = struct.pack('>i', SLOT) + bytes(v for t in PAL for v in t) + bytes(12) + NAME.encode().ljust(68, b'\0')
    if p[o:o + 132] != want:
        bad.append('set bytes at 0x%X' % o)
    back = [tuple(p[o + 4 + 3 * s:o + 7 + 3 * s]) for s in range(16)]  # the palette read back
    if back != PAL:
        bad.append('palette re-read')
    diff = [i for i in range(28, len(p)) if p[i] != src_payload[i] and not o <= i < o + 132]
    if diff:
        bad.append('%d bytes changed outside the set' % len(diff))
    return bad


def build(tmp, tag, **kw):
    """The tool's steps on the synthetic save; kw plants a fault. Returns (path, source payload)."""
    raw, src_p = synth()
    path = os.path.join(tmp, tag + '.Dat')
    p = bytearray(src_p)
    M.write_set(p, SIZE_A, SIZE_B, C, N, SLOT, PAL, NAME, force=True, off=kw.get('off'))
    M.finish(p, recompute=kw.get('recompute', True))
    with open(path, 'wb') as f:
        f.write(M.pack_save(raw, WORD, p))
    return path, src_p


# ---- S105 IMP: --mask (the saved DLC pack mask, block B + 0x81168). Own synthetic save: block B must reach 0x81168 + 4.
BIG_B = 0x90000
MASK = 0x7C


def synth_big():
    rnd = random.Random(7)
    body = bytearray(rnd.randrange(256) for _ in range(SIZE_A + BIG_B + 48))
    p = bytearray(28) + body
    struct.pack_into('>7I', p, 0, SIZE_A, BIG_B, 48, 55, 42, 2, 0)
    struct.pack_into('>I', p, 0x1C + SIZE_A + 0x81168, 0)  # the frozen save's mask is 0
    struct.pack_into('>I', p, 24, sum(p[28:]) & 0xFFFFFFFF)
    stream = zlib.compress(bytes(p), 9)
    return bytes(U.PREFIX) + b'ZLIB' + struct.pack('<III', WORD, len(stream), len(p)) + stream, p


def verify_mask(path, src_p, sizea=SIZE_A):
    """Independent: the big-endian word at 0x1C + sizeA + 0x81168 (written out again here) is MASK, sum ok, nothing else changed."""
    try:
        _, _, p, hdr = U.read_payload(path)
    except SystemExit as e:
        return ['read_payload: %s' % e]
    bad = []
    o = 0x1C + sizea + 0x81168
    if p[o:o + 4] != bytes([0, 0, 0, MASK]):
        bad.append('mask word at 0x%X is %s' % (o, p[o:o + 4].hex()))
    if sum(p[28:]) & 0xFFFFFFFF != hdr[6]:
        bad.append('sum')
    diff = [i for i in range(28, len(p)) if p[i] != src_p[i] and not o <= i < o + 4]
    if diff:
        bad.append('%d bytes changed outside the word' % len(diff))
    return bad


def build_mask(tmp, tag, **kw):
    raw, src_p = synth_big()
    path = os.path.join(tmp, tag + '.Dat')
    p = bytearray(src_p)
    if kw.get('little'):  # planted: little-endian word
        struct.pack_into('<I', p, M.mask_offset(SIZE_A), MASK)
    else:
        M.write_mask(p, SIZE_A, BIG_B, MASK, off=kw.get('off'))
    M.finish(p, recompute=kw.get('recompute', True))
    with open(path, 'wb') as f:
        f.write(M.pack_save(raw, WORD, p))
    return path, src_p


def mask_checks(tmp):
    # entry: make_test_save.write_mask / mask_offset (the --mask path of main)
    fails = 0
    if M.mask_offset(0x1224AC) != 0x1A3630:  # the frozen save's inflated-payload offset (IMP_ADV #19)
        print('mask offset on the real block A size: 0x%X, not 0x1A3630' % M.mask_offset(0x1224AC))
        fails += 1
    path, src_p = build_mask(tmp, 'mask_good')
    bad = verify_mask(path, src_p)
    print('mask good: %s' % ('PASS' if not bad else 'FAIL %s' % bad))
    fails += bool(bad)
    twins = {
        'mask sum not recomputed': dict(recompute=False),
        'mask offset without +0x81168': dict(off=0x1C + SIZE_A),
        'mask little-endian': dict(little=True),
        'mask offset 4 early': dict(off=M.mask_offset(SIZE_A) - 4),
    }
    for name, kw in twins.items():
        tp, sp = build_mask(tmp, name.replace(' ', '_'), **kw)
        bad = verify_mask(tp, sp)
        print('twin "%s": %s' % (name, 'caught (%s)' % '; '.join(bad) if bad else 'NOT CAUGHT'))
        fails += not bad
    for label, fn in (('mask past block B', lambda: M.write_mask(bytearray(synth()[1]), SIZE_A, SIZE_B, MASK)),
                      ('mask over 32 bits', lambda: M.write_mask(bytearray(src_p), SIZE_A, BIG_B, 1 << 32))):
        try:
            fn()
            print('refusal %s: NOT GIVEN' % label)
            fails += 1
        except SystemExit:
            print('refusal %s: given' % label)
    return fails


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        path, src_p = build(tmp, 'good')
        bad = verify(path, src_p)
        print('good: %s' % ('PASS' if not bad else 'FAIL %s' % bad))
        fails += bool(bad)
        for c, n in ((100, 0), (139, 0), (139, 2), (499, 2)):  # the tool's offset equals the note's formula
            if M.set_offset(SIZE_A, c, n) != expected_off(c, n):
                print('offset mismatch %d %d' % (c, n))
                fails += 1
        twins = {
            'sum not recomputed': dict(recompute=False),
            'superstar index c - 99': dict(off=M.set_offset(SIZE_A, C + 1, N)),
            'set stride 128': dict(off=0x1C + SIZE_A + 4 + 1272 * (C - 100) + 860 + 128 * N),
        }
        for name, kw in twins.items():
            tp, sp = build(tmp, name.replace(' ', '_'), **kw)
            bad = verify(tp, sp)
            print('twin "%s": %s' % (name, 'caught (%s)' % '; '.join(bad) if bad else 'NOT CAUGHT'))
            fails += not bad
        nonminus = bytearray(src_p)
        struct.pack_into('>i', nonminus, M.set_offset(SIZE_A, C, 0), 5)
        refusals = (
            ('CHAR 99', lambda: M.write_set(bytearray(src_p), SIZE_A, SIZE_B, 99, 0, 0, PAL, NAME)),
            ('SET 3', lambda: M.write_set(bytearray(src_p), SIZE_A, SIZE_B, C, 3, 0, PAL, NAME)),
            ('non -1 target', lambda: M.write_set(nonminus, SIZE_A, SIZE_B, C, 0, 0, PAL, NAME)),
        )
        outs = M.allowed_outs()
        out0 = sorted(outs)[0]
        x = os.path.join(out0, 'x')
        dsts = (('DST = frozen mirror', os.path.join(out0, 'bench', 'userdata_frozen'), x),
                ('DST = source', os.path.join(out0, 'bench', 'a'), os.path.join(out0, 'bench', 'a')),
                ('DST outside out', os.path.join(os.path.dirname(out0), 'elsewhere'), x),
                ('DST in game_data', os.path.join(os.path.dirname(out0), 'game_data', 'z'), x))
        refusals += tuple((l, (lambda d=d, s=s: M.check_dst(d, s, outs))) for l, d, s in dsts)
        for label, fn in refusals:
            try:
                fn()
                print('refusal %s: NOT GIVEN' % label)
                fails += 1
            except SystemExit:
                print('refusal %s: given' % label)
        fails += mask_checks(tmp)  # S105 IMP: --mask
    print('make_test_save_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
