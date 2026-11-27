r"""S111 VTXEXTENT: a draw-diff companion. For each draw of a STREAM capture whose model key is known, the bounding box of the draw's vertex positions in MODEL space,
against the bounding box of OUR loaded assets::Model for the same key (our loader run offline), and a verdict per key: OK / SCALE xN / MISMATCH.
Offline, 0 game runs; reads the capture (tools\particles\pcap.py) and the player's own pac files, writes nothing but the exe it builds under out\tools.

  py -I tools\scene\vtxextent.py CAPTURE_DIR [--frame N] [--arena N] [--ours-exe EXE] [--key 0xOBJECT=bgNN:KEY ...] [--all] [--tol 0.02]
  py -I tools\scene\vtxextent.py --selftest [--plant ratio_ignored]

Space [C]: a stream draw's vertex bytes are the MODEL's vertices as the game's loaded model holds them; the world matrix is a separate constant (c76..c78), never folded in. Proof: for
every joined stage draw the stream's positions equal the file mesh's positions bit for bit (tools\arena\join_census.py match_draw: `maxdiff == 0.0`). Nothing is undone here.
Ours is in the viewer's space (FinishModel mirrors y), so the verdict compares per-axis WIDTHS (hi - lo), which a mirror does not change.

How a draw gets its model key (two routes, both printed):
  1. --key 0xOBJ=bgNN:KEY  the origin's `object` (the game's model object address) named by the caller, as MATDRAW2 / MATLIVE did for the splat model (0xAB19A280 = [[0x83767DE4]+8] = bg key 32000).
  2. content join (default): the stream's float vertex element equals a file mesh's positions times ONE uniform factor, vertex for vertex (same vertex count). The factor is NOT assumed 1,
     so a model the game created at a scale (MATLIVE: x 10) still joins. The key comes from the pac entry; the mesh from its record index.
A draw joined by neither is not compared (counted). A mesh the join finds in several entries (same bytes) is listed under all of them.
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
for sub in ('particles', 'arena', 'wm30'):
    sys.path.insert(0, os.path.join(ROOT, 'tools', sub))

VK_R32G32B32_SFLOAT, VK_R32G32B32A32_SFLOAT = 106, 109
DRAW_NO_GEOMETRY = 8   # adapter_frame.h kDrawNoGeometry: the capture holds no vertex bytes for this draw
PLANTS = ('ratio_ignored',)


# ---------------------------------------------------------------- the verdict (pure)
def widths(box):
    lo, hi = box
    return [float(hi[k]) - float(lo[k]) for k in range(3)]


def fmt_ratio(s):
    t = '%.3g' % s
    return 'x' + t


def verdict(game, ours, tol=0.02, plant=None):
    """game, ours: (lo[3], hi[3]) in model space. -> (verdict text, ratio or None, per-axis ratios)."""
    gw, ow = widths(game), widths(ours)
    eps = 1e-4 * max(1.0, max(gw + ow))
    rs, axes = [], []
    for k in range(3):
        if gw[k] < eps and ow[k] < eps:
            continue                       # flat on both sides (the splat quad's y): no information on this axis
        if gw[k] < eps or ow[k] < eps:
            return 'MISMATCH', None, [None if ow[k] < eps else gw[k] / ow[k] for k in range(3)]
        rs.append(gw[k] / ow[k])
        axes.append(k)
    per_axis = [None, None, None]
    for k, r in zip(axes, rs):
        per_axis[k] = r
    if not rs:
        return 'OK', 1.0, per_axis
    mean = sum(rs) / len(rs)
    if max(rs) - min(rs) > tol * mean:
        return 'MISMATCH', None, per_axis
    if abs(mean - 1.0) <= tol or plant == 'ratio_ignored':   # the planted twin: a uniform ratio is not looked at
        return 'OK', mean, per_axis
    return 'SCALE ' + fmt_ratio(mean), mean, per_axis


def union(boxes):
    lo = [min(b[0][k] for b in boxes) for k in range(3)]
    hi = [max(b[1][k] for b in boxes) for k in range(3)]
    return lo, hi


def compare(game, ours, tol=0.02, plant=None):
    """game / ours: {(key, record or None): [box, ...]} (record None = the whole key). -> rows [(key, record, n_draws, game box, ours box or None, verdict, ratio)]."""
    rows = []
    for (key, rec), gb in sorted(game.items(), key=lambda kv: (kv[0][0], -1 if kv[0][1] is None else kv[0][1])):
        g = union(gb)
        if rec is None:
            ob = [b for (k2, r2), bl in ours.items() if k2 == key for b in bl]
        else:
            ob = ours.get((key, rec), [])
        if not ob:
            rows.append((key, rec, len(gb), g, None, 'NO-OURS', None))
            continue
        v, r, _ = verdict(g, union(ob), tol, plant)
        rows.append((key, rec, len(gb), g, union(ob), v, r))
    return rows


# ---------------------------------------------------------------- the stream side
def position_element(f, d):
    """(n, 3) float32 positions of a draw (its first float3 / float4 vertex element), or None."""
    if int(d['flags']) & DRAW_NO_GEOMETRY:
        return None
    L = f.layouts[int(d['layout'])]
    n, stride = int(d['vertex_count']), int(L['stride'])
    raw = f.cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
    if not n or raw.size < n * stride:
        return None
    rows = raw[:n * stride].reshape(n, stride)
    for k in range(int(L['count'])):
        e = f.elements[int(L['first']) + k]
        if int(e['fmt']) in (VK_R32G32B32_SFLOAT, VK_R32G32B32A32_SFLOAT):
            return np.ascontiguousarray(rows[:, int(e['off']):int(e['off']) + 12]).view('<f4').reshape(n, 3)
    return None


def box_of(p):
    return [float(x) for x in p.min(axis=0)], [float(x) for x in p.max(axis=0)]


def uniform_factor(stream, file_pos, rel=1e-4):
    """s with stream == s * file_pos vertex for vertex (positive, finite), else None."""
    if stream.shape != file_pos.shape or not len(file_pos):
        return None
    ext = file_pos.max(axis=0) - file_pos.min(axis=0)
    k = int(np.argmax(ext))
    if not ext[k] > 1e-9:
        return None
    sext = stream[:, k].max() - stream[:, k].min()
    s = float(sext / ext[k])
    if not (s > 0 and np.isfinite(s)):
        return None
    if np.abs(stream - s * file_pos).max() > rel * max(1.0, float(np.abs(stream).max())):
        return None
    return s


# ---------------------------------------------------------------- the file side (all JBOY entries of a bg pac, positions as stored)
def file_meshes(arena_id):
    import arena_files as af
    pac = af.pac
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    raw = {}
    for eid, b in pac.pach(data, p):
        raw.setdefault(eid, b)
    out = {}
    for eid in sorted(raw):
        if eid >= 50000:
            continue                       # text / light tables
        try:
            dec = pac.decode(raw[eid])
            if dec[:4] != b'JBOY':
                continue
            ms = af.parse_jboy(eid, dec, True)
        except Exception:
            continue
        for m in ms:
            if m.positions is not None and m.n >= 3:
                out[(eid, m.index)] = m.positions
    return out


def arena_of(f):
    """The bg pac id the capture's string table names, else None."""
    if 67 not in f.secs:
        return None
    es, cnt, off = f.secs[67]
    blob = bytes(f.buf[off:off + max(0, len(f.buf) - off)])
    m = re.search(rb'pac\\bg\\bg(\d+)\.pac', blob[:20000])
    return int(m.group(1)) if m else None


