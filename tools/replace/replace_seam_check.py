#!/usr/bin/env python3
"""Lane 41 acceptance test (offline, 0 game runs): the generic in-place replace seam (src\\renderer_adapter\\draw_replace_hook.h, binder.h AddReplaceHook),
proven with the TEST hook (src\\renderer_adapter\\replace_test_hook.h; no arena or wrestler class code) through the REAL binder path (fview --replace-test).

  py -I tools\\replace\\replace_seam_check.py [--root DIR] [--frame 200] [--no-build]

Material: Check B's run 7051 case zip (MAIN checkout out\\s101\\runs\\20261111_215201_s100_owner_s89_brock\\case_*.zip; carries capture\\scene\\) and the game files (the
crowd arms). A missing input FAILS (never skips). One fview process per arm, one frame picture each (frame 200, the crowd lane's frame).
Bar:
  1. switch off: the stream arm's picture md5 equals the crowd lane's pinned BASE_STREAM_MD5 (read from tools\\crowd\\crowd_ours_check.py: the picture the base draws with every switch
     off, so the seam added nothing to the off path). GPU / driver bound exactly like the crowd's pin.
  2. transparent: test hook replaces every claimed draw with its own stream geometry -> picture md5 == stream; replaced > 1000, kept 0, fallback 0. Also with 'all' (every named draw class).
  3. counts: replaced + kept == the hook's own tally of draws it claimed (an independent count), in every arm.
  4. refusals keep the stream draw: refuse_odd (every second draw refused), not_ready (the frame not ready), no_vertex (said yes, gave nothing): picture == stream, kept == claims
     (not_ready / no_vertex) or both replaced > 0 and kept > 0 (refuse_odd), and the census names the reason.
  5. the row override: 'rows' (the stream's own first constant range rewritten through the override) is transparent; 'rows_off_by_one' (the same rows one register late) must CHANGE the
     picture. And the unit test (out\\tools\\replace_seam_selftest.exe, the real effect tables): rows named change, rows not named stay, planted late / early / one-too-many /
     not-applied all caught.
  6. coexists with the crowd: --crowd-ours --replace-test gives the same picture and the same crowd census as --crowd-ours alone (crowd first, then the replace hook).
  7. --arena-ours / --wrestlers-ours with no class registered refuse loudly (non-zero exit and a message), never silently draw the stream.
A check that cannot read its input fails loudly."""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def md5(path):
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--frame', type=int, default=200)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print("FAIL  input missing: Check B's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    pin_file = os.path.join(root, 'tools', 'crowd', 'crowd_ours_check.py')
    m = re.search(r"BASE_STREAM_MD5\s*=\s*'([0-9a-f]{32})'", open(pin_file, encoding='utf-8').read()) if os.path.isfile(pin_file) else None
    if not m:
        print('FAIL  cannot read BASE_STREAM_MD5 from ' + pin_file)
        return 1
    base_md5 = m.group(1)
    out = os.path.join(root, 'out', 's101', 'replace_seam')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'case_brock'))
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        print('FAIL  the capture did not unpack: ' + cap)
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1

    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    # ---- the unit test of the row override on the real effect tables ----
    clang = next((c for c in ['C:/Program Files/LLVM/bin/clang++.exe', 'clang++'] if os.path.isfile(c) or c == 'clang++'), 'clang++')
    os.makedirs(os.path.join(root, 'out', 'tools'), exist_ok=True)
    selftest = os.path.join(root, 'out', 'tools', 'replace_seam_selftest.exe')
    rc, text = run([clang, '-O1', '-std=c++20', '-Wall', '-Wextra', '-o', selftest, os.path.join(root, 'tools', 'replace', 'seam_selftest.cpp')], 600)
    if rc:
        print(text[-2000:])
        verdict(False, 'row override unit test: build failed (exit %d)' % rc)
    else:
        rc, text = run([selftest], 600)
        last = text.strip().splitlines()[-1] if text.strip() else '(no output)'
        verdict(rc == 0 and 'failures 0' in last and 'missed 0' in last, 'row override unit test (real effect tables, independent per-register model, planted late / early / too many / not applied): ' + last)

    def arm(name, extra, expect_fail=False):
        d = os.path.join(out, 'arm_' + name)
        rc, text = run([exe, '--capture', cap, '--frame', str(args.frame), '--judge-out', d, '--root', root] + extra)
        png = os.path.join(d, 'picture.png')
        if expect_fail:
            return rc, text, None
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s failed (exit %d)' % (name, rc))
        return rc, text, png

    census = re.compile(r'\[rdr\]\[test\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    claims_re = re.compile(r'claims seen by the test hook (\d+)')

    def parse(text):
        c, k = census.search(text), claims_re.search(text)
        if not c or not k:
            print(text[-1500:])
            raise RuntimeError('no [rdr][test] census / claims line')
        v = list(map(int, c.groups()))
        return dict(ours=v[0], fallback=v[1], replaced=v[2], kept=v[3], not_ready=v[4], unjoined=v[5], mismatch=v[6], variant=v[7], texture=v[8], palette=v[9], not_ours=v[10], claims=int(k.group(1)))

    try:
        _, _, stream = arm('stream', [])
        _, t_clean, clean = arm('clean', ['--replace-test'])
        _, t_all, all_png = arm('all', ['--replace-test', '--replace-fault', 'all'])
        _, t_odd, odd = arm('refuse_odd', ['--replace-test', '--replace-fault', 'refuse_odd'])
        _, t_nr, not_ready = arm('not_ready', ['--replace-test', '--replace-fault', 'not_ready'])
        _, t_nv, no_vertex = arm('no_vertex', ['--replace-test', '--replace-fault', 'no_vertex'])
        _, t_rows, rows = arm('rows', ['--replace-test', '--replace-fault', 'rows'])
        _, t_off, off = arm('rows_off_by_one', ['--replace-test', '--replace-fault', 'rows_off_by_one'])
        _, t_half, half = arm('half_count', ['--replace-test', '--replace-fault', 'half_count'])
        _, t_swap, swap = arm('slot_swap', ['--replace-test', '--replace-fault', 'slot_swap'])
        _, t_topo, topo = arm('topology', ['--replace-test', '--replace-fault', 'topology'])
        _, t_unset, unset = arm('unset_layout', ['--replace-test', '--replace-fault', 'unset_layout'])
        _, t_same, same = arm('slot_same', ['--replace-test', '--replace-fault', 'slot_same'])
        _, t_crowd, crowd = arm('crowd', ['--crowd-ours', '--game', game])
        _, t_both, both = arm('crowd_test', ['--crowd-ours', '--game', game, '--replace-test'])
        rc_a, t_arena, _ = arm('arena_refused', ['--arena-ours'], expect_fail=True)
        rc_w, t_wres, _ = arm('wrestlers_refused', ['--wrestlers-ours'], expect_fail=True)
        reg_line = re.search(r"replace classes registered: (.*)", t_arena)
        registered = set(x.strip() for x in reg_line.group(1).split(',')) if reg_line else None
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1

    h = {n: md5(p) for n, p in [('stream', stream), ('clean', clean), ('all', all_png), ('odd', odd), ('not_ready', not_ready), ('no_vertex', no_vertex), ('rows', rows), ('off', off), ('crowd', crowd), ('both', both), ('half', half), ('swap', swap), ('topo', topo), ('unset', unset), ('same', same)]}
    verdict(h['stream'] == base_md5, 'switch off: the stream arm\'s picture md5 %s equals the crowd lane\'s pinned base %s (every switch off draws what the base drew)' % (h['stream'], base_md5))

    c = parse(t_clean)
    verdict(h['clean'] == h['stream'] and c['replaced'] > 1000 and c['kept'] == 0 and c['fallback'] == 0,
            'transparent: the test hook replaced %d draws with their own stream geometry (kept %d, fallback frames %d); picture md5 equal to the stream\'s: %s' % (c['replaced'], c['kept'], c['fallback'], h['clean'] == h['stream']))
    a = parse(t_all)
    verdict(h['all'] == h['stream'] and a['replaced'] > c['replaced'] and a['kept'] == 0, 'transparent with every named draw class claimed: replaced %d (> model parts alone %d), kept %d, picture equal: %s' % (a['replaced'], c['replaced'], a['kept'], h['all'] == h['stream']))
    for name, p in [('transparent', c), ('all', a)]:
        verdict(p['replaced'] + p['kept'] == p['claims'], 'counts (%s): replaced %d + kept %d == the hook\'s own tally of claimed draws %d' % (name, p['replaced'], p['kept'], p['claims']))

    o = parse(t_odd)
    verdict(h['odd'] == h['stream'] and o['replaced'] > 0 and o['kept'] > 0 and o['not_ours'] == o['kept'] and o['replaced'] + o['kept'] == o['claims'],
            'refusals keep the stream draw (refuse_odd): replaced %d, kept %d (all "not ours"), claims %d; picture equal: %s' % (o['replaced'], o['kept'], o['claims'], h['odd'] == h['stream']))
    n = parse(t_nr)
    verdict(h['not_ready'] == h['stream'] and n['replaced'] == 0 and n['kept'] > 0 and n['not_ready'] == n['kept'] and n['fallback'] > 0 and n['kept'] == n['claims'],
            'not ready (FrameReady false): replaced %d, kept %d (all "not ready"), fallback frames %d, claims %d; picture equal: %s' % (n['replaced'], n['kept'], n['fallback'], n['claims'], h['not_ready'] == h['stream']))
    v = parse(t_nv)
    verdict(h['no_vertex'] == h['stream'] and v['replaced'] == 0 and v['kept'] > 0 and v['mismatch'] == v['kept'] and v['kept'] == v['claims'],
            'a hook that says yes and gives no vertices (no_vertex): replaced %d, kept %d (all "mismatch"), claims %d; picture equal: %s' % (v['replaced'], v['kept'], v['claims'], h['no_vertex'] == h['stream']))

    r = parse(t_rows)
    verdict(h['rows'] == h['stream'] and r['replaced'] > 1000 and r['replaced'] + r['kept'] == r['claims'], 'row override transparent (the stream\'s own rows written through the override): replaced %d, picture equal: %s' % (r['replaced'], h['rows'] == h['stream']))
    s_img = np.asarray(Image.open(stream).convert('RGB'), dtype=np.int32)
    o_img = np.asarray(Image.open(off).convert('RGB'), dtype=np.int32)
    px = int((np.abs(s_img - o_img).sum(axis=2) > 0).sum())
    p = parse(t_off)
    verdict(h['off'] != h['stream'] and px > 0 and p['replaced'] > 1000, 'planted rows_off_by_one (the rows one register late) is CAUGHT: %d pixels differ from the stream (replaced %d)' % (px, p['replaced']))

    crowd_census = re.compile(r'\[rdr\]\[crowd\] ours frames (\d+), fallback (\d+), stream draws dropped (\d+), members drawn (\d+)')
    cc, cb = crowd_census.search(t_crowd), crowd_census.search(t_both)
    pb = parse(t_both)
    verdict(cc is not None and cb is not None and cc.groups() == cb.groups() and h['both'] == h['crowd'] and pb['replaced'] > 1000 and pb['replaced'] + pb['kept'] == pb['claims'],
            'coexists with the crowd hook: crowd census %s alone and with the test hook; test hook replaced %d (claims %d); picture equal to the crowd-only one: %s' % (cc.groups() if cc else None, pb['replaced'], pb['claims'], h['both'] == h['crowd']))

    def pixels(a, b):
        ia = np.asarray(Image.open(a).convert('RGB'), dtype=np.int32)
        ib = np.asarray(Image.open(b).convert('RGB'), dtype=np.int32)
        return int((np.abs(ia - ib).sum(axis=2) > 0).sum())

    # new plants (review of 60c29deb): each must CHANGE the picture and the transparent slot arm must not
    for name, key, text in [('half_count (index_count / 2)', 'half', t_half), ('slot_swap (slot 0 set to another image of the frame)', 'swap', t_swap), ('topology (a strip given as a list or the reverse)', 'topo', t_topo)]:
        q = parse(text)
        px_n = pixels(stream, {'half': half, 'swap': swap, 'topo': topo}[key])
        verdict(h[key] != h['stream'] and px_n > 0 and q['replaced'] > 1000 and q['replaced'] + q['kept'] == q['claims'], 'planted %s is CAUGHT: %d pixels differ from the stream (replaced %d)' % (name, px_n, q['replaced']))
    u = parse(t_unset)
    verdict(h['unset'] == h['stream'] and u['replaced'] == 0 and u['kept'] == u['claims'] and u['mismatch'] == u['kept'] and u['kept'] > 0,
            'unset_layout (a Replace that leaves the layout unset): refused and counted "mismatch" (%d of %d claims), replaced %d; picture = stream: %s' % (u['mismatch'], u['claims'], u['replaced'], h['unset'] == h['stream']))
    sm = parse(t_same)
    verdict(h['same'] == h['stream'] and sm['replaced'] > 1000 and sm['replaced'] + sm['kept'] == sm['claims'] and sm['texture'] == 0,
            'slot 0 set to the stream own image (its view format and swizzle): transparent, replaced %d, texture refusals %d; picture equal: %s' % (sm['replaced'], sm['texture'], h['same'] == h['stream']))

    if registered is None:
        verdict(False, 'fview did not print which replace classes are registered')
    else:
        for kind, rc_k, text in [('arena', rc_a, t_arena), ('wrestlers', rc_w, t_wres)]:
            if kind in registered:
                print('SKIP  %s class is registered in this build: the "no class registered" arm does not apply (its own lane gate proves it)' % kind)
            else:
                verdict(rc_k != 0 and ('no %s class' % kind) in text, '--%s-ours with no class registered refuses loudly (exit %d, message named)' % (kind, rc_k))
    print('%s  (%d failed)' % ('PASS' if not fails else 'FAIL', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
