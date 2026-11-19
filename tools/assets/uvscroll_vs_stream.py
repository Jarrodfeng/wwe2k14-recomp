r"""S103 lane E6: the real bar for the UV-scroll speeds. Offline, 0 game runs.

  py -I tools\assets\uvscroll_vs_stream.py CAPTURE --speeds LIST.txt [--frames A:B:STEP] [--plant swap_u_v|drop_layer]

CAPTURE = a capture with frames\ (out\d3caster\z7147\capture: arena 4, a Lesnar / Bryan match). LIST.txt = `uvscroll_check.exe --arena 4 --list LIST.txt`: the distinct
(UVScroll0 | UVScroll1) pairs the loader read from the player's own arena file (Material::scroll). For every stream draw of a yBG_UVScroll* / yProp_UVScroll* variant
at the main pass site, VS c203 (UVScroll0) and c204 (UVScroll1) are read from the draw's constant bank (the registers of src\renderer\shaders\fx\*UVScroll*.fx.json) and
each distinct stream pair must equal one of the loader's pairs (1e-4). PASS = at least one non-zero stream pair, and every stream pair is in the loader's set. A loader pair
the capture does not draw is reported (not a failure: the capture is one match view).
--plant swap_u_v swaps U and V in the loader's pairs, drop_layer zeroes layer 1: each must FAIL."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
import ledger as L  # noqa: E402
from ledger_io import Registry  # noqa: E402

TOL = 1e-4


def fl(b):
    import struct
    return struct.unpack('<f', struct.pack('<I', b))[0]


def read_list(path):
    out = []
    for ln in open(path, encoding='utf-8'):
        ln = ln.strip()
        if not ln:
            continue
        a, b = ln.split('|')
        out.append(tuple(float(x) for x in a.split()) + tuple(float(x) for x in b.split()))
    return out


def near(a, b):
    return all(abs(x - y) <= TOL for x, y in zip(a, b))


def stream_pairs(cap, frames):
    reg = Registry(os.path.join(L.ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    pairs = {}
    n = 0
    for fi in frames:
        f = cap.frame(fi)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] != L.SITE_A:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not vn or 'UVScro' not in vn:
                continue
            vb = f.bank(d['vs_bank'])
            key = tuple(round(fl(vb[4 * r + k]), 6) for r in (203, 204) for k in range(4))
            pairs.setdefault(key, [vn, 0])[1] += 1
            n += 1
    return pairs, n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--speeds', required=True)
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default='')
    x = ap.parse_args(argv)
    try:
        cap = L.Capture(x.capture)
        frames = L.parse_frames(x.frames or '0:%d:25' % cap.n, cap.n)
        pairs, n = stream_pairs(cap, frames)
        loader = read_list(x.speeds)
    except (L.LedgerError, OSError, KeyError, ValueError) as e:
        print('FAIL  %s' % e)
        return 1
    if x.plant == 'swap_u_v':
        loader = [tuple(p[k ^ 1] for k in range(8)) for p in loader]
    elif x.plant == 'drop_layer':
        loader = [p[:2] + (0.0, 0.0) + p[4:] for p in loader]
    print('stream UVScroll draws %d, distinct (c203, c204) pairs %d, loader pairs %d' % (n, len(pairs), len(loader)))
    if not pairs:
        print('FAIL  no UVScroll draw in the capture frames')
        return 1
    bad = 0
    nonzero = 0
    for key, (vn, cnt) in sorted(pairs.items()):
        ok = any(near(key, p) for p in loader)
        nonzero += any(v != 0 for v in key)
        print('  %-18s draws %5d  c203 %s  c204 %s  %s' % (vn, cnt, key[:4], key[4:], 'equals a loader pair' if ok else 'NOT IN THE LOADER SET'))
        bad += not ok
    undrawn = [p for p in loader if not any(near(k, p) for k in pairs)]
    print('loader pairs not drawn in these frames: %d of %d' % (len(undrawn), len(loader)))
    # a plant passes (exit 0) only when the check caught it
    if bad:
        print(('caught' if x.plant else 'FAIL') + '  STREAM SPEEDS: %d stream pair(s) not read by the loader' % bad)
        return 0 if x.plant else 1
    if not nonzero:
        print('FAIL  every stream pair is 0: nothing proves the non-zero read')
        return 1
    if x.plant:
        print('FAIL  plant %s was NOT caught' % x.plant)
        return 1
    print('PASS  every stream pair equals a loader pair (%d non-zero)' % nonzero)
    return 0


if __name__ == '__main__':
    sys.exit(main())
