#!/usr/bin/env python3
"""S102 lane G7: how many wrestler-class vertices each pass of one frame skins (offline, 0 game runs).
  py -I tools\\g7\\g7_census.py [--capture bryan|match] [--frames 40,100,160,220] [--json OUT]
Per pass site (main colour 0x82C0A950 / mirror 0x82C0B94C, shadow map 0x82C04634, rim map 0x82BFBD58, plane mask 0x82BF4D38): draws, vertices, by the stream's
vertex-shader effect name. Reads the capture in place (the main checkout's out\\). Numbers only. Exit 1 when the capture cannot be read or a pass has no draw."""
import argparse, collections, json, os, subprocess, sys
import hashlib
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
import shadow_census as sc  # noqa: E402
CAPS = {'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
        'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'}
SITES = {'main': (sc.SITE_COLOUR, sc.SITE_MIRROR), 'shadow_map': (sc.SITE_MAP,), 'rim': (sc.SITE_RIM,), 'plane_mask': (sc.SITE_PLANE,)}


def main_checkout():
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else ROOT


def census(cap, frames):
    out = {k: dict(draws=0, verts=0, by_vs=collections.Counter()) for k in SITES}
    for fi in frames:
        f = cap.frame(fi); d = f.draws; o = f.origins
        ps = o['pass_site'].astype(np.int64); ob = o['object'].astype(np.int64)
        for name, sites in SITES.items():
            idx = np.nonzero(np.isin(ps, sites) & (ob != 0))[0]
            if name == 'main':  # the wrestlers' colour programs only (yCh_*); the arena and props share the site
                idx = np.array([i for i in idx if cap.eff(int(d['vs_hash'][i])).startswith('yCh_')], dtype=np.int64)
            out[name]['draws'] += len(idx)
            out[name]['verts'] += int(d['vertex_count'][idx].sum())
            for i in idx:
                out[name]['by_vs'][cap.eff(int(d['vs_hash'][i]))] += 1
    return out


def dedup(cap, frames):
    """The shadow-layout skinned draws (shadow maps, rim map, plane mask): vertices skinned a frame today vs once per distinct (vertex bytes, palette rows of the bones used).
    Palette = the draw's own vertex-constant bank (c76 + 3 x bone), exactly what its vertex shader reads. Returns (today, once) per pass, summed over frames."""
    tot = collections.Counter()
    uniq = collections.Counter()
    for fi in frames:
        f = cap.frame(fi); d = f.draws; o = f.origins
        ps = o['pass_site'].astype(np.int64)
        seen = set()
        for name, site in (('shadow_map', sc.SITE_MAP), ('rim', sc.SITE_RIM), ('plane_mask', sc.SITE_PLANE)):
            for i in np.nonzero(ps == site)[0]:
                st, els = sc.layout_of(f, d[i])
                if not any(fm == 106 for fm, _, _ in els):
                    continue  # a rigid layout (props, the ring): one bone, not what this lane skins
                n = int(d['vertex_count'][i]); rows = sc.vertex_rows(cap, d[i], st)
                ids_off = next(off for fm, off, g in els if fm == 41)
                ids = rows[:, ids_off:ids_off + 4].astype(np.int64)
                used = sorted(int(x) for x in np.unique(ids[ids < 21]))
                bank = f.bank(int(d[i]['vs_bank']))
                pal = b''.join(bank[4 * (76 + 3 * b):4 * (76 + 3 * b + 3)].tobytes() for b in used)
                key = (hashlib.md5(np.ascontiguousarray(rows[:, :st]).tobytes()).digest(), pal)
                tot[name] += n
                if key not in seen:
                    uniq[name] += n
                seen.add(key)
    return tot, uniq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default='bryan')
    ap.add_argument('--frames', default='40,100,160,220')
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    z = os.path.join(main_checkout(), CAPS[a.capture])
    if not os.path.exists(z):
        print('FAIL input missing: ' + z)
        return 1
    frames = [int(x) for x in a.frames.split(',')]
    cap = sc.Cap(z)
    r = census(cap, frames)
    n = len(frames)
    bad = 0
    for k, v in r.items():
        print('%-11s draws/frame %7.1f  vertices/frame %9.0f   vs programs: %s' % (k, v['draws'] / n, v['verts'] / n, ', '.join('%s x%d' % (e, c) for e, c in v['by_vs'].most_common(8))))
        bad += v['draws'] == 0
    tot = sum(v['verts'] for v in r.values()) / n
    print('wrestler-class vertices skinned per frame, all passes: %.0f (main %.0f)' % (tot, r['main']['verts'] / n))
    tot, uniq = dedup(cap, frames)
    t, u = sum(tot.values()) / n, sum(uniq.values()) / n
    print('skinned shadow-layout vertices a frame: today %.0f (%s), skinned once %.0f (%s): %.0f fewer (%.0f %%)' % (t, ', '.join('%s %.0f' % (k, v / n) for k, v in tot.items()), u, ', '.join('%s %.0f' % (k, v / n) for k, v in uniq.items()), t - u, 100.0 * (t - u) / max(1.0, t)))
    if a.json:
        json.dump({k: dict(draws=v['draws'], verts=v['verts']) for k, v in r.items()}, open(a.json, 'w'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
