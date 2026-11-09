"""Session 97: the ringside crowd of an arena, from the player's own bg pac - seat rows, the people list and the check
against the arena's own chair meshes. Our code; reads game files only, prints numbers, writes nothing.

  py -I tools\\assets\\crowd.py [--arena 5] [--game game_data\\extracted] [--set 20000|20010] [--show 8]

What it does (rule documented in out\\runtime\\s97\\crowd.md, from the guest parser sub_8250ECC0):
  rows    "<letter>(x0, z0, dx, dz); count"  -> seat k of the row is at (x0 + dx*k, 0, z0 + dz*k). The count after the
          ';' is NOT read by the game (it is a note for the editor); the real number of seats is the people list.
  people  "[N] n" starts person type N; each line "<letter><2 digits>[(b<NN>)]" is one person: row letter, seat k, and
          an optional variant NN (the text 'c' / 'w' make extra child objects instead).
  yaw     every person faces the arena origin: ang = atan2(z, -x) - pi/2 wrapped to [-pi, pi], then quantised to 128
          steps of 2*pi/128 (the game keeps 7 bits).
  check   seat positions against the arena_chair meshes of the same pac (needs numpy): each seat has one chair whose
          centre lies a fixed distance behind it.
Needs tools\\wm30\\pac.py (pac reader) and out\\tools\\xctd_unpack.exe (see tools\\wm30\\build.ps1) for XCTD pacs.
"""
import argparse
import math
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import pac  # noqa: E402

ANG_SCALE = 651.8986206054688  # the game's constant: 4096 / (2*pi) as a float
SEATS_FMT = re.compile(r'^([A-Za-z])\(([-+0-9.eE]+),([-+0-9.eE]+),([-+0-9.eE]+),([-+0-9.eE]+)\)')
TYPE_FMT = re.compile(r'^\[(\d+)\](\d+)')


def lines_like_the_game(text):
    """The game's tokenizer: drop spaces and tabs, a ';' ends the line's content, CR/LF end lines, empty lines vanish."""
    out = []
    for raw in re.split(r'[\r\n]+', text):
        raw = raw.split(';', 1)[0]
        raw = raw.replace(' ', '').replace('\t', '')
        if raw:
            out.append(raw)
    return out


def parse_crowd(text):
    """-> (rows{letter: (x0, z0, dx, dz)}, people[ {type, row, seat, variant, kind} ])"""
    ls = lines_like_the_game(text)
    rows, people = {}, []
    i = 0
    while i < len(ls) and ls[i][0] != '{':
        i += 1
    i += 1
    while i < len(ls) and ls[i][0] != '}':  # seat rows
        m = SEATS_FMT.match(ls[i])
        if m:
            rows[m.group(1)] = tuple(float(m.group(k)) for k in range(2, 6))
        i += 1
    i += 1
    while i < len(ls):  # people: "[N] n" then "{", lines, "}"
        m = TYPE_FMT.match(ls[i])
        if not m or int(m.group(1)) > 60:
            i += 1
            continue
        t = int(m.group(1))
        i += 1
        while i < len(ls) and ls[i][0] != '{':
            i += 1
        i += 1
        while i < len(ls) and ls[i][0] != '}':
            s = ls[i]
            if len(s) >= 3 and s[0].isalpha() and s[1].isdigit() and s[2].isdigit():
                p = {'type': t, 'row': s[0], 'seat': int(s[1:3]), 'variant': None, 'kind': 'plain'}
                if len(s) > 4 and s[3] == '(':
                    c = s[4]
                    if c == 'b' and len(s) >= 7:
                        p['variant'], p['kind'] = int(s[5:7]), 'b'
                    elif c == 'c':
                        p['kind'] = 'c'
                    elif c == 'w':
                        p['kind'] = 'w'
                people.append(p)
            i += 1
        i += 1
    return rows, people


def place(rows, p):
    x0, z0, dx, dz = rows[p['row']]
    x, z = x0 + dx * p['seat'], z0 + dz * p['seat']
    ang = math.atan2(z, -x) - math.pi / 2          # sub_82C32810(person, origin)
    while ang > math.pi:                            # sub_82C3DAB0 wraps into [-pi, pi]
        ang -= 2 * math.pi
    while ang < -math.pi:
        ang += 2 * math.pi
    q = (int((ang + 2 * math.pi) * ANG_SCALE) >> 5) & 0x7F   # 7 bits kept (lhz, shift 5, mask 7F)
    return x, 0.0, z, q, q * 2 * math.pi / 128


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def jboy_vertices(blob):
    import numpy as np
    body = blob[8:]
    nm, mt = be32(body, 0x10), be32(body, 0x14)
    out = []
    for mi in range(nm):
        rec = mt + mi * 0xB8
        n, s0 = be32(body, rec), be32(body, rec + 0x68)
        a = np.frombuffer(body[s0 + 4:s0 + 4 + 28 * n], dtype=np.dtype([('p', '>f4', 3), ('r', 'V16')]))
        out.append(a['p'].astype(np.float64))
    return out


