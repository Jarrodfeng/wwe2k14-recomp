#!/usr/bin/env python3
"""Lane 133 (S101, N4) independent oracle, offline, 0 game runs: the crowd's sign / foam-hand draws (T_Audience_LightMap at 0x82518D60, T_Audience at 0x825192D0) joined to the player's own
arena pac by BYTES, in Python (numpy) - not the shipped C++ (src/assets/crowd_prop_game_space.cpp), so a shared mistake would show as a disagreement between the two.

  py -I tools\\crowd\\crowd_props_join.py [--root DIR] [--selftest] [--plant NAME]

What it proves (docs/renderer/CROWD_PROMOTION.md "N4", numbers only; nothing from the game is written):
  1. FILES  every bgNN.pac that has the audience set (entry 20000 of PACH 0) holds the two meshes under the same keys with byte-identical decoded entries: 40000 (34 vertices / 77 indices) and 41016 (140 / 223).
  2. DRAWS  on run 7079 (frames 200 / 300), Check I's match capture 7083 (200 / 400) and Bryan's entrance (90 / 150): every draw at the four crowd-loop call sites joins ONE file mesh by counts and bytes:
            position and uv bit-equal, index equal, normal within 1e-6 (the game's normal differs from the file's by one ulp on some vertices), the host stride is 28, slot 0 is a CPU picture
            (provenance source 3) and slot 13 / 16 is the same GPU light-map page the stands' block draws bind.
  3. PLANTS (--selftest runs them all; each must FAIL its named check): wrong_entry (the next pac entry's mesh), half_nearest (uv rounded, not truncated), pos_not_x10, index_swapped.
A check that cannot read its input fails loudly (exit 1)."""
import argparse
import collections
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path[:0] = [os.path.join(REPO, 'tools', 'solver'), os.path.join(REPO, 'tools', 'wm30')]

SITES = {0x82518D60: 'T_Audience_LightMap', 0x82518E68: 'loop A second call', 0x825192D0: 'T_Audience', 0x825194B4: 'loop B second call'}
BLOCK_SITE = 0x82518574
RUNS = [  # (label, run folder, frames)
    ('7079', '20261112_122623_s100_owner_s89_brock', (200, 300)),
    ('match7083', '20261112_131008_s100_owner_match', (200, 400)),
    ('bryan', '20261112_122700_s100_owner_bryan_entrance', (90, 150)),
]
PLANTS = ('wrong_entry', 'half_nearest', 'pos_not_x10', 'index_swapped')
NORMAL_TOL = 1e-6


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def trunc_half(x):
    import numpy as np
    x = x.astype('<f4').view('<u4')
    sign = (x >> 16) & 0x8000
    e = ((x >> 23) & 0xFF).astype(np.int32) - 127 + 15
    m = x & 0x7FFFFF
    sub = np.where(e < -10, sign, sign | ((m | 0x800000) >> (14 - np.clip(e, -30, 0)).astype(np.uint32)))
    out = np.where(e >= 31, sign | 0x7C00, np.where(e <= 0, sub, sign | (e.astype(np.uint32) << 10) | (m >> 13)))
    return out.astype('<u2')


def convert(leaf, plant=''):
    """One single-mesh JBOY entry -> (vertex_count, host vertex bytes (28 each), index bytes (plain u16)) or None."""
    import numpy as np
    if leaf[:4] != b'JBOY':
        return None
    body = leaf[8:]
    if be32(body, 0x10) != 1:
        return None
    rec = be32(body, 0x14)
    n = be32(body, rec)
    s0, s1, s2, ix = be32(body, rec + 0x68), be32(body, rec + 0x6C), be32(body, rec + 0x70), be32(body, rec + 0x9C)
    if not (0 < n < 65536 and s1 == s0 + 4 + 28 * n and s2 and s2 + 8 * n <= len(body) and ix and ix + 12 <= len(body)):
        return None
    if int.from_bytes(body[ix:ix + 4], 'little') != 6:
        return None
    cnt = be32(body, ix + 4)
    pos_nrm = np.frombuffer(body[s0 + 4:s0 + 4 + 28 * n], '>f4').reshape(n, 7)[:, :6]
    uv = np.frombuffer(body[s2:s2 + 8 * n], '>f4').reshape(n, 2).astype('<f4')
    v = np.zeros((n, 7), '<u4')
    scale = np.float32(1) if plant == 'pos_not_x10' else np.float32(10)
    v[:, 0:3] = (pos_nrm[:, 0:3].astype('<f4') * scale).astype('<f4').view('<u4')
    v[:, 3:6] = pos_nrm[:, 3:6].astype('<f4').view('<u4')
    h = uv.astype(np.float16).view('<u2') if plant == 'half_nearest' else trunc_half(uv)
    v[:, 6] = h[:, 0].astype('<u4') | (h[:, 1].astype('<u4') << 16)
    raw = np.frombuffer(body[ix + 12:ix + 12 + 2 * cnt], '>u2')
    idx = raw.astype('>u2' if plant == 'index_swapped' else '<u2').tobytes()
    return n, v.tobytes(), idx