def game_boxes(cap, fr, arena_id, key_map, join=True):
    """-> (game {(key, rec): [box]}, stats). key_map: {object: (arena, key)} (route 1); join: the content route (route 2)."""
    game, st = {}, dict(draws=len(fr.draws), no_geometry=0, by_object=0, joined=0, unjoined=0)
    fm = file_meshes(arena_id) if join else {}
    by_n = {}
    for (key, rec), pos in fm.items():
        by_n.setdefault(pos.shape[0], []).append((key, rec, pos))
    cache = {}
    for i in range(len(fr.draws)):
        d = fr.draws[i]
        pos = position_element(fr, d)
        if pos is None:
            st['no_geometry'] += 1
            continue
        obj = int(fr.origins[i]['object']) if fr.has_origins else 0
        if obj in key_map and key_map[obj][0] == arena_id:
            game.setdefault((key_map[obj][1], None), []).append(box_of(pos))
            st['by_object'] += 1
            continue
        ck = (int(d['vertex_offset']), int(d['vertex_bytes']))
        hit = cache.get(ck)
        if hit is None:
            hit = []
            for key, rec, fp in by_n.get(pos.shape[0], []):
                s = uniform_factor(pos, fp)
                if s is not None:
                    hit.append((key, rec, s))
            cache[ck] = hit
        if not hit:
            st['unjoined'] += 1
            continue
        st['joined'] += 1
        key, rec, _ = hit[0]
        game.setdefault((key, rec), []).append(box_of(pos))
    return game, st


