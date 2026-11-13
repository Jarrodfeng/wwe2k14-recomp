"""AKPK (Wwise package) reader for the game's sound pck files (big-endian header, Xbox 360).
Answers: which banks / sounds a package holds and where. Reads only; never writes game data anywhere.
  py tools/audio/akpk.py PCK            -> counts + first entries
Library: akpk.read(path) -> (banks, sounds); each entry (id, offset, size, folder)."""
import struct, sys

def _table(buf):
    n = struct.unpack('>I', buf[:4])[0]
    out = []
    for i in range(n):
        fid, blk, size, start, folder = struct.unpack('>5I', buf[4 + 20 * i:24 + 20 * i])
        out.append((fid, start * (blk or 1), size, folder))
    return out

def read(path):
    with open(path, 'rb') as f:
        h = f.read(0x1C)
        assert h[:4] == b'AKPK'
        hs, ver, lang_sz, banks_sz, sounds_sz = struct.unpack('>5I', h[4:24])
        data = f.read(lang_sz + banks_sz + sounds_sz)
    return _table(data[lang_sz:lang_sz + banks_sz]), _table(data[lang_sz + banks_sz:])

if __name__ == '__main__':
    b, s = read(sys.argv[1])
    print(len(b), 'banks', len(s), 'sounds')
    for e in b[:5]: print('bank', e)
    for e in s[:5]: print('snd', e)
