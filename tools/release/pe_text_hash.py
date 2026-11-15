"""S102 S0-6: SHA-256 of a PE file's executable sections (.text and every other section with the code flag), to show that a symbols build
(CMake WWE2K14_EXE_SYMBOLS=ON) generates the same code as a plain one.  py -I tools\\release\\pe_text_hash.py A.exe [B.exe ...]
Exit 0 when every file given has the same code hash, 1 when they differ, 2 on a bad input."""
import hashlib, struct, sys


def code_hash(path):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ':
        raise ValueError(f'{path}: not a PE file')
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path}: no PE signature')
    nsec, = struct.unpack_from('<H', d, pe + 6)
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    sec = pe + 24 + opt
    h = hashlib.sha256()
    total = 0
    for i in range(nsec):
        name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', d, sec + 40 * i)
        ch, = struct.unpack_from('<I', d, sec + 40 * i + 36)
        if ch & 0x20:  # IMAGE_SCN_CNT_CODE
            h.update(name + d[rptr:rptr + rsize])
            total += rsize
    if total == 0:
        raise ValueError(f'{path}: no code section found')
    return h.hexdigest(), total


def text_bytes(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    sec = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
    out = b''
    for i in range(n):
        name, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, sec + 40 * i)
        if struct.unpack_from('<I', d, sec + 40 * i + 36)[0] & 0x20:
            out += d[rp:rp + rs]
    return out


def relaxed(a, b):
    """Same code up to rip-relative displacements? Every differing byte must sit in a 4-byte window that is an int32 displacement
    shifted by one of a few constant deltas (a data section moved by the debug directory growing). Returns (ok, n_windows, deltas)."""
    ta, tb = text_bytes(a), text_bytes(b)
    if len(ta) != len(tb):
        return False, 0, set()
    diff = [i for i in range(len(ta)) if ta[i] != tb[i]]
    deltas, wins, i = set(), 0, 0
    while i < len(diff):
        for s in range(diff[i] - 3, diff[i] + 1):
            if s < 0 or s + 4 > len(ta):
                continue
            da = struct.unpack_from('<i', ta, s)[0]
            db = struct.unpack_from('<i', tb, s)[0]
            end = max([x for x in diff if x < s + 4] or [-1])
            if 0 < abs(db - da) <= 0x100000 and diff[i] >= s and end < s + 4:
                deltas.add(db - da)
                wins += 1
                while i < len(diff) and diff[i] < s + 4:
                    i += 1
                break
        else:
            return False, wins, deltas
    return True, wins, deltas


def main(argv):
    if argv[:1] == ['--relaxed'] and len(argv) == 3:
        ok, w, d = relaxed(argv[1], argv[2])
        print(f"{'SAME CODE' if ok else 'DIFFERENT CODE'} up to rip-relative displacements: {w} displacement windows, deltas {sorted(d)[:8]}")
        return 0 if ok else 1
    if not argv:
        print(__doc__)
        return 2
    seen = set()
    for a in argv:
        try:
            hx, n = code_hash(a)
        except Exception as e:
            print(f'ERROR {e}')
            return 2
        print(f'{hx}  {n} bytes of code  {a}')
        seen.add(hx)
    return 0 if len(seen) == 1 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
