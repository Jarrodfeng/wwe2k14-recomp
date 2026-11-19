"""S103 CMB: offline census of the attires the game builds by default (misc01_Start.pac p0/600 flags A and B both 1), from the player's own files, read-only.

    py -I tools\\assets\\attire_compose_census.py [--all-pachs]

For each (character, slot): the remapped PACH (file +34+s = attire of kind-2 key; attire-th model PACH, LoadWrestlerPac's rule), then
  (i)   entry 12 / 13..16: size, bits, fourcc, DDSD_MIPMAPCOUNT + mip count, channel masks; any input Compose() would refuse is listed (REFUSE)
  (ii)  which entries 13..16 exist (Mask4 = entry 16)
  (iii) claimed texels (a mask channel is non-zero) whose base DXT1 alpha is 0 (BC1 3-colour mode, index 3), under the header mapping
  (iv)  per step k: texels claimed by each channel R, G, B, A under the header mapping (R 0x0F00 G 0x00F0 B 0x000F A 0xF000) and under the captured
        replay's mapping (R 0xF000 G 0x000F B 0x00F0 A 0x0F00)
Game-derived numbers: print only, never committed.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import attire_build as ab  # noqa: E402

pac = ab.pac
HDR = (0x0F00, 0x00F0, 0x000F, 0xF000)
REPLAY = (0xF000, 0x000F, 0x00F0, 0x0F00)


def model_pachs(cid):
    """[{entry id: decoded bytes}] of every PACH holding entry 0 (a model), in file order."""
    data = pac.load(os.path.join(pac.GAME, 'pac', 'ch', 'ch%d.pac' % cid))
    pos = 0x4000 if data[:4] == b'EPAC' else 0
    out = []
    while pos + 8 <= len(data):
        if data[pos:pos + 4] == b'PACH':
            ents = {eid: pac.decode(raw) for eid, raw in pac.pach(data, pos)}
            if 0 in ents:
                out.append(ents)
        pos += 0x800
    return out


def dds_info(blob):
    at = blob.find(b'DDS ', 0, 0x100)
    if at < 0:
        return None
    h = at + 4
    height, width = struct.unpack_from('<II', blob, h + 8)
    flags = struct.unpack_from('<I', blob, h + 4)[0]
    mips = struct.unpack_from('<I', blob, h + 24)[0]
    pf = h + 72
    pflags, fourcc, bits, rm, gm, bm, am = struct.unpack_from('<I4sIIIII', blob, pf + 4)
    return dict(w=width, h=height, fourcc=fourcc, bits=bits, masks=(rm, gm, bm, am), mipflag=bool(flags & 0x20000), mips=mips, data=blob[at + 128:])


def bc1_alpha0(data, w, h):
    bw, bh = w // 4, h // 4
    blocks = np.frombuffer(data, dtype='<u2', count=bw * bh * 4).reshape(bh, bw, 4)
    c0, c1 = blocks[..., 0], blocks[..., 1]
    idx = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 16)
    out = np.zeros((bh, 4, bw, 4), bool)
    three = (c0 <= c1)
    for y in range(4):
        for x in range(4):
            out[:, y, :, x] = three & (((idx >> (2 * (4 * y + x))) & 3) == 3)
    return out.reshape(h, w)


def claims(v, masks):
    """[R, G, B, A] bool 'first non-zero' claim maps for 16-bit values v under the channel masks."""
    done = np.zeros(v.shape, bool)
    out = []
    for m in masks:
        shift = (m & -m).bit_length() - 1
        pick = ~done & (((v & m) >> shift) != 0)
        out.append(pick)
        done |= pick
    return out


def main():
    table = ab.colour_table(True)
    names = {}
    refused = 0
    any_alpha0 = 0
    alpha0_total = [0]
    for cid in sorted(table):
        for s, cols in enumerate(table[cid]):
            if cols is None:
                continue
            rec = None
            data = pac.load(os.path.join(pac.GAME, 'pac', 'misc01_Start.pac'))
            tab = next(d for n, d in pac.entries(data) if n == 'p0/600')
            for k in range(struct.unpack_from('<H', tab, 0)[0]):
                if struct.unpack_from('<H', tab, 2 + ab.RECORD * k)[0] == cid:
                    rec = tab[2 + ab.RECORD * k: 2 + ab.RECORD * (k + 1)]
            kind, att = rec[24 + s], rec[34 + s]
            try:
                pachs = model_pachs(cid)
            except OSError as e:
                print('%d s%d: no pac (%s)' % (cid, s, e))
                continue
            target = att if kind == 0 else s   # kind-2 remap only when the word +24+s is 0 (CMP2)
            if target >= len(pachs):
                print('%d s%d: remap target PACH %d of %d' % (cid, s, target, len(pachs)))
                target = 0
            ents = pachs[target]
            line = '%d slot %d -> PACH %d (+24 %d, +34 %d): entries %s' % (cid, s, target, kind, att, [e for e in (12, 13, 14, 15, 16) if e in ents])
            print(line)
            if 12 not in ents:
                print('   REFUSE: no entry 12')
                refused += 1
                continue
            b = dds_info(ents[12])
            print('   base %dx%d %s bits %d mipflag %s mips %d' % (b['w'], b['h'], b['fourcc'], b['bits'], b['mipflag'], b['mips']))
            if b['fourcc'] != b'DXT1':
                print('   REFUSE: base is not DXT1')
                refused += 1
                continue
            a0 = bc1_alpha0(b['data'], b['w'], b['h'])
            print('   base alpha-0 texels (BC1 3-colour mode, index 3): %d of %d' % (int(a0.sum()), a0.size))
            alpha0_total[0] += int(a0.sum())
            for e in (13, 14, 15, 16):
                if e not in ents:
                    continue
                m = dds_info(ents[e])
                why = []
                if (m['w'], m['h']) != (b['w'], b['h']):
                    why.append('size %dx%d' % (m['w'], m['h']))
                if m['bits'] != 16 or m['fourcc'] != b'\0\0\0\0':
                    why.append('bits %d fourcc %r' % (m['bits'], m['fourcc']))
                print('   mask %d %dx%d bits %d fourcc %r mipflag %s mips %d masks %s' % (e, m['w'], m['h'], m['bits'], m['fourcc'], m['mipflag'], m['mips'], tuple('%04X' % x for x in m['masks'])))
                if why:
                    print('   REFUSE: mask %d: %s' % (e, ', '.join(why)))
                    refused += 1
                    continue
                v = np.frombuffer(m['data'], '<u2', count=m['w'] * m['h']).reshape(m['h'], m['w']).astype(np.uint32)
                ch = claims(v, HDR)
                rp = claims(v, REPLAY)
                claimed = np.logical_or.reduce(ch)
                n0 = int((claimed & a0).sum())
                any_alpha0 += n0
                print('   k%d header R,G,B,A %s | replay %s | claimed on base alpha 0: %d' % (e - 13, [int(c.sum()) for c in ch], [int(c.sum()) for c in rp], n0))
    print("SUMMARY: refusals %d; claimed texels on base alpha 0 (total) %d; base alpha-0 texels (all slots) %d" % (refused, any_alpha0, alpha0_total[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