# ---------------------------------------------------------------- our side
def ours_boxes(exe, arena_id, game_dir):
    p = subprocess.run([exe, '--game', game_dir, '--arena', str(arena_id)], capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        raise SystemExit('vtxextent: %s failed (exit %d): %s' % (exe, p.returncode, p.stderr.strip()[:300]))
    ours, errs = {}, []
    for line in p.stdout.splitlines():
        t = line.split()
        if t and t[0] == 'M' and len(t) == 11:
            key, rec = int(t[2]), int(t[3])
            nums = [float(x) for x in t[5:11]]
            ours.setdefault((key, rec), []).append((nums[0:3], nums[3:6]))
        elif t and t[0] == 'E':
            errs.append(line)
    return ours, errs


def report(rows, show_all):
    n_ok = sum(1 for r in rows if r[5] == 'OK')
    print('%-7s %-5s %6s  %-30s %-30s %-8s %s' % ('key', 'mesh', 'draws', 'game widths (model space)', 'ours widths', 'ratio', 'verdict'))
    for key, rec, n, g, o, v, r in rows:
        if v == 'OK' and not show_all:
            continue
        gw = ' '.join('%.3g' % w for w in widths(g))
        ow = ' '.join('%.3g' % w for w in widths(o)) if o else '-'
        print('%-7d %-5s %6d  %-30s %-30s %-8s %s' % (key, '-' if rec is None else rec, n, gw, ow, '-' if r is None else fmt_ratio(r), v))
    bad = [r for r in rows if r[5] not in ('OK', 'NO-OURS')]
    print('keys compared: %d (OK %d, SCALE/MISMATCH %d, no ours %d)' % (len(rows), n_ok, len(bad), sum(1 for r in rows if r[5] == 'NO-OURS')))


# ---------------------------------------------------------------- the selftest
def selftest(plant):
    fails = []

    def chk(name, got, want):
        if got != want:
            fails.append('%s: got %r want %r' % (name, got, want))
    box = lambda a, b: ([-a, -b, -a], [a, b, a])
    chk('equal', verdict(box(1, 1), box(1, 1), plant=plant)[0], 'OK')
    chk('x10', verdict(box(10, 5), box(1, 0.5), plant=plant)[0], 'SCALE x10')            # the planted twin (ratio ignored) reads this as OK
    chk('x0.1', verdict(box(1, 0.5), box(10, 5), plant=plant)[0], 'SCALE x0.1')
    chk('nonuniform', verdict(([-10, -1, -1], [10, 1, 1]), box(1, 1), plant=plant)[0], 'MISMATCH')
    # the splat: flat in y on both sides, x / z +-10 against +-1
    chk('flat_y_x10', verdict(([-10, -1, -10], [10, -1, 10]), ([-1, 0.1, -1], [1, 0.1, 1]), plant=plant)[0], 'SCALE x10')
    chk('one_flat', verdict(([-1, 0, -1], [1, 0, 1]), box(1, 1), plant=plant)[0], 'MISMATCH')
    # the join: the stream is a uniform multiple of the file's vertices, or not
    fp = np.array([[1, -0.1, 1], [1, -0.1, -1], [-1, -0.1, -1], [-1, -0.1, 1]], np.float32)
    chk('join_x10', round(uniform_factor(fp * 10, fp) or 0, 3), 10.0)
    chk('join_x1', round(uniform_factor(fp.copy(), fp) or 0, 3), 1.0)
    bent = fp * 10
    bent[2, 0] += 1.0
    chk('join_bent_refused', uniform_factor(bent, fp), None)
    # compare(): per (key, record) and per whole key
    rows = compare({(32000, None): [box(10, 1)]}, {(32000, 0): [box(1, 0.1)]}, plant=plant)
    chk('compare_key', [r[5] for r in rows], ['SCALE x10'])
    rows = compare({(7, 0): [box(2, 2)]}, {(7, 0): [box(2, 2)], (7, 1): [box(9, 9)]}, plant=plant)
    chk('compare_record', [r[5] for r in rows], ['OK'])
    rows = compare({(9, 0): [box(1, 1)]}, {}, plant=plant)
    chk('compare_no_ours', [r[5] for r in rows], ['NO-OURS'])
    for f in fails:
        print('FAIL  ' + f)
    print('vtxextent selftest%s: %s' % (' (plant %s)' % plant if plant else '', 'FAILED (%d)' % len(fails) if fails else 'all checks passed'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--frame', type=int, default=-1, help='frame file index (default: the last)')
    ap.add_argument('--arena', type=int, default=None)
    ap.add_argument('--ours-exe', default=os.path.join(ROOT, 'out', 'tools', 'vtxextent_ours.exe'))
    ap.add_argument('--game', default=None, help='game files folder (default: the pac reader\'s)')
    ap.add_argument('--key', action='append', default=[], help='0xOBJECT=bgNN:KEY (the origin object named by the caller)')
    ap.add_argument('--no-join', action='store_true', help='only the --key objects')
    ap.add_argument('--all', action='store_true', help='print OK rows too')
    ap.add_argument('--tol', type=float, default=0.02)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--plant', choices=PLANTS)
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.plant)
    if not a.capture:
        ap.error('a capture folder is required (or --selftest)')
    import pcap
    cap = pcap.Capture(a.capture)
    fr = cap.frame(a.frame if a.frame >= 0 else len(cap.frames) - 1)
    arena = a.arena if a.arena is not None else arena_of(fr)
    if arena is None:
        raise SystemExit('vtxextent: the capture names no bg pac; pass --arena N')
    key_map = {}
    for kv in a.key:
        m = re.fullmatch(r'(0x[0-9A-Fa-f]+|\d+)=bg(\d+):(\d+)', kv)
        if not m:
            raise SystemExit('vtxextent: --key wants 0xOBJECT=bgNN:KEY, got %r' % kv)
        key_map[int(m.group(1), 0)] = (int(m.group(2)), int(m.group(3)))
    game, st = game_boxes(cap, fr, arena, key_map, join=not a.no_join)
    import arena_files as af
    game_dir = a.game or af.pac.GAME
    ours, errs = ours_boxes(a.ours_exe, arena, game_dir)
    print('capture %s frame file %d (guest %d) bg%02d; ours: %s' % (a.capture, fr.idx, fr.guest_frame, arena, a.ours_exe))
    print('draws %(draws)d: by object %(by_object)d, joined by content %(joined)d, not keyed %(unjoined)d, no geometry %(no_geometry)d' % st)
    for e in errs:
        print('ours loader said: ' + e)
    rows = compare(game, ours, a.tol, a.plant)
    report(rows, a.all)
    return 0


if __name__ == '__main__':
    sys.exit(main())
