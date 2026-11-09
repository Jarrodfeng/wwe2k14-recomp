r"""Session 97: read a gpu_debug_draw_probe CSV (all 256 vertex constants per draw) and print the viewer's `camera` pipe
command for the main camera of one frame (swap).  Usage:
  py tools\assets\probe_camera.py CSV [--swap N] [--eye x,y,z]
The main camera = the draws with the commonest c72 (eye) among the draws whose c56..c59 are not shadow-map passes
(--eye picks one by value).  c56..c59 are the ROWS of the view-projection (clip.i = dot(c5x, (world,1))), world = the files'
own space (-Y up); the viewer turns that into its own space (Y negated) when it gets "viewproj"."""
import csv, sys, json, collections
csv.field_size_limit(1 << 30)

def rows(path, swap=None):
    with open(path, newline='') as f:
        r = csv.DictReader(f)
        for row in r:
            if swap is not None and int(row['swap']) != swap:
                continue
            row['c'] = [float(x) for x in row['consts'].split()]
            yield row

def reg(row, n):
    return row['c'][4 * n:4 * n + 4]

def main():
    a = sys.argv[1:]
    path = a[0]
    swap = int(a[a.index('--swap') + 1]) if '--swap' in a else None
    eye = tuple(float(x) for x in a[a.index('--eye') + 1].split(',')) if '--eye' in a else None
    cnt = collections.Counter()
    first = {}
    for row in rows(path, swap):
        c = reg(row, 72)
        vp = tuple(round(v, 3) for i in range(4) for v in reg(row, 56 + i))
        key = (row['swap'],) + tuple(round(v, 1) for v in c[:3]) + (vp,)
        cnt[key] += 1
        first.setdefault(key, row)
    if eye:
        keys = [k for k, _ in cnt.most_common() if k[1:4] == tuple(round(v, 1) for v in eye)]
    else:
        keys = [k for k, _ in cnt.most_common(1)]
    for k, n in cnt.most_common(6):
        print('# group swap %s eye %s draws %d' % (k[0], k[1:4], n), file=sys.stderr)
    k = keys[0]
    row = first[k]
    vp = sum((reg(row, 56 + i) for i in range(4)), [])
    print('# swap %s eye %s draws %d' % (k[0], k[1:4], cnt[k]), file=sys.stderr)
    print(json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': reg(row, 72)[:3]}))

if __name__ == '__main__':
    main()
