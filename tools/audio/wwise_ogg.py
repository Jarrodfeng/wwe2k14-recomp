"""Wwise Vorbis (RIFX, Xbox 360, big-endian) -> standard Ogg Vorbis, so ffmpeg can decode a sound for LOUDNESS MEASUREMENT.
A re-implementation of the published ww2ogg algorithm for this one flavour (fmt 0x42: 0x2A-byte 'vorb' block, 2-byte packet
headers, external codebooks, modified audio packets). Our own code; reads a sound out of the player's own game files and writes
an Ogg to a scratch path you give. NOTHING it writes may be committed or shipped (CLAUDE.md rule 1).
The codebook library (packed_codebooks_aoTuV_603.bin or packed_codebooks.bin, from the ww2ogg project) is a data file you pass in.
  py tools/audio/wwise_ogg.py PCK SOUND_ID OUT.ogg LIBRARY.bin
Library: convert(rifx_bytes, library_bytes) -> (ogg bytes, info) (raises ValueError if the sound is not this flavour)."""
import os, struct, sys


# ---------- bits (Vorbis order: least significant bit first) ----------
class BitIn:
    def __init__(s, b):
        s.b, s.pos = b, 0

    def read(s, n):
        v = 0
        for i in range(n):
            byte = s.b[s.pos >> 3] if (s.pos >> 3) < len(s.b) else 0
            v |= ((byte >> (s.pos & 7)) & 1) << i
            s.pos += 1
        return v

    def left(s):
        return len(s.b) * 8 - s.pos


class BitOut:
    def __init__(s):
        s.buf, s.cur, s.n = bytearray(), 0, 0

    def write(s, v, n):
        for i in range(n):
            s.cur |= ((v >> i) & 1) << s.n
            s.n += 1
            if s.n == 8:
                s.buf.append(s.cur)
                s.cur, s.n = 0, 0

    def bytes(s):
        return bytes(s.buf) + (bytes([s.cur]) if s.n else b'')


def ilog(x):
    n = 0
    while x:
        n += 1
        x >>= 1
    return n


def quantvals(entries, dim):
    v = int(entries ** (1.0 / dim))
    while (v + 1) ** dim <= entries:
        v += 1
    while v ** dim > entries:
        v -= 1
    return v


class Library:
    def __init__(s, data):
        off = struct.unpack('<I', data[-4:])[0]
        n = (len(data) - off) // 4
        s.offs = struct.unpack('<%dI' % n, data[off:off + 4 * n])
        s.data = data

    def book(s, i):
        a = s.offs[i]
        b = s.offs[i + 1] if i + 1 < len(s.offs) else len(s.data)
        return s.data[a:b]

    def rebuild(s, i, out):
        r = BitIn(s.book(i))
        dim, entries = r.read(4), r.read(14)
        out.write(0x564342, 24)
        out.write(dim, 16)
        out.write(entries, 24)
        ordered = r.read(1)
        out.write(ordered, 1)
        if ordered:
            out.write(r.read(5), 5)
            cur = 0
            while cur < entries:
                nb = ilog(entries - cur)
                num = r.read(nb)
                out.write(num, nb)
                cur += num
        else:
            cll, sparse = r.read(3), r.read(1)
            if cll == 0 or cll > 5:
                raise ValueError('bad codebook length-length')
            out.write(sparse, 1)
            for _ in range(entries):
                present = 1
                if sparse:
                    present = r.read(1)
                    out.write(present, 1)
                if present:
                    out.write(r.read(cll), 5)
        lt = r.read(1)
        out.write(lt, 4)
        if lt == 1:
            out.write(r.read(32), 32)
            out.write(r.read(32), 32)
            vl = r.read(4)
            out.write(vl, 4)
            out.write(r.read(1), 1)
            for _ in range(quantvals(entries, dim)):
                out.write(r.read(vl + 1), vl + 1)
        elif lt != 0:
            raise ValueError('bad lookup type')


# ---------- ogg ----------
def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
        t.append(r)
    return t


_CRC = _crc_table()


def _crc(b):
    c = 0
    for x in b:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC[((c >> 24) ^ x) & 0xFF]
    return c


