"""Session 97: the arena "stage motion key" (pac entry 30000, lights) and the stage slot motions (entry 30001), both "YMKs"
key sets, read from the player's own game files (read-only).  Layouts and numbers only; claims in out\\runtime\\s97\\stage_key.md.

    py -I tools\\assets\\stagekey.py --arena 5 --sets                  list the light sets of entry 30000 (steps, animated tracks)
    py -I tools\\assets\\stagekey.py --arena 5 --set 31 [--frame F]     evaluated light set (5 dirs, 5 colours, sky, ground)
    py -I tools\\assets\\stagekey.py --arena 5 --motions [--frame F]    the 19 slot motions m(101..119) at frame F as 3x4 matrices
    py -I tools\\assets\\stagekey.py --arena 5 --probe CSV --swap N     compare with a gpu draw-probe CSV (local)

Format (both entries; same family as pac\\m2.pac, tools\\assets\\m2mot.py):
- Entry 30000 starts with "YMKs" + 12 bytes (a 256 byte table follows); entry 30001 is the same without those 16 bytes.
  The index = LE u32 N at +0x110 (30000) / +0x100 (30001), N 16-byte records from the same place (record 0's first 4 bytes
  hold N):  {4 bytes, u8 track, u8 0, LE u16 act (30000: the light set id), LE u32 offset, LE u32 size (steps of 2 frames)}.
  Data base = index + 16 N + 4.
- An entry = a token stream: token < 96 skip that many steps; 96 = chunk with a LE u16 length; 97..255 = chunk of token-96 bytes.
- Chunk = optional prefix (first byte 0x80|n then n more bytes: skipped), then `count` (u8: this key lasts (count+1) steps =
  2 (count+1) frames), then a 12 byte position (3 LE float32), then:
    entry 30000 (lights): 3 LE int16 angles (65536 = 360 deg) and 3 LE int16 scales (12 bit: colour = s / 4095)
    entry 30001 (slots) : 3 LE float32 Euler angles (radians) and a float32 1.0 (scale)
"""
import argparse
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'  # worktrees have no game_data
sys.path.insert(0, os.path.join(MAIN, "tools", "wm30"))  # pac.py finds its helper exe relative to its own repo
GAME = os.path.join(MAIN, 'game_data', 'extracted')
import pac  # noqa: E402

ANG = 2 * math.pi / 65536.0


def arena_entries(arena):
    d = pac.load(os.path.join(GAME, 'pac', 'bg', 'bg%02d.pac' % arena))
    return dict(pac.entries(d, 8))


class YMKs:
    def __init__(self, data):
        self.d = data
        self.idx = 0x110 if data[:4] == b'YMKs' else 0x100
        self.n = struct.unpack_from('<I', data, self.idx)[0]
        self.base = self.idx + 16 * self.n + 4
        self.rec = []
        for i in range(self.n):
            p = self.idx + 16 * i
            off, size = struct.unpack_from('<II', data, p + 8)
            self.rec.append((i, struct.unpack_from('<H', data, p + 6)[0], data[p + 4], off, size))
        self.by = {(r[1], r[2]): r for r in self.rec}

    def acts(self):
        return sorted(set(r[1] for r in self.rec))

    def tracks(self, act):
        return sorted(r[2] for r in self.rec if r[1] == act)

    def chunks(self, act, track):
        """[(step, body)] of one entry; body = count byte + data (prefix removed)."""
        r = self.by.get((act, track))
        if r is None:
            return [], 0
        end = self.rec[r[0] + 1][3] if r[0] + 1 < self.n else len(self.d) - self.base
        d = self.d
        p, e, step, out = self.base + r[3], self.base + end, 0, []
        while p < e:
            t = d[p]
            if t < 96:
                step += t
                p += 1
                continue
            if t == 96:
                ln, q = d[p + 1] | d[p + 2] << 8, p + 3
            else:
                ln, q = t - 96, p + 1
            c = d[q:q + ln]
            if c and c[0] & 0x80:
                c = c[(c[0] & 0x7F) + 1:]
            out.append((step, c))
            step += 1
            p = q + ln
        return out, r[4]


def _lerp(a, b, t):
    return a + (b - a) * t


def _lerp_ang(a, b, t):  # int16 angles, the short way round
    dlt = ((b - a + 32768) & 0xFFFF) - 32768
    return a + dlt * t


def light_keys(k, act, track):
    """-> (size_steps, [(step, count, pos[3], ang[3] int16, scale[3] int16)])"""
    ch, size = k.chunks(act, track)
    out = []
    for step, c in ch:
        if len(c) < 25:
            continue
        out.append((step, c[0], struct.unpack_from('<3f', c, 1), struct.unpack_from('<3h', c, 13), struct.unpack_from('<3h', c, 19)))
    return size, out


def sample_light(k, act, track, frame):
    """pos, ang(int16 units), scale at game frame `frame` (looping over the entry's length)."""
    size, ks = light_keys(k, act, track)
    if not ks:
        return None
    t = (frame / 2.0) % size if size else 0.0
    cur = ks[0]
    for i, key in enumerate(ks):
        if key[0] <= t:
            cur = key
            nxt = ks[i + 1] if i + 1 < len(ks) else key
    span = cur[1] + 1
    f = min(1.0, max(0.0, (t - cur[0]) / span)) if nxt is not cur else 0.0
    # keys sit every step or are held (skip tokens): between two keys the value goes linearly from key to next key
    gap = max(1, nxt[0] - cur[0]) if nxt is not cur else 1
    f = min(1.0, max(0.0, (t - cur[0]) / gap)) if nxt is not cur else 0.0
    pos = [_lerp(a, b, f) for a, b in zip(cur[2], nxt[2])]
    ang = [_lerp_ang(a, b, f) for a, b in zip(cur[3], nxt[3])]
    sc = [_lerp(a, b, f) for a, b in zip(cur[4], nxt[4])]
    return pos, ang, sc


