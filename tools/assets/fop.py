r"""S98 research (read-only, 0 game runs): decode the `0FOP` / `EV` table of a wrestler's entrance (pac\evt\Entrance0N.pac).

    py -I tools\assets\fop.py CHAR [--variant G] [--rows] [--lengths]

CHAR = bank / character id (Orton 161, Bank 109 in this dump's numbering, ...).  --variant G = 0..4 reads the shortened
table `1G<CHAR>` (G+1 cues: 1, 6, 11, 16, 21 rows per act); default = the base entry `<CHAR>` (21 rows, = variant 4).
--rows prints every enabled row; --lengths adds the body-motion length of each cue (yanm, motion frames at 60 Hz).
Findings and what is unconfirmed: out\runtime\s98\fop.md.   Nothing here is written; the tables are the player's own.

FILE LAYOUT (big endian; offsets in the decoded `BPE ` block of the pac directory entry; [C] = confirmed by the data
and the game code listed in fop.md, [U] = unconfirmed):
  +0x00 '0FOP' (POF0 reversed: a pointer-offset-table file), +4 u32 size of the body (the POF0 relocation table follows)
  +0x08 'EV\0\0', +0x0C u32 size again, +0x10 u32 0x00010000 (version [U]), +0x14 u32 0x18 [U]
  +0x20 u16 id (= bank id for the base entry; 10000/11000/.. + bank id for the variants), +0x22 char[] name ('Randy_Orton')
  +0x40 u32 N records (low 24 bits: a 31-char name overwrites the high byte; 18 for Orton: acts 25000..25017; Hogan 20; Rock 16), +0x44 N x 32-byte records:
        +0 u16 rows in the block (1/6/11/16/21), +4 u32 offset of the block, +8 u32 index (= act - 25000 + 1; record 0
        = the table itself), +12 u32 X (see below), +16 u32 kind bytes, +20 u32 kind bytes, +24 s32 -1, +28 0
  block k (k >= 1) = rows of 212 bytes, row r at  offset + 8 + 212 * r   (212 = the game's cut row, sub_8233C520 reads
        row+4 act, +8 sub, +12 track, +16/+20/+24 x y z, +28/+32 two floats, +208 u16 prop id (9999 = none)) [C]
  row: +0 u16 r (row index = sub), +2 u8 ENABLED (1 = this (act, sub) has a motion / is a step; the game's "next step"
        scan sub_82326730 takes the next row with this byte set) [C], +3 u8 [U], +4 act, +8 sub, +12 track (0),
        +16 x, +20 y (file space: -Y up), +24 z, +28 heading rad [U], +32 float [U],
        then up to N points of 24 bytes (x, y, z, heading, ?, id/time int) from +16 + 24 * j [U], +208 u16 9999.
  X (record +12): bank*100+2 for the performer acts (16102 = bank 161 + '02'), 5009 / 2038 for 25007 / 25009 (sub_8233C758
        compares the actor's +44 with 2038 = 0x7F6: special case) [C that 2038 is compared; meaning U]; 1 / 2 for the rope
        acts 25012+ (= number of point segments, matches the point counts) [U].
  kind bytes: +16 = 01000000 character acts (25000-25006, 25008), 00000100 (25007, 25009), 00010000 camera (25010),
        +20 = 00010000 light rig (25011), 00000001 rope acts (25012+) [U meaning].
"""
import glob
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
GAME = os.path.join(MAIN, 'game_data', 'extracted')
sys.path.insert(0, os.path.join(MAIN, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(MAIN, 'tools', 'assets'))
import pac  # noqa: E402

ROW = 212
ACT0 = 25000


def directory(d):
    """{entry id: (start byte, size)} of an Entrance0N.pac (16-byte entries from 0x80C: 8-char id, LE32 start in 2 KB
    units from 0x4000, LE32 size in 256-byte units)."""
    o, res = 0x80C, {}
    while o + 16 <= 0x4000:
        nm = d[o:o + 8].strip(b' \0')
        if not nm.isdigit():
            break
        st, _sz = struct.unpack_from('<II', d, o + 8)
        res[int(nm)] = 0x4000 + st * 0x800
        o += 16
    return res


def load_entry(entry_id):
    """(pac file name, decoded EV table bytes) for a directory entry id (161 = base, 10161.. = variants)."""
    for f in sorted(glob.glob(os.path.join(GAME, 'pac', 'evt', 'Entrance0*.pac'))):
        d = pac.load(f)
        dr = directory(d)
        if entry_id in dr:
            return os.path.basename(f), pac.bpe_decode(d[dr[entry_id]:])
    raise SystemExit('no entrance pac has entry %d' % entry_id)


class Row:
    def __init__(self, b, o):
        self.o = o
        self.idx, self.en, self.b3 = struct.unpack_from('>HBB', b, o)
        self.act, self.sub, self.track = struct.unpack_from('>3I', b, o + 4)
        self.pos = struct.unpack_from('>3f', b, o + 16)
        self.f28, self.f32 = struct.unpack_from('>2f', b, o + 28)
        self.prop = struct.unpack_from('>H', b, o + 208)[0]
        # point list: 24-byte points from +16 while something is non-zero (rope / group acts)
        pts = []
        for j in range(8):
            p = o + 16 + 24 * j
            if p + 24 > o + 208:
                break
            x, y, z, h, e, n = struct.unpack_from('>6f', b, p)
            if any(struct.unpack_from('>6I', b, p)):
                pts.append((x, y, z, h, n))
        self.points = pts


class Ev:
    def __init__(self, b):
        assert b[:4] == b'0FOP' and b[8:10] == b'EV', 'not a 0FOP/EV table'
        self.b = b
        self.size, = struct.unpack_from('>I', b, 4)
        self.id, = struct.unpack_from('>H', b, 0x20)
        # S98 (props general): a 31-character name runs through +0x40 = the HIGH byte of the record count (banks 676, 677, 731,
        # 757, 777: the count read 0x69000010 ..); the name ends at +0x41 and the count is the low 24 bits.
        self.name = b[0x22:0x41].split(b'\0')[0].decode('latin1').strip()
        self.n = struct.unpack_from('>I', b, 0x40)[0] & 0xFFFFFF
        self.recs = []
        for i in range(self.n):
            o = 0x44 + 32 * i
            nrows, = struct.unpack_from('>H', b, o)
            off, idx, x = struct.unpack_from('>3I', b, o + 4)
            kind = b[o + 16:o + 24]
            self.recs.append(dict(i=i, nrows=nrows, off=off, idx=idx, x=x, kind=kind))
        self.acts = {}
        for r in self.recs[1:]:
            self.acts[ACT0 + r['idx'] - 1] = [Row(b, r['off'] + 8 + ROW * k) for k in range(r['nrows'])]

    def steps(self):
        """sorted list of enabled sub numbers of the body act (the game's step list, = cues)."""
        return [r.sub for r in self.acts.get(ACT0, []) if r.en]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    char = int(args[0])
    var = None
    if '--variant' in sys.argv:
        var = int(sys.argv[sys.argv.index('--variant') + 1])
    eid = char if var is None else 10000 + 1000 * var + char
    fname, b = load_entry(eid)
    ev = Ev(b)
    print('%s entry %d: %s  name=%r  id=%d  size=%d  records=%d' % (fname, eid, 'EV table', ev.name, ev.id, ev.size, ev.n))
    print('step list (enabled rows of act 25000) =', ev.steps())
    print()
    print('act   X       kind(+16..+23)    rows  enabled subs                       placement at the enabled subs')
    for r in ev.recs[1:]:
        act = ACT0 + r['idx'] - 1
        rows = ev.acts[act]
        en = [x.sub for x in rows if x.en]
        pl = []
        for x in rows:
            if x.en or x.points or any(x.pos):
                if x.points:
                    pl.append('s%d:%s' % (x.sub, ' '.join('(%.0f,%.0f,%.0f|%g)' % (p[0], -p[1], p[2], p[4]) for p in x.points)))
                elif any(x.pos):
                    pl.append('s%d:(%.0f,%.0f,%.0f h%.2f)' % (x.sub, x.pos[0], -x.pos[1], x.pos[2], x.f28))
        print('%d %-7d %s %3d  %-34s %s' % (act, r['x'], r['kind'].hex(), r['nrows'], en, '  '.join(pl[:6])))
    if '--rows' in sys.argv:
        print()
        for act, rows in ev.acts.items():
            for x in rows:
                if x.en or x.points or any(x.pos):
                    print(act, 'sub', x.sub, 'en', x.en, 'b3', x.b3, 'pos', x.pos, 'f28 %.3f f32 %.3f' % (x.f28, x.f32),
                          'prop', hex(x.prop), 'pts', x.points)
    if '--lengths' in sys.argv:
        import spots
        _f, path = spots.entrance_path(char)
        print()
        t = 0
        print('cue  last Bip key frame (~ body motion length, 60 Hz)  seconds  start (scene frames)  guest frames at 2 mf/frame')
        for cue in sorted(path):
            fr = path[cue][-1][0]
            print('%3d  %6d  %7.2f  %6d  %6.0f' % (cue, fr, fr / 60.0, t, fr / 2.0))
            t += fr
        print('total', t, 'frames = %.1f s' % (t / 60.0))


if __name__ == '__main__':
    main()