def node_name(blob):
    body = blob[8:]
    nt = be32(body, 0x20)
    return body[nt:nt + 16].split(b'\0')[0].decode('latin1')


# ---- the stands crowd (block crowd of sub_82513D78) -------------------------------------------------------------
SPACING_A, SPACING_B = 92.0, 105.0  # steps along edge A (corner 0 -> 1) and edge B (corner 0 -> 2); x1.25 / 115 / 131.25 in the game's low mode


def quad_lines(text):
    out = []
    for l in text.splitlines():
        l = l.strip()
        if not l or l.startswith(';'):
            continue
        v = [float(x) for x in l.split()]
        if len(v) >= 12:
            out.append(v[:12])
    return out


def gen_quad(q):
    """One zone quad (12 numbers = 4 corners x (x, z, h), decimetres) -> ([world positions], yaw, (nA, nB, nC)).
    Corner world position = (x*10, -h*10, z*10) (the arena file is -Y up). Rule: sub_82513D78 0x82515774..0x825162F4."""
    import numpy as np
    P = [np.array([q[3 * i] * 10, -q[3 * i + 2] * 10, q[3 * i + 1] * 10]) for i in range(4)]
    la, lb, lc = (float(np.linalg.norm(P[1] - P[0])), float(np.linalg.norm(P[2] - P[0])), float(np.linalg.norm(P[3] - P[2])))
    na, nb, nc = int(la / SPACING_A + 1), int(lb / SPACING_B + 1), int(lc / SPACING_A + 1)
    sa = (P[1] - P[0]) / max(la, 1e-9) * SPACING_A
    sb = (P[2] - P[0]) / max(lb, 1e-9) * SPACING_B
    pts = []
    for r in range(nb):
        cnt = int((nb - r) * na / nb + r * nc / nb + 0.5)       # row length: edge A's count blends to edge C's
        base = P[0] + r * sb + (0.5 * sa if r & 1 else 0)         # odd rows shifted half a step
        pts += [base + k * sa for k in range(cnt)]
    yaw = math.atan2(-(P[1][2] - P[0][2]), P[1][0] - P[0][0])   # sub_82C328F8(corner0, corner1), one yaw per quad
    return pts, yaw, (na, nb, nc)