def _rz(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def _ry(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def _rx(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def _mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def euler_zyx(ex, ey, ez):
    """column-vector rotation Rz Ry Rx (the game's `v*Rx*Ry*Rz` in row vectors)."""
    return _mm(_mm(_rz(ez), _ry(ey)), _rx(ex))


def light_dir(ang):
    """Direction TO the light = Rz Ry Rx (-1, 0, 0)   [C: reproduces the live source object dirs of set 10 to 1e-3]."""
    m = euler_zyx(ang[0] * ANG, ang[1] * ANG, ang[2] * ANG)
    return [-m[0][0], -m[1][0], -m[2][0]]


def eval_set(k, sid, frame=0):
    """Evaluated light set: dir[5][3], col[5][3], sky[3], ground[3] (colours = scale / 4095)."""
    out = {'id': sid, 'dir': [], 'col': [], 'sky': None, 'ground': None, 'pos': []}
    for tr in (10, 11, 12, 13, 14):
        s = sample_light(k, sid, tr, frame)
        if s is None:
            out['dir'].append([0, 0, 0])
            out['col'].append([0, 0, 0])
            out['pos'].append([0, 0, 0])
        else:
            out['dir'].append(light_dir(s[1]))
            out['col'].append([v / 4095.0 for v in s[2]])
            out['pos'].append(list(s[0]))
    for name, tr in (('sky', 0), ('ground', 1)):
        s = sample_light(k, sid, tr, frame)
        out[name] = [v / 4095.0 for v in s[2]] if s else [0, 0, 0]
    return out


def motion_keys(k, act):
    ch, size = k.chunks(act, 0)
    ks = []
    for step, c in ch:
        if len(c) < 29:
            continue
        f = struct.unpack_from('<7f', c, 1)
        ks.append((step, c[0], f[0:3], f[3:6], f[6]))
    return size, ks


def sample_motion(k, act, frame):
    """-> (pos[3] in world units (key / 100), euler[3] radians, scale) at game frame `frame` (looping)."""
    size, ks = motion_keys(k, act)
    if not ks:
        return None
    t = (frame / 2.0) % size
    cur = nxt = ks[0]
    for i, key in enumerate(ks):
        if key[0] <= t:
            cur = key
            nxt = ks[i + 1] if i + 1 < len(ks) else ks[0]
    gap = (nxt[0] - cur[0]) if nxt[0] > cur[0] else max(1, size - cur[0])
    f = min(1.0, max(0.0, (t - cur[0]) / gap))
    pos = [_lerp(a, b, f) * 0.01 for a, b in zip(cur[2], nxt[2])]
    eul = [_lerp(a, b, f) for a, b in zip(cur[3], nxt[3])]
    return pos, eul, cur[4]


def motion_matrix(pos, eul):
    """3x4 world matrix rows (c76..c78 layout: row i = [R[i][0..2], t_i]); R = Rz Ry Rx."""
    r = euler_zyx(*eul)
    return [r[i] + [pos[i]] for i in range(3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--sets', action='store_true')
    ap.add_argument('--set', type=int)
    ap.add_argument('--motions', action='store_true')
    ap.add_argument('--frame', type=float, default=0)
    ap.add_argument('--probe')
    ap.add_argument('--swap', type=int)
    a = ap.parse_args()
    E = arena_entries(a.arena)
    kl = YMKs(bytes(E['p0/30000']))
    km = YMKs(bytes(E['p0/30001']))
    if a.sets:
        for sid in kl.acts():
            anim = [t for t in kl.tracks(sid) if len(set(c[1:] for s, c in kl.chunks(sid, t)[0])) > 1]
            print('set %3d tracks %s steps %d animated %s' % (sid, kl.tracks(sid), kl.by[(sid, kl.tracks(sid)[0])][4], anim))
    if a.set is not None:
        e = eval_set(kl, a.set, a.frame)
        for i in range(5):
            print('light %d dir %s col %s pos %s' % (i, [round(v, 4) for v in e['dir'][i]], [round(v, 4) for v in e['col'][i]], [round(v, 1) for v in e['pos'][i]]))
        print('sky', [round(v, 4) for v in e['sky']], 'ground', [round(v, 4) for v in e['ground']])
    if a.motions:
        for act in km.acts():
            s = sample_motion(km, act, a.frame)
            m = motion_matrix(s[0], s[1])
            print('m(%d) pos %s eul %s' % (act, [round(v, 2) for v in s[0]], [round(v, 3) for v in s[1]]))
    if a.probe:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))
        from probe_camera import rows, reg
        seen = {}
        for row in rows(a.probe, a.swap):
            m = [reg(row, n) for n in (76, 77, 78)]
            seen.setdefault(tuple(round(m[i][3], 2) for i in range(3)), (row['draw'], m))
        for act in km.acts():
            ks = motion_keys(km, act)[1]
            p = ks[0][2]
            t = tuple(round(v * 0.01, 2) for v in p)
            if t not in seen:
                continue
            m = seen[t][1]
            best = None
            for step, cnt, pos, eul, sc in ks:
                r = euler_zyx(*eul)
                err = max(abs(r[i][j] - m[i][j]) for i in range(3) for j in range(3))
                if best is None or err < best[0]:
                    best = (err, step)
            print('m(%d) draw %s best key step %d (frame %d) max err %.4f' % (act, seen[t][0], best[1], best[1] * 2, best[0]))


if __name__ == '__main__':
    main()