class OggOut:
    def __init__(s, serial=0x57575757):
        s.out, s.seq, s.serial = bytearray(), 0, serial

    def page(s, packet, granule, flags):
        segs = []
        n = len(packet)
        while n >= 255:
            segs.append(255)
            n -= 255
        segs.append(n)
        if len(segs) > 255:
            raise ValueError('packet too large for one page')
        hdr = (b'OggS' + bytes([0, flags]) + struct.pack('<qIII', granule, s.serial, s.seq, 0) +
               bytes([len(segs)]) + bytes(segs))
        pg = bytearray(hdr + packet)
        pg[22:26] = struct.pack('<I', _crc(bytes(pg)))
        s.out += pg
        s.seq += 1


# ---------- the converter ----------
OPT = dict(reserved=1, time=1, fw=8, rw=8, mw=8)  # fixed by the leftover-bit probe: this game's setup keeps the standard 8-bit fields

def convert(d, library_bytes):
    if d[:4] != b'RIFX' or d[8:12] != b'WAVE':
        raise ValueError('not RIFX WAVE')
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        tag, sz = d[pos:pos + 4], struct.unpack('>I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + sz]
        if tag == b'fmt ':
            fmt = body
        elif tag == b'data':
            data = body
        pos += 8 + sz + (sz & 1)
    if fmt is None or data is None:
        raise ValueError('no fmt/data')
    tag, ch, sr = struct.unpack('>HHI', fmt[:8])
    if tag != 0xFFFF or len(fmt) != 0x42:
        raise ValueError('not the 0x42 Wwise Vorbis flavour (fmt %d)' % len(fmt))
    vorb = fmt[0x18:]
    sample_count, mod_signal = struct.unpack('>II', vorb[:8])
    setup_off, audio_off = struct.unpack('>II', vorb[0x10:0x18])
    bs0, bs1 = vorb[0x28], vorb[0x29]
    mod_packets = mod_signal not in (0x4A, 0x4B, 0x69, 0x70)
    lib = Library(library_bytes)

    def packet_at(off):
        sz = struct.unpack('>H', data[off:off + 2])[0]
        return data[off + 2:off + 2 + sz], off + 2 + sz

    idh = BitOut()
    idh.write(1, 8)
    for c in b'vorbis':
        idh.write(c, 8)
    idh.write(0, 32)
    idh.write(ch, 8)
    idh.write(sr, 32)
    idh.write(0, 32)
    idh.write(0, 32)
    idh.write(0, 32)
    idh.write(bs0, 4)
    idh.write(bs1, 4)
    idh.write(1, 1)
    com = BitOut()
    com.write(3, 8)
    for c in b'vorbis':
        com.write(c, 8)
    vendor = b'wwise-measure'
    com.write(len(vendor), 32)
    for c in vendor:
        com.write(c, 8)
    com.write(0, 32)
    com.write(1, 1)
    sp, _ = packet_at(setup_off)
    r, w = BitIn(sp), BitOut()
    w.write(5, 8)
    for c in b'vorbis':
        w.write(c, 8)
    ncb = r.read(8) + 1
    w.write(ncb - 1, 8)
    for _ in range(ncb):
        lib.rebuild(r.read(10), w)
    w.write(0, 6)
    w.write(0, 16)
    nf = r.read(6) + 1
    w.write(nf - 1, 6)
    for _ in range(nf):
        w.write(1, 16)
        parts = r.read(5)
        w.write(parts, 5)
        cls = [r.read(4) for _ in range(parts)]
        for c in cls:
            w.write(c, 4)
        maxc = max(cls) if cls else 0
        cdim = [0] * (maxc + 1)
        for c in range(maxc + 1):
            cdim[c] = r.read(3) + 1
            w.write(cdim[c] - 1, 3)
            sub = r.read(2)
            w.write(sub, 2)
            if sub:
                w.write(r.read(8), 8)
            for _ in range(1 << sub):
                w.write(r.read(8), 8)
        w.write(r.read(2), 2)
        rb = r.read(4)
        w.write(rb, 4)
        for c in cls:
            for _ in range(cdim[c]):
                w.write(r.read(rb), rb)
    nr = r.read(6) + 1
    w.write(nr - 1, 6)
    for _ in range(nr):
        rt = r.read(2)
        w.write(rt, 16)
        w.write(r.read(24), 24)
        w.write(r.read(24), 24)
        w.write(r.read(24), 24)
        ncl = r.read(6) + 1
        w.write(ncl - 1, 6)
        w.write(r.read(8), 8)
        casc = []
        for _ in range(ncl):
            low = r.read(3)
            w.write(low, 3)
            flag = r.read(1)
            w.write(flag, 1)
            high = 0
            if flag:
                high = r.read(5)
                w.write(high, 5)
            casc.append(high << 3 | low)
        for c in casc:
            for j in range(8):
                if c & (1 << j):
                    w.write(r.read(8), 8)
    nm = r.read(6) + 1
    w.write(nm - 1, 6)
    for _ in range(nm):
        w.write(0, 16)
        sf = r.read(1)
        w.write(sf, 1)
        subs = 1
        if sf:
            subs = r.read(4) + 1
            w.write(subs - 1, 4)
        spf = r.read(1)
        w.write(spf, 1)
        if spf:
            steps = r.read(8) + 1
            w.write(steps - 1, 8)
            nb = ilog(ch - 1)
            for _ in range(steps):
                w.write(r.read(nb), nb)
                w.write(r.read(nb), nb)
        if OPT['reserved']:
            w.write(r.read(2), 2)
        else:
            w.write(0, 2)
        if subs > 1:
            for _ in range(ch):
                w.write(r.read(4), 4)
        for _ in range(subs):
            if OPT['time']:
                r.read(8)
            w.write(0, 8)
            w.write(r.read(OPT['fw']), 8)
            w.write(r.read(OPT['rw']), 8)
    nmo = r.read(6) + 1
    w.write(nmo - 1, 6)
    flags = []
    for _ in range(nmo):
        bf = r.read(1)
        flags.append(bf)
        w.write(bf, 1)
        w.write(0, 16)
        w.write(0, 16)
        w.write(r.read(OPT['mw']), 8)
    if OPT.get('probe'):
        return r.left()
    w.write(1, 1)
    mode_bits = ilog(nmo - 1)
    pkts, off = [], audio_off
    while off + 2 <= len(data):
        p, off2 = packet_at(off)
        if off2 > len(data):
            break
        pkts.append(p)
        off = off2
    if not mod_packets:
        raise ValueError('standard (unmodified) packets not supported by this tool')
    og = OggOut()
    og.page(idh.bytes(), 0, 2)
    og.page(com.bytes(), 0, 0)
    og.page(w.bytes(), 0, 0)
    gran, prev_bs, prev_flag = 0, None, 0
    for i, p in enumerate(pkts):
        if not p:
            continue
        rd = BitIn(p)
        o = BitOut()
        o.write(0, 1)
        mode = rd.read(mode_bits)
        o.write(mode, mode_bits)
        bf = flags[mode]
        if bf:
            nxt = 0
            if i + 1 < len(pkts) and pkts[i + 1]:
                nxt = flags[BitIn(pkts[i + 1]).read(mode_bits)]
            o.write(prev_flag, 1)
            o.write(nxt, 1)
        while rd.left() > 0:
            n = min(8, rd.left())
            o.write(rd.read(n), n)
        bs = (1 << bs1) if bf else (1 << bs0)
        if prev_bs is not None:
            gran += (prev_bs + bs) // 4
        prev_bs, prev_flag = bs, bf
        last = i == len(pkts) - 1
        og.page(o.bytes(), min(gran, sample_count) if last else gran, 4 if last else 0)
    return bytes(og.out), dict(channels=ch, rate=sr, samples=sample_count, packets=len(pkts))


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import akpk
    pck, sid, outp, libp = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    _, sounds = akpk.read(pck)
    e = [s for s in sounds if s[0] == sid][0]
    with open(pck, 'rb') as f:
        f.seek(e[1])
        raw = f.read(e[2])
    ogg, info = convert(raw, open(libp, 'rb').read())
    open(outp, 'wb').write(ogg)
    print(info)
