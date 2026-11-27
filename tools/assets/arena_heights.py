r"""Session 98 (research, read-only, 0 game runs): per arena heights from the player's own game files.

    py -I tools\assets\arena_heights.py [ARENA ...]        (default: every pac\bg\bgNN.pac)

Prints, per arena (Y UP here, the viewer's space; the files are -Y up):
  canvas top   pac\bg\bgNN.pac entry 953 (the ring canvas mesh): the highest vertex (file y -min) and the flat top's
               vertex count; the flat top is |x|,|z| <= 310, the rim rolls down to 111 at +-319, the side to 100.
  entrance offset  the arena table in pac\misc01_Start.pac leaf p0/2 (resource type 2, loaded by sub_822C42D8: 12-byte
               header, LE u32 count at +4, 108-byte records from +12; byte +4 = arena id, byte +24 = the flag
               sub_8230F840 reads via sub_824E7668 / sub_822C38B8). sub_8230F840 returns the float -120.0 (file space,
               = +120 up; image 0x82003778) when the flag is set, else 0.0; sub_8233C520 (the entrance "set actor
               motion" cut) adds it to the row's y when it positions the actor. Arena id = bg number is [U] (by count
               and order: ids 0..56 minus the missing bgs).
  stage / ramp rects  entry 50003 (light 200 = stage, 201 = ramp), floor = flat meshes (y extent <= 6) clipped to the rect,
               clustered by height (rounded), biggest bounding-box area first, in Y up; roofs / decks show as clusters too.
"""
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spots  # noqa: E402

pac = spots.pac
GAME = spots.GAME


def arena_table():
    """{arena id: flag byte +24} from misc01_Start.pac p0/2."""
    d = pac.load(os.path.join(GAME, 'pac', 'misc01_Start.pac'))
    b = dict(pac.entries(d, 8))['p0/2']
    n = struct.unpack_from('<I', b, 4)[0]
    return {b[12 + 108 * i + 4]: b[12 + 108 * i + 24] for i in range(n)}


def canvas(entries):
    for k, blob in entries:
        if k == 'p0/953':
            ys = [p[1] for m in spots.meshes(blob) for p in m]
            top = min(ys)
            return -top, sum(1 for y in ys if abs(y - top) < 0.05)
    return None, 0


def floor_clusters(entries, r):
    """Flat meshes (y extent <= 6) clipped to the rectangle, clustered by height: [(y_up, share of the rectangle covered
    by their bounding boxes, vertex count)] by decreasing area (y = the vertex median of a mesh, rounded to 1 unit).
    Overlaps count twice, so shares can exceed 100%; roofs / ceilings / seat decks show as separate clusters."""
    xs = sorted((r['x0'], r['x1']))
    zs = sorted((r['z0'], r['z1']))
    area_rect = max(1.0, (xs[1] - xs[0]) * (zs[1] - zs[0]))
    cl = {}
    for k, blob in entries:
        if not k.startswith('p0/') or not k[3:].isdigit() or int(k[3:]) >= 400:
            continue
        try:
            ms = spots.meshes(blob)
        except Exception:
            continue
        for pts in ms:
            if not pts or max(p[1] for p in pts) - min(p[1] for p in pts) > 6:
                continue
            ins = [p for p in pts if xs[0] <= p[0] <= xs[1] and zs[0] <= p[2] <= zs[1]]
            if len(ins) < 3:
                continue
            ys = sorted(-p[1] for p in ins)
            ar = (max(p[0] for p in ins) - min(p[0] for p in ins)) * (max(p[2] for p in ins) - min(p[2] for p in ins))
            a = cl.setdefault(round(ys[len(ys) // 2]), [0.0, 0])
            a[0] += ar
            a[1] += len(ins)
    return sorted(((y, a[0] / area_rect, a[1]) for y, a in cl.items()), key=lambda t: -t[1])


def fmt(cl):
    if not cl:
        return 'no flat mesh'
    return '; '.join('y %d (%.0f%% of rect, %d v)' % (y, 100 * sh, n) for y, sh, n in cl[:3] if sh >= 0.05) or 'y %d' % cl[0][0]


def main():
    want = [int(a) for a in sys.argv[1:]]
    tab = arena_table()
    files = sorted(glob.glob(os.path.join(GAME, 'pac', 'bg', 'bg[0-9][0-9].pac')))
    for f in files:
        n = int(os.path.basename(f)[2:4])
        if want and n not in want:
            continue
        d = pac.load(f)
        es = list(pac.entries(d, 8))
        top, cnt = canvas(es)
        flag = tab.get(n)
        off = {1: '+120.0 (flag 1)', 0: '0 (flag 0)', None: 'n/a (id not in table)'}.get(flag, 'flag %s' % flag)
        print('bg%02d  canvas top %s (%d flat verts)  entrance offset %s' % (
            n, '%.1f' % top if top is not None else '-', cnt, off))
        rects = spots.arena_rects(d)
        if not rects:
            print('      no 50003 rectangles')
        for r in rects:
            tag = 'stage' if r['light'] == 200 else 'ramp ' if r['light'] == 201 else 'light%d' % r['light']
            print('      %s x %+5.0f..%+5.0f z %5.0f..%5.0f  %s' % (
                tag, min(r['x0'], r['x1']), max(r['x0'], r['x1']), min(r['z0'], r['z1']), max(r['z0'], r['z1']),
                fmt(floor_clusters(es, r))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
