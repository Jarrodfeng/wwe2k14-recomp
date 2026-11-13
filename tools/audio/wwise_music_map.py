"""Which Wwise streams does a music event play?  (Session 101 lane 283: finds the entrance themes to MEASURE their loudness.)
Reads the HIRC chunks of the banks in sound/Game_Sound_M.pck (Wwise bank v65, big-endian) and walks event -> action -> music
objects -> tracks by scanning object bodies for the ids of other objects (a 32-bit id appearing in a body is a reference; random
collisions are negligible at 2^-32 per window). Prints/returns only ids and counts; nothing from the game is stored.
  py tools/audio/wwise_music_map.py PCK [MUSIC_ID ...]       -> per music id the stream sound ids reached
Library: build(pck) -> Graph; Graph.streams(event_name) -> sorted stream ids (those present in the package's sound table)."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import akpk


def fnv1_32_lower(name):
    h = 0x811C9DC5
    for c in name.lower().encode():
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= c
    return h


class Graph:
    def __init__(s):
        s.objs = {}  # id -> (type, body bytes)

    def add_bank(self, raw):
        pos = 0
        while pos + 8 <= len(raw):
            tag, sz = raw[pos:pos + 4], struct.unpack('>I', raw[pos + 4:pos + 8])[0]
            if tag == b'HIRC':
                body = raw[pos + 8:pos + 8 + sz]
                n = struct.unpack('>I', body[:4])[0]
                p = 4
                for _ in range(n):
                    t, osz = body[p], struct.unpack('>I', body[p + 1:p + 5])[0]
                    ob = body[p + 5:p + 5 + osz]
                    if len(ob) >= 4:
                        self.objs[struct.unpack('>I', ob[:4])[0]] = (t, ob[4:])
                    p += 5 + osz
            pos += 8 + sz

    def ids_in(self, body):
        for i in range(len(body) - 3):
            yield (body[i] << 24) | (body[i + 1] << 16) | (body[i + 2] << 8) | body[i + 3]

    def streams(self, event_name, sound_ids, maxdepth=8):
        """Event -> play actions -> (Sound: its stream source | Random/Sequence container: its Sound/container children).
        Only type 2 / 5 objects are followed (buses, states and switches would link everything to everything)."""
        eid = fnv1_32_lower(event_name)
        if eid not in self.objs or self.objs[eid][0] != 4:
            return None
        body = self.objs[eid][1]
        n = struct.unpack('>I', body[:4])[0]
        frontier = []
        for k in range(n):
            a = struct.unpack('>I', body[4 + 4 * k:8 + 4 * k])[0]
            if a in self.objs and self.objs[a][0] == 3 and self.objs[a][1][:2] == b'':
                frontier.append(struct.unpack('>I', self.objs[a][1][2:6])[0])
        seen, found = set(frontier), set()
        for _ in range(maxdepth):
            nxt = []
            for o in frontier:
                if o not in self.objs:
                    continue
                t, b = self.objs[o]
                if t == 2:
                    src = struct.unpack('>I', b[8:12])[0]
                    if src in sound_ids:
                        found.add(src)
                elif t == 5:
                    for c in self.ids_in(b):
                        if c not in seen and c in self.objs and self.objs[c][0] in (2, 5) and c != o:
                            seen.add(c)
                            nxt.append(c)
            frontier = nxt
        return sorted(found)


def build(pck):
    banks, sounds = akpk.read(pck)
    g = Graph()
    with open(pck, 'rb') as f:
        for bid, off, size, _ in banks:
            f.seek(off)
            g.add_bank(f.read(size))
    return g, {s[0]: s for s in sounds}


if __name__ == '__main__':
    g, snd = build(sys.argv[1])
    ids = [int(x) for x in sys.argv[2:]] or [343]
    print(len(g.objs), 'objects,', len(snd), 'sounds')
    for i in ids:
        r = g.streams('Play_MUS_%04d_0_0' % i, snd)
        print(i, None if r is None else [(x, snd[x][2]) for x in r])
