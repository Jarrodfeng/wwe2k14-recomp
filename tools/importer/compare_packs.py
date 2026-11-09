"""Session 98: compares a pack built by tools\\importer\\build_mod_pack.py (the run-073 proven Python builder) with one built by
the C++ core (src\\importer, tools\\importer\\test_core.cpp) for the same character, file by file and row by row.

    py tools\\importer\\compare_packs.py PY_PACK CPP_PACK [--char 397] [--source 385]

Both packs are built from the player's files under out\\ (never commit them). Prints what is identical and what differs;
expected differences (C++ writes the character's own name texts and no Twitter text, a real BPE packed-size word, an
entrance pac and a tron) are marked as such.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_mod_pack as b  # noqa: E402
import pac  # noqa: E402  (tools\wm30, put on the path by build_mod_pack)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('py')
    ap.add_argument('cpp')
    ap.add_argument('--char', type=int, default=397)
    ap.add_argument('--source', type=int, default=385)
    a = ap.parse_args()
    cp, cc = b.read_catalog(os.path.join(a.py, 'info', 'catalog.dlc')), b.read_catalog(os.path.join(a.cpp, 'info', 'catalog.dlc'))
    gate = list(b.GATE_TABLE)
    gate[0] = 0x00060001
    _, _, ip = b.parse_catalog_like_game(cp['data'], gate)
    _, _, ic = b.parse_catalog_like_game(cc['data'], gate)
    fp, fc = {f[0]: f for f in ip['files']}, {f[0]: f for f in ic['files']}
    print('header: py pack %d rev %d, cpp pack %d rev %d' % (cp['pack'], cp['revision'], cc['pack'], cc['revision']))
    print('entries: py %s\n         cpp %s' % (ip['entries'], ic['entries']))
    print('files only in cpp (expected: entrance pac, tron): %s' % sorted(set(fc) - set(fp)))
    print('files only in py: %s' % sorted(set(fp) - set(fc)))
    same_ab = all(fp[n][1:] == fc[n][1:] for n in fp if n in fc and not n.startswith('pac\\evt') and 'Entrance' not in n)
    print('arc indexes (a, b) of the common files equal: %s (cpp shifts the running index after the entrance pac)' % same_ab)
    for n in sorted(set(fp) & set(fc)):
        dp, dc = open(os.path.join(a.py, n), 'rb').read(), open(os.path.join(a.cpp, n), 'rb').read()
        print('%-34s %s' % (n, 'IDENTICAL' if dp == dc else 'differs (%d vs %d bytes)' % (len(dp), len(dc))))
    # chEtc rows
    ep = b.Pac.read(os.path.join(a.py, 'pac', 'ch', 'chEtc_dlc06.pac'))
    ec = b.Pac.read(os.path.join(a.cpp, 'pac', 'ch', 'chEtc_dlc06.pac'))
    for tag in (b'DAT ', b'DSBD', b'PRO '):
        lp, lc = dict(b.pach_read(ep.find(b'CHAR', tag)[1])), dict(b.pach_read(ec.find(b'CHAR', tag)[1]))
        other = [k for k in lp if k != a.char and lp[k] != lc.get(k)]
        rp, rc = pac.decode(lp[a.char]), pac.decode(lc[a.char])
        diff = [o for o in range(min(len(rp), len(rc))) if rp[o] != rc[o]]
        print('CHAR/%s: other rows identical: %s; row %d decoded %s%s' % (
            tag.decode(), not other, a.char, 'IDENTICAL' if rp == rc else 'differs at %s' % diff,
            ' (raw leaf identical)' if lp[a.char] == lc[a.char] else ''))
        if tag == b'DAT ' and diff:
            for o in diff:
                print('    +%d (record +%d): py %d cpp %d' % (o, o - 4, rp[o], rc[o]))
    # DLC_HD / SD renders
    for res in ('HD', 'SD'):
        pp, pc = b.Pac.read(os.path.join(a.py, 'pac', 'DLC_%s_dlc06.pac' % res)), b.Pac.read(os.path.join(a.cpp, 'pac', 'DLC_%s_dlc06.pac' % res))
        for tag in (b'SSFA', b'SSFB', b'SSFC'):
            x, y = pp.find(tag, b'%04d' % a.char), pc.find(tag, b'%04d' % a.char)
            print('DLC_%s %s/%04d: %s' % (res, tag.decode(), a.char, 'IDENTICAL' if x[1] == y[1] else 'differs'))
        tp, tc = dict(pac.pach(pp.find(b'IGST', b'TEX ')[1])), dict(pac.pach(pc.find(b'IGST', b'TEX ')[1]))
        print('DLC_%s IGST/TEX: same leaf ids %s, same leaf bytes %s' % (res, sorted(tp) == sorted(tc), all(tp[k] == tc[k] for k in tp)))
    # the model pac
    mp, mc = b.Pac.read(os.path.join(a.py, 'pac', 'ch', 'ch%d_dlc06.pac' % a.char)), b.Pac.read(os.path.join(a.cpp, 'pac', 'ch', 'ch%d_dlc06.pac' % a.char))
    print('ch%d: py %s, cpp %s' % (a.char, [(e[0], e[2], e[3]) for e in mp.dirs[0][2]], [(e[0], e[2], e[3]) for e in mc.dirs[0][2]]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