def stands_report(leaves, set_id, show):
    import numpy as np
    names = ('NR', 'R')
    all_pts = []
    for key, tag in ((21201, 'NR'), (21202, 'R')):
        b = leaves.get('p0/%d/%d' % (set_id, key))
        if b is None:
            continue
        qs = quad_lines(b.decode('cp932', 'replace'))
        n = 0
        for qi, q in enumerate(qs):
            pts, yaw, cnt = gen_quad(q)
            n += len(pts)
            all_pts += pts
            if qi < show:
                print('  %s quad %2d corners(dm) %s -> counts A/B/C %s, %3d members, yaw %.1f deg, first %s' % (
                    tag, qi, [round(v, 1) for v in q[:6]] + ['...'], cnt, len(pts), math.degrees(yaw), np.round(pts[0]).tolist() if pts else None))
        print('%s (%d): %d quads, %d members (x2 with the mirrored half = %d)' % (tag, key, len(qs), n, 2 * n))
    # check against the stand meshes (Sta_Seat*): is each member's height inside the local stand height range?
    sv = []
    for name, blob in leaves.items():
        parts = name.split('/')
        if len(parts) == 2 and blob[:4] == b'JBOY' and node_name(blob).startswith('Sta_Seat'):
            sv.append(np.concatenate(jboy_vertices(blob)))
    if not sv:
        print('no Sta_Seat meshes: no check')
        return
    sv = np.concatenate(sv)
    sv = np.concatenate([sv, sv * [-1, 1, 1]])  # the file holds the -x half; the game mirrors it
    ok = n_near = 0
    for p in all_pts:
        d = np.hypot(sv[:, 0] - p[0], sv[:, 2] - p[2])
        sel = sv[d < 400]
        if len(sel) == 0:
            continue
        n_near += 1
        ok += sel[:, 1].min() - 100 <= p[1] <= sel[:, 1].max() + 100
    print('check vs %d Sta_Seat vertices: %d of %d members have stand vertices within 400 (xz); %.0f%% of those lie inside the local stand height range +-100' % (
        len(sv), n_near, len(all_pts), 100.0 * ok / max(n_near, 1)))
    print('  (stand meshes are coarse, so nearest-vertex distances are large; the height test with y = -h*10 passes, y = +h*10 would not)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--game', default=os.path.join(REPO, 'game_data', 'extracted'))
    ap.add_argument('--set', type=int, default=20000, choices=(20000, 20010))
    ap.add_argument('--show', type=int, default=8)
    ap.add_argument('--stands', action='store_true', help='print the stands crowd (zone quads 21201/21202) instead of the seated people')
    a = ap.parse_args()
    path = os.path.join(a.game, 'pac', 'bg', 'bg%02d.pac' % a.arena)
    data = pac.load(path)
    leaves = dict(pac.entries(data, 2))
    text = None
    for key in (a.set + 100, a.set + 101):  # 21100 (set A) / 20010's own 21100; 21101 is the variant with (w) marks
        pass
    if a.stands:
        stands_report(leaves, a.set, a.show)
        return 0
    text_bytes = leaves.get('p0/%d/21100' % a.set)
    if text_bytes is None:
        print('arena %d: no seat text in set %d' % (a.arena, a.set))
        return 1
    text = text_bytes.decode('cp932', 'replace')
    rows, people = parse_crowd(text)
    seats = {(r, k) for r, v in rows.items() for k in range(0, 100)}  # upper bound only
    used = {}
    for p in people:
        used.setdefault((p['row'], p['seat']), []).append(p['type'])
    dup = [k for k, v in used.items() if len(v) > 1]
    print('arena %d set %d: %d rows, %d people in %d types, %d distinct seats used, %d seats listed twice' %
          (a.arena, a.set, len(rows), len(people), len({p['type'] for p in people}), len(used), len(dup)))
    kinds = {}
    for p in people:
        kinds[p['kind']] = kinds.get(p['kind'], 0) + 1
    print('flags:', kinds, ' variants used:', sorted({p['variant'] for p in people if p['variant'] is not None}))
    nrow = {}
    for p in people:
        nrow[p['row']] = max(nrow.get(p['row'], -1), p['seat'])
    print('rows (x0, z0, dx, dz; highest seat index used):')
    for r, v in rows.items():
        print('  %s  %8.1f %8.1f %7.1f %7.1f   seat max %d' % (r, v[0], v[1], v[2], v[3], nrow.get(r, -1)))
    print('first people (type -> body entry 20000+type near / 20040+type mid / 20080+type far inside set %d; texture aud_<type>a):' % (
        a.set))
    for p in people[:a.show]:
        x, y, z, q, yaw = place(rows, p)
        print('  type %2d %s%02d  pos (%8.1f, %.0f, %8.1f)  yaw step %3d = %6.1f deg  variant %s' %
              (p['type'], p['row'], p['seat'], x, y, z, q, math.degrees(yaw), p['variant']))
    # --- the check against the arena's own chairs
    try:
        import numpy as np
    except ImportError:
        print('numpy missing: chair check skipped')
        return 0
    chairs = []
    for name, blob in leaves.items():
        parts = name.split('/')
        if len(parts) == 2 and blob[:4] == b'JBOY' and node_name(blob).startswith('arena_chair'):
            v = jboy_vertices(blob)[0]
            chairs.append((node_name(blob), v))
    if not chairs:
        print('no arena_chair meshes in this pac: no check')
        return 0
    per_chair = 12  # bg05: each chair is 12 vertices in the first mesh of its slot [measured: 102 seats * 12 = 1224]
    cents = np.concatenate([v.reshape(-1, per_chair, 3).mean(1) for _, v in chairs if len(v) % per_chair == 0])
    allv = np.concatenate([v for _, v in chairs])
    pos = np.array([(place(rows, p)[0], place(rows, p)[2]) for p in people])  # (x, z)
    seat_xz = {}
    for p, xz in zip(people, pos):
        seat_xz[(p['row'], p['seat'])] = xz
    sx = np.array(list(seat_xz.values()))
    d_v = np.array([np.hypot(allv[:, 0] - s[0], allv[:, 2] - s[1]).min() for s in sx])
    d_c = np.array([np.hypot(cents[:, 0] - s[0], cents[:, 2] - s[1]).min() for s in sx])
    j = np.array([np.hypot(cents[:, 0] - s[0], cents[:, 2] - s[1]).argmin() for s in sx])
    print('check: %d seats against %d chair meshes (%d chairs of %d vertices, %d vertices)' %
          (len(sx), len(chairs), len(cents), per_chair, len(allv)))
    print('  nearest chair VERTEX: median %.1f max %.1f units' % (np.median(d_v), d_v.max()))
    print('  nearest chair CENTRE: median %.1f max %.1f units; distinct chairs hit %d of %d seats' %
          (np.median(d_c), d_c.max(), len(set(j.tolist())), len(sx)))
    off = cents[j][:, [0, 2]] - sx
    print('  chair centre - seat: mean (%.1f, %.1f)  (mean over all sections; per section the chair sits ~44 units OUTWARD of the seat, away from the ring)' %
          tuple(off.mean(0)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
