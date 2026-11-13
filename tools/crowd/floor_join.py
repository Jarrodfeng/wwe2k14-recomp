#!/usr/bin/env python3
"""Lane 82 check A (offline, 0 game runs): the seated floor people (draw site 0x8276ED60) joined to the player's own seat text, on Check B's capture.

  py -I tools\\crowd\\floor_join.py [--capture DIR_OR_ZIP] [--game DIR] [--arena 4] [--frames A-B] [--emit FILE] [--plant NAME] [--selftest]

What it proves (docs\\renderer\\CROWD_FAMILY_8276ED60.md sections 2.2 and 4A; numbers only, nothing from the game is written except the --emit sidecar, which goes under out\\):
  1. the capture has draw origins (a version-1 capture, or a frame without them, FAILS loudly: the site cannot be found without them)
  2. every draw at 0x8276ED60 joins to exactly ONE seat of pac entry 21100 (c76.w / c78.w against (x0 + dx k, z0 + dz k), xz distance <= 0.01)
  3. no seat is drawn twice in a frame (a twin draw / a doubled member would show here)
  4. the draw's vertex count equals the joined type's LOD 0 body (pac entry 20000 + type), all 20 types
  5. slot 0 is ONE image per type over the whole run, 20 types -> 20 distinct images (the "several outfits" class: one picture for every member would collapse this)
  6. the vertex / pixel program hashes are the stands block draws' (0x82518574) hashes: ONE pair, equal to the block site's pair
  7. c77.w (the world y) is -10.0 in every draw (the phase-4 lift of 10 units): a world taken from the file (y 0, quantised yaw) is NOT the game's
Planted twins (`--plant NAME`, each must FAIL its named check; --selftest runs them all): type_shift (4), lod1 (4), world_file (7), first_texture (5), dup (3), no_origins (1).
--emit FILE writes the floor members for fview --crowd-floor-synth (crowd_floor_synth.h): `frame type index w0..w11` (the 12 floats of c76-c78 as hex bits).
A check that cannot read its input fails loudly (exit 1)."""
import argparse
import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path[:0] = [os.path.join(REPO, 'tools', 'solver'), os.path.join(REPO, 'tools', 'assets'), os.path.join(REPO, 'tools', 'wm30')]

FLOOR_SITE = 0x8276ED60
BLOCK_SITE = 0x82518574
TOL = 0.01  # xz distance of a join (float32 positions against the file's decimal positions)


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


def seats_and_bodies(game, arena):
    import numpy as np
    import crowd
    import pac
    data = pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena))
    leaves = dict(pac.entries(data, 2))
    text = leaves['p0/20000/21100'].decode('cp932', 'replace')
    rows, people = crowd.parse_crowd(text)
    if not people:
        raise SystemExit('FAIL  no people in pac entry 21100 of arena %d' % arena)
    nth = collections.Counter()
    seats = []  # (x, z, type, index within the type, yaw step)
    for p in people:
        x, _, z, q, _ = crowd.place(rows, p)
        seats.append((x, z, p['type'], nth[p['type']], q))
        nth[p['type']] += 1
    lod0 = {t: len(crowd.jboy_vertices(leaves['p0/20000/%d' % (20000 + t)])[0]) for t in range(20)}
    lod1 = {t: len(crowd.jboy_vertices(leaves['p0/20000/%d' % (20040 + t)])[0]) for t in range(20)}
    return np.array([(s[0], s[1]) for s in seats]), seats, lod0, lod1


def open_capture(arg, main_co):
    from capread import Source
    if arg:
        return Source(arg)
    folder = os.path.join(main_co, 'out', 's101', 'crowd', 'case_brock', 'capture')
    if os.path.isdir(os.path.join(folder, 'frames')):
        return Source(folder)
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        raise SystemExit('FAIL  input missing: Check B\'s capture (%s) or case zip (%s)' % (folder, runs))
    return Source(zips[0])


def analyse(src, seats_xz, seats, lod0, lod1, frames, plant, emit):
    """-> dict of counts and the check verdicts."""
    from capread import Frame
    import numpy as np
    R = collections.Counter()
    images_of_type = collections.defaultdict(set)
    floor_hashes, block_hashes = set(), set()
    ys, bad_lines = [], []
    emitted = []
    for fi in frames:
        f = Frame(src.read(src.frames[fi]))
        R['frames'] += 1
        if plant == 'no_origins':
            f.has_origins = False
        if not f.has_origins:
            R['no_origins_frames'] += 1
            continue
        origins = f.origins
        site = origins['call_site']
        idx = np.nonzero(site == FLOOR_SITE)[0]
        for d in np.nonzero(site == BLOCK_SITE)[0]:
            block_hashes.add((int(f.draws[d]['vs_hash']), int(f.draws[d]['ps_hash'])))
        if plant == 'dup' and len(idx):
            idx = np.concatenate([idx, idx[:1]])
        first_image = None
        seen_seat = collections.Counter()
        for d in idx:
            dr = f.draws[d]
            c = f.reg(int(dr['vs_bank']), 76, 3)  # rows: c76 c77 c78 = the 12 floats
            x, y, z = float(c[0, 3]), float(c[1, 3]), float(c[2, 3])
            dist = np.hypot(seats_xz[:, 0] - x, seats_xz[:, 1] - z)
            near = np.nonzero(dist <= TOL)[0]
            R['draws'] += 1
            R['joined_one'] += len(near) == 1
            R['joined_none'] += len(near) == 0
            R['joined_many'] += len(near) > 1
            if len(near) != 1:
                continue
            s = seats[int(near[0])]
            typ = s[2]
            if plant == 'type_shift':
                typ = (typ + 1) % 20
            R['max_dist_e6'] = max(R['max_dist_e6'], int(dist[near[0]] * 1e6))
            seen_seat[int(near[0])] += 1
            want = lod1[typ] if plant == 'lod1' else lod0[typ]
            R['verts_ok'] += int(dr['vertex_count']) == want
            # slot 0 image
            img = None
            for k in range(int(dr['tex_count'])):
                sl = f.slots[int(dr['tex_first']) + k]
                if int(sl['slot']) == 0:
                    img = int(sl['image'])
            if plant == 'first_texture':
                if first_image is None:
                    first_image = img
                img = first_image
            images_of_type[typ].add(img)
            floor_hashes.add((int(dr['vs_hash']), int(dr['ps_hash'])))
            if plant == 'world_file':
                y = 0.0  # the file's world: y 0
            ys.append(y)
            R['y_lift'] += y == -10.0
            if emit is not None:
                emitted.append((fi, s[2], s[3], c.astype('<f4').reshape(-1).view('<u4')))
        R['seat_twice'] += sum(1 for v in seen_seat.values() if v > 1)
    return R, images_of_type, floor_hashes, block_hashes, emitted


