r"""Session 97: crowd body rig reader (our code; reads the player's own arena pac, prints numbers, writes nothing).

    py -I tools\assets\crowd_pose.py [--arena 5] [--body 0] [--lod 0|1|2] [--frame 0] [--arms-down] [--scan]

What it prints (per node of the 17-node 1/10-scale crowd rig; JBOY entry 20000 + 40*lod + body of key 20000):
  the local translation, the local rotation stored in the model's node table (euler x y z in radians; also as a quaternion x y z w),
  the parent, the node-name hash (same function as the wrestler tracks) and the bind world position.
--frame 0  = the stored (bind) pose: NO crowd motion exists in any pac (see out\runtime\s97\crowd_pose.md), so frame 0 is the bind pose.
--arms-down  = a procedural rest pose (NOT from game data): both clavicle nodes turned to hang the arms; prints the skinned
               (2-influence, CPU) bounding box of the body in the bind pose and in this pose, to show the pose is sane.
--scan  = look for the crowd node-name hashes in every mot pac / audience.pac / this arena pac (all hits are expected to be zero).
Rule documented in out\runtime\s97\crowd_pose.md.
"""
import argparse
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))
import pac  # noqa: E402
import yanm  # noqa: E402

try:
    import numpy as np
except ImportError:
    np = None


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def bf(b, o):
    return struct.unpack_from('>f', b, o)[0]


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def euler_q(ex, ey, ez):
    """q = qz * qy * qx (the wrestler tracks' order, yanm.euler_quat)."""
    def ax(a, i):
        v = [0.0, 0.0, 0.0, math.cos(a / 2)]
        v[i] = math.sin(a / 2)
        return tuple(v)
    return qmul(qmul(ax(ez, 2), ax(ey, 1)), ax(ex, 0))


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def read_body(blob):
    """-> dict(nodes, pal = node index per palette slot, pos (n,3), skin [(w0, s0, w1, s1)] per vertex)."""
    b = blob[8:]
    mt, nn, nt = be32(b, 0x14), be32(b, 0x18), be32(b, 0x20)
    nodes = []
    for i in range(nn):
        r = nt + 80 * i
        nodes.append(dict(name=b[r:r + 16].split(b'\0')[0].decode('latin1'),
                          t=[bf(b, r + 16 + 4 * j) for j in range(3)], rot=[bf(b, r + 32 + 4 * j) for j in range(3)],
                          parent=struct.unpack_from('>i', b, r + 48)[0]))
    rec = mt  # the crowd bodies have one mesh
    n, npal = be32(b, rec), be32(b, rec + 8)
    pal = [be32(b, rec + 12 + 4 * k) - 1 for k in range(npal)]  # palette words are node index + 1 [measured: 17 distinct values 1..17]
    s0, s1 = be32(b, rec + 0x68), be32(b, rec + 0x6C)
    pos = np.frombuffer(b[s0 + 4:s0 + 4 + 28 * n], dtype=np.dtype([('p', '>f4', 3), ('r', 'V16')]))['p'].astype(np.float64)
    skin = []
    for v in range(n):
        o = s1 + 4 + 16 * v
        skin.append((bf(b, o), struct.unpack_from('<I', b, o + 4)[0], bf(b, o + 8), struct.unpack_from('<I', b, o + 12)[0]))
    return dict(nodes=nodes, pal=pal, pos=pos, skin=skin)


def world(nodes, rot_over=None):
    """Node world matrices (4x4, column vectors) from the node table; rot_over {node: (ex, ey, ez)} replaces a local rotation."""
    W = []
    for i, nd in enumerate(nodes):
        rot = (rot_over or {}).get(i, nd['rot'])
        M = np.eye(4)
        M[:3, :3] = qmat(euler_q(*rot))
        M[:3, 3] = nd['t']
        W.append(W[nd['parent']] @ M if nd['parent'] >= 0 else M)
    return W


def skin_positions(body, W_bind, W_pose):
    pal = body['pal']
    out = np.zeros_like(body['pos'])
    for v, (w0, s0, w1, s1) in enumerate(body['skin']):
        p = np.append(body['pos'][v], 1.0)
        acc = np.zeros(3)
        # the game's skin loop (sub_82508738): bone 255 = none; with no first bone the second one takes the whole weight, otherwise
        # w0 for the first and (1 - w0) for the second [file record = (weight BE float, slot LE u32) x 2, measured on bg05 body 0]
        pairs = ((1.0, s1),) if s0 == 255 else ((w0, s0), (1.0 - w0, s1))
        for w, s in pairs:
            if w == 0 or s >= len(pal):
                continue
            nid = pal[s]
            acc += w * ((W_pose[nid] @ np.linalg.inv(W_bind[nid]) @ p)[:3])
        out[v] = acc
    return out


