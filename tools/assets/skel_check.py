"""Session 97: numeric checks of the JBOY skeleton (node table) conventions, from the player's own game files.

    py tools\\assets\\skel_check.py [GAME_DIR]        (default: game_data\\extracted)

Reads game files only (via tools\\wm30\\pac.py, which unpacks XCTD pacs into the gitignored out\\wm30\\cache); writes
nothing. Prints the checks listed in out\\runtime\\s97\\skeleton.md. All maths is in FILE space (-Y up); the asset
viewer's loader mirrors Y afterwards (src\\assets\\jboy.h FinishModel).

Convention proven here (and in the guest code sub_82BD8D60 / sub_82BD30E8 / sub_82296630, see skeleton.md):
  local(k)  = [ Rz(rz) * Ry(ry) * Rx(rx) | t ]          column-vector form, standard right-handed rotations
  world(k)  = world(parent) * local(k)                  parent index -1 = root (world = local)
  bind(k)   = world chain of the node table itself      vertices are stored in this (model/bind) space
  skin(k)   = world_pose(k) * inverse(bind(k))          palette value p -> node p-1  (1-based!)
"""
import itertools
import os
import struct
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import pac  # noqa: E402


def rx(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def ry(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rz(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


AX = {'x': rx, 'y': ry, 'z': rz}


def euler(e, order=('z', 'y', 'x'), transpose=False, sign=1):
    m = np.eye(3)
    for ax in order:
        m = m @ AX[ax](sign * e['xyz'.index(ax)])
    return m.T if transpose else m


def parse(blob, order=('z', 'y', 'x'), transpose=False, sign=1):
    """JBOY entry -> nodes, world(bind) matrices, meshes (positions in file space, per-vertex dominant node)."""
    body = blob[8:]

    def be(o):
        return struct.unpack_from('>I', body, o)[0]

    nmesh, mtab, nnode, ntab = be(0x10), be(0x14), be(0x18), be(0x20)
    nodes = []
    for i in range(nnode):
        o = ntab + 80 * i
        nodes.append(dict(
            name=body[o:o + 16].split(b'\0')[0].decode(),
            t=np.array(struct.unpack_from('>3f', body, o + 16)),
            r=np.array(struct.unpack_from('>3f', body, o + 32)),
            par=struct.unpack_from('>i', body, o + 48)[0],
            sph=np.array(struct.unpack_from('>4f', body, o + 64))))
    world = []
    for n in nodes:
        m = np.eye(4)
        m[:3, :3] = euler(n['r'], order, transpose, sign)
        m[:3, 3] = n['t']
        world.append(m if n['par'] < 0 else world[n['par']] @ m)
    meshes = []
    for mi in range(nmesh):
        rec = mtab + mi * 0xB8
        n, npal = be(rec), be(rec + 8)
        pal = [be(rec + 0x0C + 4 * k) for k in range(npal)]
        s0, s1, s2 = be(rec + 0x68), be(rec + 0x6C), be(rec + 0x70)
        st1 = (s2 - s1) // n
        raw = np.frombuffer(body[s0 + 4:s0 + 4 + 28 * n], dtype=np.uint8).reshape(n, 28)
        pos = np.frombuffer(raw[:, :12].tobytes(), dtype='>f4').reshape(n, 3).astype(float)
        dom = np.full(n, -1)
        for v in range(n):
            bw = -1.0
            for pr in range(st1 // 8):
                q = s1 + st1 * v + 8 * pr
                slot = struct.unpack_from('<I', body, q)[0]
                w = struct.unpack_from('>f', body, q + 4)[0]
                if slot < npal and w > bw:
                    bw, dom[v] = w, pal[slot]
        meshes.append(dict(n=n, pal=pal, pos=pos, dom_pal=dom,
                           mat=body[rec + 0x7C:rec + 0x8C].split(b'\0')[0].decode()))
    return nodes, world, meshes


def entry(game, pacname, key):
    d = pac.load(os.path.join(game, 'pac', pacname))
    return dict(pac.entries(d, 8))[key]


def kane_checks(game):
    print('== 1. Kane (ch107 p0/0): node convention vs the mesh ==')
    blob = entry(game, 'ch/ch107.pac', 'p0/0')
    best = []
    for order in itertools.permutations('xyz'):
        for tr in (False, True):
            for sg in (1, -1):
                nodes, world, meshes = parse(blob, order, tr, sg)
                v = np.concatenate([m['pos'] for m in meshes])
                b = np.concatenate([m['dom_pal'] for m in meshes]) - 1  # palette is 1-based
                ds = []
                for k in range(len(nodes)):
                    msk = b == k
                    if msk.sum() >= 30:
                        ds.append(np.linalg.norm(v[msk] - world[k][:3, 3], axis=1).min())
                best.append((float(np.mean(ds)), ''.join(order), tr, sg))
    best.sort()
    print('  mean over bones (>=30 dominant verts) of  min |vertex - joint|, best 4 / worst of %d variants:' % len(best))
    for r in best[:4]:
        print('    %.2f  order(left->right)=%s  transpose=%s  angle sign=%+d' % r)
    print('    %.2f  (worst)' % best[-1][0])
    print('  -> winner: R = Rz*Ry*Rx (zyx, no transpose, sign +1); ties with xyz+transpose are the same matrix.')

    nodes, world, meshes = parse(blob)
    v = np.concatenate([m['pos'] for m in meshes])
    dom = np.concatenate([m['dom_pal'] for m in meshes])
    for off, label in ((1, 'palette value - 1 (game: sub_82BD7630 uses (p-1)*64)'), (0, 'palette value as node index')):
        ds = []
        for k in range(len(nodes)):
            msk = (dom - off) == k
            if msk.sum() >= 30:
                ds.append(np.linalg.norm(v[msk] - world[k][:3, 3], axis=1).min())
        print('  node = %-58s mean min-dist %.2f over %d bones' % (label, np.mean(ds), len(ds)))
    print('  max palette value over all meshes = %d, node count = %d (so p-1 stays inside the table)' % (
        max(max(m['pal']) for m in meshes), len(nodes)))
    print('  sample joints (file space, -Y up):')
    for name in ('Bip', 'Bip Head', 'Bip L Hand', 'Bip R Hand', 'Bip L Foot', 'Bip R Foot'):
        k = [i for i, n in enumerate(nodes) if n['name'] == name][0]
        msk = (dom - 1) == k
        c = v[msk].mean(0) if msk.any() else None
        print('    %-11s joint %s  centroid of its verts %s  sphere(+64) %s' % (
            name, np.round(world[k][:3, 3], 1), None if c is None else np.round(c, 1), np.round(nodes[k]['sph'], 1)))
    # the last float4 is a bounding sphere of the bone's vertices, not a joint
    ok = tot = 0
    for k in range(len(nodes)):
        msk = (dom - 1) == k
        if msk.sum() >= 30 and nodes[k]['sph'][3] > 0:
            tot += 1
            d = np.linalg.norm(v[msk] - nodes[k]['sph'][:3], axis=1)
            ok += int(np.percentile(d, 95) <= nodes[k]['sph'][3] * 1.25)
    print('  +64 float4 = bind-space bounding sphere (centre xyz, radius w): 95%% of dominant verts inside r*1.25 for %d/%d bones' % (ok, tot))
    # pose test: skin matrix of a rotated bone moves its vertices, bind pose is the identity
    k = [i for i, n in enumerate(nodes) if n['name'] == 'Bip L Forearm'][0]
    inv = np.linalg.inv(world[k])
    print('  skin(k) at the bind pose = world*inv(world): max |M - I| = %.2e' % np.abs(world[k] @ inv - np.eye(4)).max())


def ring_checks(game, arena='bg/bg05.pac'):
    print('\n== 3. Ring set of %s ==' % arena)
    d = pac.load(os.path.join(game, 'pac', arena))
    E = dict(pac.entries(d, 8))
    cn, cw, cm = parse(E['p0/953'])
    corner_idx = {'bone03': (-1, -1), 'bone05': (-1, 1), 'bone07': (1, 1), 'bone09': (1, -1)}
    cor = {nm: cw[[i for i, n in enumerate(cn) if n['name'] == nm][0]][:3, 3] for nm in corner_idx}
    cv = cm[0]['pos']
    print('  ring canvas 953: mesh x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f' % (
        cv[:, 0].min(), cv[:, 0].max(), cv[:, 1].min(), cv[:, 1].max(), cv[:, 2].min(), cv[:, 2].max()))
    for nm, p in cor.items():
        print('   canvas corner bone %s world %s' % (nm, np.round(p, 1)))
    pm = {}
    for key in ('951', '981', '952', '964', '982', '956', '960', '980', '973', '974'):
        nodes, world, meshes = parse(E['p0/' + key])
        allp = np.concatenate([m['pos'] for m in meshes])
        pm[key] = (nodes, world, meshes, allp)
        names = [n['name'] for n in nodes]
        print('  %s  nodes=%d  vertex bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f  palettes are 1-based: max pal %d' % (
            key, len(nodes), allp[:, 0].min(), allp[:, 0].max(), allp[:, 1].min(), allp[:, 1].max(), allp[:, 2].min(),
            allp[:, 2].max(), max(max(m['pal']) for m in meshes)))
    # post: bone vs canvas corner, vertices vs bone
    nodes, world, meshes, allp = pm['951']
    kb = [i for i, n in enumerate(nodes) if n['name'] == 'bone01'][0]
    pb = world[kb][:3, 3]
    print('  post 951 bone01 world %s  == canvas corner bone03 %s : %s' % (
        np.round(pb, 1), np.round(cor['bone03'], 1), np.allclose(pb, cor['bone03'])))
    c = allp.mean(0)
    print('  post vertex centroid x,z = %.1f, %.1f (post axis at the bone x,z); y runs %.0f..%.0f (0 = floor, -Y up)' % (
        c[0], c[2], allp[:, 1].max(), allp[:, 1].min()))
    print('  -> vertices are in BIND space (already at the -x,-z corner); no bone-local offset to add.')
    # four corners: game sub_825CCFE0 builds RotY(i * f) into the model matrix, f = float @0x82000ADC (pi/2 if the corners land)
    print('  corner rule: model matrix = RotY(i*pi/2), i=0..3 (sub_825CCFE0); post axis (x,z) maps to:')
    axis = np.array([pb[0], pb[1], pb[2]])
    for i in range(4):
        p = ry(i * np.pi / 2) @ axis
        hit = [nm for nm, q in cor.items() if abs(q[0] - p[0]) < 1 and abs(q[2] - p[2]) < 1]
        print('     i=%d  (%.1f, %.1f)  canvas corner bone: %s' % (i, p[0], p[2], hit))
    # ropes
    nodes, world, meshes, allp = pm['960']
    xs = [world[i][0, 3] for i in range(len(nodes)) if nodes[i]['name'].startswith('bone')]
    print('  rope 960: bones along x %.1f..%.1f (pitch %.2f), vertices x %.1f..%.1f, y/z within +-%.1f -> one rope centred on the origin' % (
        min(xs), max(xs), np.diff(sorted(xs)).mean(), allp[:, 0].min(), allp[:, 0].max(), max(abs(allp[:, 1]).max(), abs(allp[:, 2]).max())))
    nodes, world, meshes, allp = pm['952']
    print('  turnbuckle 952: bones %s ; vertex bounds x %.1f..%.1f z %.1f..%.1f (sits on the -z side at the -x corner, y 0 = rope height origin)' % (
        [(n['name'], np.round(world[i][:3, 3], 1).tolist()) for i, n in enumerate(nodes) if n['name'].startswith('bone')],
        allp[:, 0].min(), allp[:, 0].max(), allp[:, 2].min(), allp[:, 2].max()))
    nodes, world, meshes, allp = pm['964']
    print('  cover 964: vertex bounds x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f (at the -x,-z corner, bone49 %s)' % (
        allp[:, 0].min(), allp[:, 0].max(), allp[:, 1].min(), allp[:, 1].max(), allp[:, 2].min(), allp[:, 2].max(),
        np.round(world[2][:3, 3], 1)))
    nodes, world, meshes, allp = pm['973']
    print('  apron 973 (=974 in size): vertex bounds x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f -> one side, hanging from the canvas edge (y -120) to the floor (y -1)' % (
        allp[:, 0].min(), allp[:, 0].max(), allp[:, 1].min(), allp[:, 1].max(), allp[:, 2].min(), allp[:, 2].max()))
    a = parse(E['p0/973'])[2]
    b = parse(E['p0/974'])[2]
    same = all(np.array_equal(x['pos'], y['pos']) for x, y in zip(a, b))
    print('  973 vs 974 vertex positions identical: %s' % same)

    # ---- placement check (follow-up): provisional rule, side s = 0 is the -z side, s*pi/2 about Y ----
    print('  placement check (rule: side s: RotY(s*pi/2) * T(0, h, -320); post/pad corner i: RotY(i*pi/2))')
    rope = pm['960'][3]
    tb = pm['952'][3]
    pad = tb[(tb[:, 0] > -325) & (tb[:, 0] < -283)]
    rz = lambda a: a
    for end, rx_ in (('-x', rope[:, 0].min()), ('+x', rope[:, 0].max())):
        inside = (tb[:, 0].min() <= rx_ <= tb[:, 0].max())
        print('   rope %s end x=%.1f inside turnbuckle x-range %.1f..%.1f: %s' % (end, rx_, tb[:, 0].min(), tb[:, 0].max(), inside))
    print('   rope z=0 -> -320 lies inside pad z-range %.1f..%.1f: %s ; rope y +-%.1f inside pad y +-%.1f (same height offset h for both)' % (
        tb[:, 2].min(), tb[:, 2].max(), tb[:, 2].min() <= -320 <= tb[:, 2].max(), abs(rope[:, 1]).max(), abs(tb[:, 1]).max()))
    print('   turnbuckle pad per corner i (RotY(i*pi/2) of the authored one): side/end it lands on:')
    for i in range(4):
        c = ry(i * np.pi / 2) @ np.array([-300.0, 0, -320.0])
        print('     i=%d  pad centre (%.0f, %.0f)' % (i, c[0], c[2]))
    print('   -> one pad per corner on ONE end of each side (the other end of the rope has no 2nd pad in the data: mirrored copy or none) [U]')
    sk = pm['973'][3]
    for s in range(4):
        v = sk @ ry(s * np.pi / 2).T + (ry(s * np.pi / 2) @ np.array([0, 0, -320.0]))
        print('   skirt side %d: x %.1f..%.1f z %.1f..%.1f (canvas edge +-318.8)' % (s, v[:, 0].min(), v[:, 0].max(), v[:, 2].min(), v[:, 2].max()))
    print('   rope HEIGHTS h_j, cover offsets, mirrored pad: NOT in the pac (ring-context floats, runtime) [U]')


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'game_data', 'extracted')
    kane_checks(game)
    ring_checks(game)
    return 0


if __name__ == '__main__':
    sys.exit(main())
