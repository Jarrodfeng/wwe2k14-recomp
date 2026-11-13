"""Lane 117 (R4, S101): read the drawn text of a frame file (names.h section 69 run strings, section 64 origins) offline. 0 game runs.

Usage: py -I tools/n2/caw_text_read.py FRAME_FILE [--verts]
Prints each distinct run string with its draw count and first draw; --verts adds the first glyph quad's screen x/y (vertex bytes).
"""
import struct, sys, collections


def load(path):
    b = open(path, 'rb').read()
    magic, ver, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
    secs = {}
    for i in range(sc):
        sid, es, cnt, o = struct.unpack_from('<IIQQ', b, 32 + i * 24)
        secs[sid] = (es, cnt, o)
    return b, gf, secs


def get_name(S, n):
    if n == 0:
        return None
    e = S.index(b'\0', n - 1)
    return S[n - 1:e].decode('utf8', 'replace')


def frame(path):
    b, gf, secs = load(path)
    es, cnt, o = secs[67]
    S = b[o:o + cnt * es]
    es, cnt, o = secs[69]
    runs = [struct.unpack_from('<IIII', b, o + i * es) for i in range(cnt)]
    return b, gf, secs, S, runs


def origins(b, secs, S):
    es, cnt, o = secs[64]
    out = []
    for i in range(cnt):
        r = struct.unpack_from('<BBH7I', b, o + i * es)
        out.append(r)
    return out


if __name__ == '__main__':
    b, gf, secs, S, runs = frame(sys.argv[1])
    print(gf, {k: (v[0], v[1]) for k, v in secs.items()})
    seen = collections.OrderedDict()
    for d, s, u, f in runs:
        seen.setdefault(get_name(S, s), []).append(d)
    for k, v in seen.items():
        print(repr(k), len(v), v[0])


def expand_banks(b, secs):
    """Expand BankDefs (section 17) + BankPatches (18) over the full banks (13) into 4096-byte banks, as CaptureReader::ParseFrame does."""
    fes, fn, fo = secs[13]
    des, dn, do = secs[17]
    pes, pn, po = secs[18]
    out = []
    for i in range(dn):
        base, full, pf, pc = struct.unpack_from('<IIII', b, do + i * des)
        if base == 0xFFFFFFFF:
            bank = bytearray(b[fo + full * fes: fo + (full + 1) * fes])
        else:
            bank = bytearray(out[base])
        for p in range(pf, pf + pc):
            blk = struct.unpack_from('<I', b, po + p * pes)[0]
            bank[blk * 64: blk * 64 + 64] = b[po + p * pes + 4: po + p * pes + 68]
        out.append(bytes(bank))
    return out
