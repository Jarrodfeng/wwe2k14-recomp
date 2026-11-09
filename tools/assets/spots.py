r"""Session 98 step 1: named wrestler spots per arena, derived from the player's own game files (read-only, 0 game runs).

    py -I tools\assets\spots.py ARENA [--char CHAR] [--json]

ARENA = the bg number (5 = bg05.pac).  Space = the files' own arena space (the viewer's space with Y mirrored): X right,
Z toward the entrance stage (the ring is at the origin, the entrance stage at +Z ~2900-3300, the announce desk at -Z),
units are the game's (the ring canvas is +-318.8, a wrestler ~176 tall), Y UP here (file space is -Y up: file y = -Y).
Facing yaw: degrees, atan2(dx, dz) of the facing vector in the XZ plane (0 = facing +Z toward the stage, 180 = facing -Z
toward the desk, 90 = facing +X).  Evidence and what is unconfirmed: out\runtime\s98\spots.md.

Sources (all game files, nothing is written):
  pac\bg\bgNN.pac entry 50003      text, Shift-JIS: rectangles "start x, start z, end x, end z, light set" = the entrance
                                   stage (light 200) and the ramp / hanamichi (light 201) areas
  pac\bg\bgNN.pac entries 0..399   stage meshes (floor height inside the rectangles); 953 = the ring canvas
  pac\bg\bgEtc.pac p9/6/0          announce desk model (extents), placed at z -775.4 (probe 20911, bg05)
  pac\evt\Entrance0N.pac           per wrestler section, act 25000 = the entrance walk (motions 0000/0005/0010/0015/0020 =
                                   cue 0 / 5 / 10 / 15 / 20), track `Bip` position keys = the walk path in arena space
"""
import glob
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
GAME = os.path.join(MAIN, 'game_data', 'extracted')
sys.path.insert(0, os.path.join(MAIN, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(MAIN, 'tools', 'assets'))
import pac  # noqa: E402

BIP_HASH = 0x10B4F0  # node hash of `Bip` (yanm.node_hash)


def be(b, o):
    return struct.unpack_from('>I', b, o)[0]


def meshes(blob):
    """JBOY entry -> [(vertex_count, [(x, y, z)] in FILE space)]"""
    body = blob[8:]
    nm, tab = be(body, 0x10), be(body, 0x14)
    out = []
    for m in range(nm):
        rec = tab + 0xB8 * m
        nv = be(body, rec)
        s0 = be(body, rec + 0x68) + 4
        out.append([struct.unpack_from('>3f', body, s0 + 28 * i) for i in range(nv)])
    return out


def arena_rects(d):
    for k, b in pac.entries(d, 8):
        if k.endswith('/50003'):
            rects = []
            for ln in b.decode('cp932', 'replace').splitlines():
                ln = ln.split(';')[0].strip()
                if not ln or ln.startswith('/'):
                    continue
                v = ln.split()
                if len(v) >= 5:
                    rects.append(dict(x0=float(v[0]), z0=float(v[1]), x1=float(v[2]), z1=float(v[3]), light=int(v[4])))
            return rects
    return []


def floor_in_rect(entries, r):
    """Median file-y of the vertices inside the rectangle that belong to FLAT meshes (y extent <= 6): the floor slab."""
    xs = sorted((r['x0'], r['x1']))
    zs = sorted((r['z0'], r['z1']))
    ys = []
    for k, blob in entries:
        if not k.startswith('p0/') or not k[3:].isdigit() or int(k[3:]) >= 400:
            continue
        try:
            ms = meshes(blob)
        except Exception:
            continue
        for pts in ms:
            if not pts:
                continue
            ex = max(p[1] for p in pts) - min(p[1] for p in pts)
            if ex > 6:
                continue
            inside = [p[1] for p in pts if xs[0] <= p[0] <= xs[1] and zs[0] <= p[2] <= zs[1]]
            ys += inside
    if not ys:
        return None
    ys.sort()
    return -ys[len(ys) // 2]  # to Y up


def entrance_section(char):
    """(file, section PACH dict) of a wrestler's entrance section; pac\\evt\\Entrance0*.pac."""
    for f in sorted(glob.glob(os.path.join(GAME, 'pac', 'evt', 'Entrance0*.pac'))):
        d = pac.load(f)
        names, last = [], 0
        o = 0x80C
        while o + 16 <= 0x4000:
            nm = d[o:o + 8].decode('latin1').strip(' \0')
            if not nm or not nm.isdigit():
                break
            st, sz = struct.unpack_from('<II', d, o + 8)
            last = max(last, 0x4000 + st * 0x800 + sz * 256)
            if int(nm) < 1000:
                names.append(int(nm))
            o += 16
        if char not in names:
            continue
        p = (last + 0x7FF) & ~0x7FF
        while d[p:p + 4] != b'BPE ':
            p += 0x800
        for _ in range(names.index(char)):
            p += (struct.unpack_from('<I', d, p + 8)[0] + 16 + 0x7FF) & ~0x7FF
        return os.path.basename(f), dict(pac.walk(pac.bpe_decode(d[p:]), 's', 8))
    raise SystemExit('no entrance pac holds bank %d' % char)


def entrance_path(char):
    """{cue: (frames, [(frame, (x, y_up, z))])} the `Bip` position keys of act 25000, motions 000C0000 (C = cue 0,5,10,15,20)."""
    import yanm
    fname, sec = entrance_section(char)
    x = sec['s/0']
    e, _, n, _f = struct.unpack_from('>4I', x, 0)
    names = [x[o + 4:o + 36].split(b'\0')[0].decode('latin1') for o in range(n, e, 40)][2:]
    ys = [m.start() for m in re.finditer(rb'YANM', x)]
    lay = yanm.image_layouts()
    out = {}
    for nm, y in zip(names, ys):
        if not nm.endswith('0000'):
            continue
        for t in yanm.parse_motion(x, y, lay):
            if t.hash == BIP_HASH and t.keys_pos:
                k = t.keys_pos
                out[int(nm[:4])] = [(0, tuple(k[1]))] if k[0] == 'const' else [(a, tuple(b)) for a, b in k]
    return fname, out


def yaw(dx, dz):
    import math
    return math.degrees(math.atan2(dx, dz))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    arena = int(a[0])
    char = int(a[a.index('--char') + 1]) if '--char' in a else None
    d = pac.load(os.path.join(GAME, 'pac', 'bg', 'bg%02d.pac' % arena))
    entries = list(pac.entries(d, 8))
    rects = arena_rects(d)
    print('arena bg%02d: 50003 rectangles (x0 z0 -> x1 z1, light set)' % arena)
    for r in rects:
        print('  %7.0f %7.0f -> %7.0f %7.0f  light %d' % (r['x0'], r['z0'], r['x1'], r['z1'], r['light']))
    stage = [r for r in rects if r['light'] == 200 or max(r['z0'], r['z1']) > 3000]
    stage = max(rects, key=lambda r: max(r['z0'], r['z1'])) if rects else None
    ramps = [r for r in rects if r is not stage and abs(r['x0'] - r['x1']) <= 700]
    ramp = min(ramps, key=lambda r: min(r['z0'], r['z1'])) if ramps else None
    print('\nspot table (X right, Z toward the stage, Y up; yaw 180 = facing the ring along -Z)')
    if stage:
        zc = (stage['z0'] + stage['z1']) / 2
        print('stage   rect centre (0, %.0f, %.0f) rect x %+.0f..%+.0f z %.0f..%.0f  floor y %s   [yaw 180: toward the ring]' % (
            floor_in_rect(entries, stage) or 0, zc, stage['x1'], stage['x0'], stage['z1'], stage['z0'],
            floor_in_rect(entries, stage)))
    if ramp:
        z_far, z_near = max(ramp['z0'], ramp['z1']), min(ramp['z0'], ramp['z1'])
        cx = (ramp['x0'] + ramp['x1']) / 2
        fy = floor_in_rect(entries, ramp)
        print('ramp    t=0 (%.0f, %s, %.0f)  t=1 (%.0f, %s, %.0f)  width +-%.0f  floor y (flat slabs) %s   [yaw 180]' % (
            cx, fy, z_far, cx, fy, z_near, abs(ramp['x0'] - ramp['x1']) / 2, fy))
    # ring
    for k, blob in entries:
        if k == 'p0/953':
            pts = [p for m in meshes(blob) for p in m]
            ex = [(min(p[i] for p in pts), max(p[i] for p in pts)) for i in range(3)]
            print('ring    canvas x %.1f..%.1f z %.1f..%.1f, y_file %.0f..%.0f (canvas top y_up = %.0f); corner posts at (+-321, +-321)' % (
                ex[0][0], ex[0][1], ex[2][0], ex[2][1], ex[1][0], ex[1][1], -(-111.0)))
    # desk
    try:
        dd = pac.load(os.path.join(GAME, 'pac', 'bg', 'bgEtc.pac'))
        E = dict(pac.entries(dd, 8))
        pts = [p for m in meshes(E['p9/6/0']) for p in m]
        zmin, zmax = min(p[2] for p in pts), max(p[2] for p in pts)
        zd = -775.4
        near = zd + zmax
        print('desk    bgEtc p9/6/0 local z %.0f..%.0f at z %.1f (probe 20911, bg05): ring-side edge z %.0f; ringside floor spot (0, 0, %.0f) = midway apron(-321)..desk  [yaw 0 for a seated announcer]' % (
            zmin, zmax, zd, near, (-321 + near) / 2))
    except Exception as ex:
        print('desk    unreadable', ex)
    if char is not None:
        fname, path = entrance_path(char)
        print('\nentrance walk of bank %d (%s), act 25000, Bip keys (frame: x, y_up, z) first / last, direction yaw' % (char, fname))
        for cue in sorted(path):
            ks = path[cue]
            f0, p0 = ks[0]
            f1, p1 = ks[-1]
            yw = yaw(p1[0] - p0[0], p1[2] - p0[2]) if (abs(p1[0] - p0[0]) + abs(p1[2] - p0[2])) > 20 else None
            print('  cue %2d: %4d keys, f%d (%.0f, %.0f, %.0f) -> f%d (%.0f, %.0f, %.0f)  dir yaw %s' % (
                cue, len(ks), f0, p0[0], p0[1], p0[2], f1, p1[0], p1[1], p1[2], '%.0f' % yw if yw is not None else '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