LEAVES = {}


def scan_hashes(arena):
    names = [n['name'] for n in read_body(LEAVES['p0/20000/20000'])['nodes'] if n['name'] != 'root']
    keys = {n: struct.pack('>I', yanm.node_hash(n)) for n in names}
    hits = 0
    paths = [os.path.join(yanm.PACDIR, 'mot0%d.pac' % i) for i in (0, 1, 2, 3, 4, 7, 8)]
    paths += [os.path.join(yanm.PACDIR, 'audience.pac'), os.path.join(yanm.PACDIR, 'bg', 'bg%02d.pac' % arena)]
    for p in paths:
        data = pac.load(p)
        for label, blob in [('raw', data)] + list(pac.entries(data, 4)):
            found = [n for n, k in keys.items() if k in blob]
            if len(found) >= 3:
                hits += 1
                print('  HIT %s %s %s' % (os.path.basename(p), label, found))
    print('scan: %d leaves/files carry >= 3 of the %d crowd node hashes' % (hits, len(keys)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--body', type=int, default=0)
    ap.add_argument('--lod', type=int, default=0, choices=(0, 1, 2))
    ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--arms-down', action='store_true')
    ap.add_argument('--scan', action='store_true')
    a = ap.parse_args()
    if np is None:
        print('numpy needed')
        return 1
    data = pac.load(os.path.join(REPO, 'game_data', 'extracted', 'pac', 'bg', 'bg%02d.pac' % a.arena))
    LEAVES.update(dict(pac.entries(data, 3)))
    key = 'p0/20000/%d' % (20000 + 40 * a.lod + a.body)
    if key not in LEAVES:
        print('no', key)
        return 1
    body = read_body(LEAVES[key])
    nodes = body['nodes']
    Wb = world(nodes)
    print('%s: %d nodes, %d vertices, palette (slot -> node) %s' % (key, len(nodes), len(body['pos']), body['pal']))
    print('frame %d = the stored bind pose (no crowd motion found; out\\runtime\\s97\\crowd_pose.md)' % a.frame)
    print('%-3s %-10s %-3s %-10s %-22s %-24s %s' % ('#', 'name', 'par', 'hash', 'local t', 'local euler xyz (rad)', 'quat xyzw / bind world pos'))
    for i, nd in enumerate(nodes):
        q = euler_q(*nd['rot'])
        print('%-3d %-10s %-3d 0x%08X %-22s %-24s (%+.3f %+.3f %+.3f %+.3f) @ (%+.2f %+.2f %+.2f)' % (
            i, nd['name'], nd['parent'], yanm.node_hash(nd['name']), '%+.3f %+.3f %+.3f' % tuple(nd['t']),
            '%+.3f %+.3f %+.3f' % tuple(nd['rot']), *q, *Wb[i][:3, 3]))

    def bb(p):
        return '%s .. %s' % (np.round(p.min(0), 2), np.round(p.max(0), 2))
    print('bind skinned bbox (must equal the stored vertices): %s ; stored: %s' % (bb(skin_positions(body, Wb, Wb)), bb(body['pos'])))
    if a.arms_down:
        over = {}
        for i, nd in enumerate(nodes):
            if nd['name'] in ('r_sakotsu', 'l_sakotsu'):
                sgn = -1.0 if nd['name'][0] == 'r' else 1.0
                over[i] = (nd['rot'][0], nd['rot'][1], sgn * (math.pi - 0.12))  # procedural, not game data
        Wp = world(nodes, over)
        print('procedural arms-down pose, clavicle euler z:', {nodes[i]['name']: round(v[2], 3) for i, v in over.items()})
        print('  posed bbox: %s' % bb(skin_positions(body, Wb, Wp)))
        for i, nd in enumerate(nodes):
            if nd['name'] in ('r_te', 'l_te', 'atama', 'r_kote', 'l_kote'):
                print('  %-8s bind (%+.2f %+.2f %+.2f)  posed (%+.2f %+.2f %+.2f)' % (nd['name'], *Wb[i][:3, 3], *Wp[i][:3, 3]))
    if a.scan:
        scan_hashes(a.arena)
    return 0


if __name__ == '__main__':
    sys.exit(main())