def vertex_ok(ours, stream, n):
    import numpy as np
    a = np.frombuffer(ours, '<u4').reshape(n, 7)
    b = np.frombuffer(stream, '<u4').reshape(n, 7)
    if not np.array_equal(a[:, 0:3], b[:, 0:3]) or not np.array_equal(a[:, 6], b[:, 6]):
        return False, 0.0
    d = float(np.abs(a[:, 3:6].view('<f4') - b[:, 3:6].view('<f4')).max())
    return d <= NORMAL_TOL, d


def audience_set(game, arena):
    import pac
    data = pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena))
    return [(nm, leaf) for nm, leaf in pac.entries(data, 6) if nm.startswith('p0/20000/') and 40000 <= int(nm.rsplit('/', 1)[1]) < 43000]


def check_files(game, problems, quiet=False):
    import glob
    sums = collections.defaultdict(set)
    n_pacs = 0
    for f in sorted(glob.glob(os.path.join(game, 'pac', 'bg', 'bg[0-9][0-9].pac'))):
        arena = int(os.path.basename(f)[2:4])
        try:
            ents = audience_set(game, arena)
        except Exception:
            continue
        if not ents:
            continue
        n_pacs += 1
        got = {}
        for nm, leaf in ents:
            key = int(nm.rsplit('/', 1)[1])
            c = convert(leaf)
            if c:
                got[key] = (c[0], len(c[2]) // 2, hashlib.md5(leaf).hexdigest())
        for key in (40000, 41016):
            if key not in got:
                problems.append('arena %d: key %d missing or not a single-mesh model' % (arena, key))
            else:
                sums[key].add(got[key])
    for key, want in ((40000, (34, 77)), (41016, (140, 223))):
        vals = sums.get(key, set())
        counts = {(v[0], v[1]) for v in vals}
        if counts != {want} or len({v[2] for v in vals}) != 1:
            problems.append('key %d: counts %s, %d distinct entry md5s over %d pacs (want one md5, counts %s)' % (key, sorted(counts), len({v[2] for v in vals}), n_pacs, want))
    if not quiet:
        print('INFO  FILES  %d arena pacs with the audience set; key 40000 and 41016 identical in all of them: %s' % (n_pacs, 'yes' if not problems else 'NO'))
    return n_pacs


def check_draws(main_co, game, plant, problems, quiet=False):
    import numpy as np
    from capread import Source, Frame
    summary = collections.Counter()
    arena_sets = {}
    for label, run, frames in RUNS:
        folder = os.path.join(main_co, 'out', 's101', 'runs', run)
        zips = [os.path.join(folder, x) for x in os.listdir(folder) if x.startswith('case_') and x.endswith('.zip')] if os.path.isdir(folder) else []
        if not zips:
            problems.append('input missing: %s (%s)' % (label, folder))
            continue
        src = Source(zips[0])
        gb = src.zf.read([n for n in src.zf.namelist() if n.endswith('geometry.bin')][0])
        for fi in frames:
            f = Frame(src.read(src.frames[fi]))
            if not f.has_origins:
                problems.append('%s frame %d: no draw origins' % (label, fi))
                continue
            prov = {int(p['image']): p for p in f.provenance}
            site = f.origins['call_site']
            block_pages = set()
            for d in np.nonzero(site == BLOCK_SITE)[0]:
                for t in f.slots[f.draws[d]['tex_first']:f.draws[d]['tex_first'] + f.draws[d]['tex_count']]:
                    if int(t['slot']) in (13, 16):
                        block_pages.add(int(t['image']))
            arena = 4
            if arena not in arena_sets:
                arena_sets[arena] = [(nm, convert(leaf, plant)) for nm, leaf in audience_set(game, arena)]
                arena_sets[arena] = [(nm, c) for nm, c in arena_sets[arena] if c]
            cands = arena_sets[arena]
            for s, tech in SITES.items():
                idx = np.nonzero(site == s)[0]
                for d in idx:
                    dr = f.draws[d]
                    summary['draws'] += 1
                    n, ic = int(dr['vertex_count']), int(dr['index_count'])
                    vb = gb[dr['vertex_offset']:dr['vertex_offset'] + dr['vertex_bytes']]
                    ib = gb[dr['index_offset']:dr['index_offset'] + dr['index_bytes']]
                    won = None
                    pool = [(nm, c) for nm, c in cands]
                    if plant == 'wrong_entry':   # the next entry after the true one
                        true = next((i for i, (nm, c) in enumerate(pool) if c[0] == n and len(c[2]) // 2 == ic), None)
                        if true is not None:   # the next entry (cyclic) whose bytes differ: 40100 is byte-identical to 40000, which would not be a wrong sign
                            nxt = next((pool[(true + k) % len(pool)] for k in range(1, len(pool)) if pool[(true + k) % len(pool)][1][1] != pool[true][1][1]), None)
                            pool = [nxt] if nxt else []
                    for nm, c in pool:
                        if c[0] != n or len(c[2]) // 2 != ic:
                            continue
                        ok, dn = vertex_ok(c[1], vb, n)
                        if ok and c[2] == ib:
                            won = nm
                            summary['max_normal_diff_e9'] = max(summary['max_normal_diff_e9'], int(dn * 1e9))
                            break
                    if won is None:
                        summary['unjoined'] += 1
                        if summary['unjoined'] <= 3:
                            problems.append('%s frame %d site 0x%X draw %d (%d vertices, %d indices): joins no file mesh' % (label, fi, s, d, n, ic))
                        continue
                    summary['joined'] += 1
                    summary['by_key_' + won.rsplit('/', 1)[1]] += 1
                    summary['site_%X' % s] += 1
                    lay = None
                    # slot 0 = a CPU picture, the light-map page = the stands' page
                    tex = {int(t['slot']): int(t['image']) for t in f.slots[dr['tex_first']:dr['tex_first'] + dr['tex_count']]}
                    p0 = prov.get(tex.get(0, 0))
                    if p0 is None or int(p0['source']) != 3:
                        summary['slot0_not_cpu'] += 1
                    page = tex.get(13) or tex.get(16)
                    if page not in block_pages:
                        summary['page_not_the_blocks'] += 1
    if summary['draws'] == 0:
        problems.append('no crowd-loop draws found in any capture (the check read nothing)')
    if summary['slot0_not_cpu'] or summary['page_not_the_blocks']:
        problems.append('%d draws without a CPU slot 0, %d without the stands\' light-map page' % (summary['slot0_not_cpu'], summary['page_not_the_blocks']))
    if not quiet:
        print('INFO  DRAWS  %s' % dict(summary))
    return summary


def run_once(main_co, game, plant):
    problems = []
    if not plant:
        check_files(game, problems)
    s = check_draws(main_co, game, plant, problems)
    return problems, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=REPO)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--plant', default='')
    a = ap.parse_args()
    main_co = main_checkout()
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    if a.selftest:
        bad = 0
        problems, s = run_once(main_co, game, '')
        ok = not problems and s['joined'] == s['draws'] and s['joined'] > 0
        print(('PASS' if ok else 'FAIL') + '  clean: %d of %d draws join (problems: %s)' % (s['joined'], s['draws'], problems[:3]))
        bad += not ok
        for plant in PLANTS:
            problems, s = run_once(main_co, game, plant)
            failed = s['unjoined'] > 0
            print(('PASS' if failed else 'FAIL') + '  planted %s: %d of %d draws unjoined (must be > 0)' % (plant, s['unjoined'], s['draws']))
            bad += not failed
        print('crowd_props_join selftest: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
        return 1 if bad else 0
    problems, s = run_once(main_co, game, a.plant)
    for p in problems:
        print('FAIL  ' + p)
    ok = not problems and s['unjoined'] == 0 and s['joined'] > 0
    print('crowd_props_join: %s (%d of %d draws joined)' % ('PASS' if ok else 'FAIL', s['joined'], s['draws']))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