def verdicts(R, images_of_type, floor_hashes, block_hashes, expect_draws=None):
    out = []

    def add(name, ok, text):
        out.append((name, ok, text))
    add('origins', R['no_origins_frames'] == 0 and R['frames'] > 0, '%d frames read, %d without draw origins (the capture must be format version 2+)' % (R['frames'], R['no_origins_frames']))
    n = R['draws']
    add('join', n > 0 and R['joined_one'] == n, '%d floor draws at 0x%08X: %d joined exactly one seat of pac entry 21100 (none %d, several %d), max xz distance %.6f' % (n, FLOOR_SITE, R['joined_one'], R['joined_none'], R['joined_many'], R['max_dist_e6'] / 1e6))
    add('no_twins', R['seat_twice'] == 0, '%d seats drawn twice in one frame' % R['seat_twice'])
    add('vertices', n > 0 and R['verts_ok'] == n, '%d of %d draws have the joined type\'s LOD 0 vertex count' % (R['verts_ok'], n))
    types = len(images_of_type)
    one_each = all(len(v) == 1 and None not in v for v in images_of_type.values())
    distinct = len({next(iter(v)) for v in images_of_type.values() if len(v) == 1}) == types
    add('textures', types > 0 and one_each and distinct, 'slot 0: %d person types seen, one image each: %s, %d distinct images' % (types, one_each, len({i for v in images_of_type.values() for i in v})))
    add('programs', len(floor_hashes) == 1 and len(block_hashes) >= 1 and floor_hashes <= block_hashes, 'program hashes: %d floor pair(s), %d block pair(s), the floor pair is a block pair: %s' % (len(floor_hashes), len(block_hashes), floor_hashes <= block_hashes))
    add('world_y', n > 0 and R['y_lift'] == n, 'c77.w == -10.0 in %d of %d draws (the game\'s world, not the file\'s y 0)' % (R['y_lift'], n))
    return out


PLANTS = {  # plant -> the check that must fail
    'type_shift': 'vertices', 'lod1': 'vertices', 'world_file': 'world_y', 'first_texture': 'textures', 'dup': 'no_twins', 'no_origins': 'origins',
}


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    a, _, b = spec.partition('-')
    return list(range(int(a), min(int(b or a), total - 1) + 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture')
    ap.add_argument('--game')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--frames', default='')
    ap.add_argument('--emit')
    ap.add_argument('--plant', default='')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    try:
        import numpy  # noqa: F401
    except Exception as e:
        print('FAIL  numpy is needed: %s' % e)
        return 1
    main_co = main_checkout()
    game = args.game or os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    src = open_capture(args.capture, main_co)
    seats_xz, seats, lod0, lod1 = seats_and_bodies(game, args.arena)
    frames = parse_frames(args.frames, len(src.frames))
    if not frames:
        print('FAIL  no frames selected')
        return 1
    runs = [args.plant] if not args.selftest else [''] + list(PLANTS)
    fails = 0
    for plant in runs:
        emit = [] if (args.emit and not plant) else None
        R, imgs, fh, bh, emitted = analyse(src, seats_xz, seats, lod0, lod1, frames, plant, emit)
        res = verdicts(R, imgs, fh, bh)
        if plant:
            want = PLANTS.get(plant)
            if want is None:
                print('FAIL  unknown plant ' + plant)
                return 1
            failed = [n for n, ok, _ in res if not ok]
            ok = want in failed
            print('%s  plant %s: check "%s" %s (failed checks: %s)' % ('PASS' if ok else 'FAIL', plant, want, 'FAILED as it must' if ok else 'did NOT fail', ', '.join(failed) or 'none'))
            fails += not ok
            continue
        for n, ok, text in res:
            print('%s  %s: %s' % ('PASS' if ok else 'FAIL', n, text))
            fails += not ok
        if emit is not None:
            os.makedirs(os.path.dirname(os.path.abspath(args.emit)), exist_ok=True)
            per_frame = collections.Counter()
            with open(args.emit, 'w') as fh_out:
                fh_out.write('# lane 82 floor members: frame type index w0..w11 (c76-c78 bits); made by tools/crowd/floor_join.py from arena %d\n' % args.arena)
                for fi, typ, idx, words in emitted:
                    fh_out.write('%d %d %d %s\n' % (fi, typ, idx, ' '.join('%08x' % int(w) for w in words)))
                    per_frame[fi] += 1
            print('emit: %d floor members in %d frames -> %s' % (len(emitted), len(per_frame), args.emit))
    print('floor_join: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
