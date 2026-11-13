#!/usr/bin/env python3
"""Lane 134 acceptance test, offline, 0 game runs: the character PIECES (the referee's shirt patch, ch255 attire 0 entry 21011; the alpha piece 20002) drawn by ours.

  py -I tools\\wrestler\\pieces_check.py [--root DIR] [--no-build]

Why they were the stream's (docs\\renderer\\WRESTLER_PROMOTION_MAP.md section 13): a yCh_trans piece of a file that carries a tangent stream (S3) is drawn from an 11-dword vertex (one of
tangent / colour is not written), the roster's file map said 12, so wrestler_geom::Classify refused the layout and the object stayed unjoined. Classify now also tries the two
11-dword maps (src\\assets\\wrestler_geom.h).

Inputs (a missing one FAILS, never skips): Check I's in-ring match (run 7083), run 7079 (Brock, Check F), Check B (run 7051), WM30 (run 7084), the game files.
CPU  out\\tools\\wrestler_ours_check.exe over EVERY frame of the four captures: nothing unjoined / mismatched / variant, wrong counters 0, pieces replaced; 7083: the 12 objects join (10 before)
     and --dump-kept lists no unjoined draw. Plants piece_wrong_entry (the next pac entry) and piece_bind_pose (identity bones): checks ON nothing wrong gets through and the pieces stay the
     stream's; checks OFF the independent proof sees them. R (lane 100's body part from another attire, per part by bytes): plant own_model_only re-found on WM30, pieces unaffected on 7083.
PIC  fview --wrestlers-ours on 7083 frames 200 and 400: the picture equals the pieces-refused arm (plant piece_wrong_entry, checks on) pixel for pixel, i.e. drawing the pieces ourselves
     changes nothing; the plants' pictures with checks ON equal it, with checks OFF they break; validation errors 0; census mismatch 0 / unjoined 0 / variant 0."""
import argparse
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


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


CAPS = [('match7083', '20261112_131008_s100_owner_match'), ('brock7079', '20261112_124537_s100_owner_s89_brock'),
        ('checkB7051', '20261111_215201_s100_owner_s89_brock'), ('wm30_7084', '20261112_131029_s100_owner_s90_wm30')]


def kv(line):
    return {k: int(v) for k, v in (x.split('=') for x in line.split())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        print('FAIL  numpy / Pillow are needed: %s' % e)
        return 1
    out = os.path.join(main_co, 'out', 's101', 'pieces_ours')
    caps = {}
    for name, run_dir in CAPS:
        d = os.path.join(main_co, 'out', 's101', 'runs', run_dir)
        zips = [os.path.join(d, f) for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(d) else []
        if not zips:
            print('FAIL  input missing: the case zip of %s under %s' % (name, d))
            return 1
        cap = os.path.join(out, name, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            os.makedirs(os.path.join(out, name), exist_ok=True)
            zipfile.ZipFile(zips[0]).extractall(os.path.join(out, name))
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            print('FAIL  the capture of %s did not unpack: %s' % (name, cap))
            return 1
        caps[name] = cap
    if not args.no_build:
        for what, script in [('wrestler_ours_check', 'tools/wrestler/build_wrestler_ours_check.ps1'), ('fview', 'tools/fview/build.ps1')]:
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, script)], 2400)
            if rc:
                print(text[-3000:])
                print('FAIL  %s build (exit %d)' % (what, rc))
                return 1
    cpu_exe = os.path.join(root, 'out', 'tools', 'wrestler_ours_check.exe')
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview_exe = _fe.find_or_exit(root=root, kind='dev')
    for p in (cpu_exe, fview_exe):
        if not os.path.isfile(p):
            print('FAIL  missing ' + p)
            return 1
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    def cpu(name, faults='', frames=None, dump=False):
        cmd = [cpu_exe, '--capture', caps[name], '--game', game]
        if frames:
            cmd += ['--frames', frames]
        if faults:
            cmd += ['--fault', faults]
        if dump:
            cmd += ['--dump-kept']
        rc, text = run(cmd, 2400)
        m = re.search(r'^RESULT (.*)$', text, re.M)
        pm = re.search(r'^PIECES (.*)$', text, re.M)
        if rc or not m or not pm:
            print(text[-2500:])
            raise RuntimeError('cpu arm %s %r failed (exit %d)' % (name, faults, rc))
        r = kv(m.group(1))
        r['pieces'] = kv(pm.group(1))
        r['kept_unjoined_lines'] = len(re.findall(r'^KEPT .* why 1 ', text, re.M))   # KeepWhy::Unjoined
        r['unjoined_objects'] = len(set(re.findall(r'^KEPT frame \d+ draw \d+ object (0x[0-9A-F]+) part \d+ why 1 ', text, re.M)))
        return r

    wrong = ('wrong_vertex', 'wrong_index', 'wrong_rows', 'wrong_count')
    # ---------------- CPU half: clean, every frame ----------------
    for name in caps:
        r = cpu(name, dump=True)
        pc = r['pieces']
        verdict(all(r[k] == 0 for k in wrong) and r['kept_unjoined'] == 0 and r['kept_mismatch'] == 0 and r['kept_variant'] == 0 and r['kept_texture'] == 0 and pc['replaced'] > 0
                and pc['proof_wrong_rows'] == 0 and pc['proof_wrong_vertex'] == 0 and pc['palette_refused'] == 0 and pc['no_owner'] == 0 and r['kept_unjoined_lines'] == 0,
                '%-10s every frame: replaced %d (pieces %d draws, linked %d object-frames), unjoined 0 mismatch 0 variant 0, wrong 0, palette-refused pieces %d%s' % (
                    name, r['replaced'], pc['replaced'], pc['linked_object_frames'], pc['palette_refused'], ('; kept palette %d (Brock, the known refusal)' % r['kept_palette']) if r['kept_palette'] else ''))
        if name == 'match7083':
            verdict(r['objects_joined'] == 12 and pc['linked_object_frames'] > 0, 'match7083: 12 objects joined (10 before: the referee patch A95B8B00 and the alpha piece A95B2D10 were unjoined), got %d' % r['objects_joined'])
    # ---------------- follow-up plants: the short maps ----------------
    ut = os.path.join(root, 'out', 'tools', 'classify_unit.exe')
    rc, ub = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools/wrestler/build_classify_unit.ps1')], 900)
    verdict(rc == 0 and os.path.isfile(ut), 'classify_unit builds')
    rc, utext = run([ut], 120)
    verdict(rc == 0 and '0 failure(s)' in utext and utext.count('PASS') >= 9, 'Classify unit test on synthetic element lists: f17@9 only Tangent, f6@9 only Colour, no dword-9 element gives equal Roles, stride-2 refused, no S3 no fallback, the no-normal shape accepted but Compare fails (9 checks)')
    rc, ptext = run([ut, '--plant'], 120)
    verdict(rc != 0 and 'FAIL  A:' in ptext, 'classify_unit twin: with the short maps off, case A fails')
    probe = cpu('match7083', '', '190-215', dump=True)
    verdict(probe['unjoined_objects'] == 0 and probe['objects_joined'] == 12, 'match7083 frames 190-215 clean: unjoined objects 0, 12 joined')
    for plant in ('no_short_maps', 'short_map_shift'):
        for off in ('', ',checks_off'):
            r = cpu('match7083', plant + off, '190-215', dump=True)
            verdict(r['objects_joined'] == 10 and r['unjoined_objects'] == 2 and all(r[k] == 0 for k in wrong) and r['kept_unjoined'] > 0,
                    'plant %-15s checks %-3s: back to 10 joined / 2 unjoined objects (the patch and the alpha piece), %d draws kept Unjoined, wrong 0' % (plant, 'OFF' if off else 'ON', r['kept_unjoined']))
    # ---------------- plants on the match ----------------
    frames = '190-215'
    base = cpu('match7083', '', frames)
    for plant, seen in (('piece_wrong_entry', ('wrong_count',)), ('piece_bind_pose', ('wrong_rows',))):
        on = cpu('match7083', plant, frames)
        verdict(all(on[k] == 0 for k in wrong) and on['pieces']['replaced'] == 0 and on['replaced'] < base['replaced'],
                'plant %-17s checks ON : nothing wrong gets through (wrong 0), the pieces stay the stream\'s (%d -> %d draws replaced)' % (plant, base['replaced'], on['replaced']))
        off = cpu('match7083', plant + ',checks_off', frames)
        verdict(any(off[k] > 0 for k in seen), 'plant %-17s checks OFF: the independent proof sees it (%s)' % (plant, ', '.join('%s=%d' % (k, off[k]) for k in wrong)))
    r_on = cpu('wm30_7084', 'own_model_only', '100-200')
    r_ok = cpu('wm30_7084', '', '100-200')
    verdict(r_ok['kept_mismatch'] == 0 and r_on['kept_mismatch'] > 0 and r_on['pieces']['replaced'] == r_ok['pieces']['replaced'],
            'R (lane 100 re-planted): own_model_only on WM30 still re-found (Mismatch %d, clean 0), the pieces unaffected (%d draws)' % (r_on['kept_mismatch'], r_on['pieces']['replaced']))
    r7083 = cpu('match7083', 'own_model_only', frames)
    verdict(r7083['pieces']['replaced'] == base['pieces']['replaced'] and r7083['kept_mismatch'] == 0, 'R: own_model_only on the match: the pieces take their own attire\'s model (same %d draws)' % r7083['pieces']['replaced'])

    # ---------------- PIC half ----------------
    def fview(name, faults, frame, extra=()):
        d = os.path.join(out, 'arm_%s_f%d' % (name, frame))
        cmd = [fview_exe, '--capture', caps['match7083'], '--frame', str(frame), '--judge-out', d, '--root', root] + list(extra)
        if faults is not None:
            cmd += ['--wrestlers-ours', '--game', game]
            if faults:
                cmd += ['--replace-fault', faults]
        for attempt in range(4):
            rc, text = run(cmd, 1200)
            if rc and 'Not enough graphics memory for the frame ring' in text and attempt < 3:
                print('  (fview: the shared GPU was full, retry %d)' % (attempt + 1))
                continue
            break
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('fview arm %s failed (exit %d)' % (name, rc))
        return png, text

    def load(png):
        return np.asarray(Image.open(png).convert('RGB')).astype(int)

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int((d > 0).sum()), int(d.max())

    for frame in (200, 400):
        s_png, _ = fview('stream', None, frame)
        o_png, otext = fview('ours', '', frame)
        p_png, _ = fview('piecesrefused', 'piece_wrong_entry', frame)
        S, O, P = load(s_png), load(o_png), load(p_png)
        n_os, mx_os = diff(S, O)
        n_ps, _ = diff(S, P)
        n_op, mx_op = diff(O, P)
        m = re.search(r'\[rdr\]\[wrestlers\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+)', otext)
        c = [int(x) for x in m.groups()] if m else None
        verdict(n_op == 0 and n_os <= n_ps and c is not None and c[5] == 0 and c[6] == 0 and c[7] == 0,
                'frame %d: ours (pieces drawn by ours) == the pieces-refused arm (%d differing pixels, max %d); against the stream %d px (max %d), the same as without the pieces (%d); census unjoined/mismatch/variant %s' % (
                    frame, n_op, mx_op, n_os, mx_os, n_ps, '%d/%d/%d' % (c[5], c[6], c[7]) if c else '?'))
        for plant in ('piece_wrong_entry', 'piece_bind_pose'):
            on_png, _ = fview(plant, plant, frame)
            off_png, _ = fview(plant + '_off', plant + ',checks_off', frame)
            n_on, _ = diff(O, load(on_png))
            n_off, mx_off = diff(O, load(off_png))
            verdict(n_on == 0 and n_off > 100, 'frame %d plant %-17s checks ON : picture == ours (%d px); checks OFF: the picture breaks (%d px, max %d)' % (frame, plant, n_on, n_off, mx_off))
    for frame in (200, 400):
        p_png, _ = fview('piecesrefused', 'piece_wrong_entry', frame)
        for plant in ('no_short_maps', 'short_map_shift', 'short_map_shift,checks_off'):
            q_png, qtext = fview(plant.replace(',', '_'), plant, frame)
            n_q, _ = diff(load(p_png), load(q_png))
            m = re.search(r'\[rdr\]\[wrestlers\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+)', qtext)
            verdict(n_q == 0 and m is not None and int(m.group(6)) > 0, 'frame %d plant %-26s: picture == the pieces-refused arm (%d px), pieces unjoined (%s draws)' % (frame, plant, n_q, m.group(6) if m else '?'))
    # ---------------- rim arm (fview --wrestlers-ours --rim-ours, every frame) ----------------
    def sweep(tag, extra):
        csv = os.path.join(out, 'rim_%s.csv' % tag)
        if os.path.isfile(csv):
            os.remove(csv)
        cmd = [fview_exe, '--capture', caps['match7083'], '--sweep', csv, '--sweep-slot', '15', '--sweep-images', '1189', '--game', game, '--wrestlers-ours'] + extra
        for attempt in range(4):
            rc, text = run(cmd, 2400)
            if rc and 'Not enough graphics memory for the frame ring' in text and attempt < 3:
                continue
            break
        if not os.path.isfile(csv):
            print(text[-1500:])
            raise RuntimeError('sweep %s failed (exit %d)' % (tag, rc))
        rows = {}
        with open(csv, encoding='utf-8') as f:
            head = f.readline().strip().split(',')
            for l in f:
                v = l.strip().split(',')
                if v[0].isdigit() and len(v) == len(head): rows[int(v[0])] = dict(zip(head, v))
        d = re.search(r'rim map: claimed (\d+) replaced (\d+) \| kept: not ours (\d+), unjoined (\d+) \| verify (\d+) bad (\d+), layout refused (\d+)', text)
        c = re.search(r'\[rdr\]\[rim_map\] ours frames \d+, fallback \d+, replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+)', text)
        return rc, text, rows, ([int(x) for x in d.groups()] if d else None), ([int(x) for x in c.groups()] if c else None)

    def moved(a, b):
        return [f for f in sorted(a) if f in b and any(a[f][k] != b[f][k] for k in ('picture', 'img1189', 'slot15_hash'))]
    _, _, off_rows, _, _ = sweep('off', [])
    rc, ton, on_rows, on_d, on_c = sweep('on', ['--rim-ours', '--validation', '1'])
    bad = moved(off_rows, on_rows)
    verdict(not bad and len(off_rows) == len(on_rows) > 300 and rc == 0 and 'validation errors 0' in ton,
            'rim arm, %d frames: --wrestlers-ours --rim-ours: picture, the rim target (image 1189) and every slot-15 image byte-equal to rim off on every frame (%d differ), validation errors 0' % (len(on_rows), len(bad)))
    verdict(on_d is not None and on_d[1] == on_d[0] and on_d[4] == on_d[1] and on_d[5] == 0 and on_d[6] == 0 and on_c is not None and on_c[4] == 0 and on_c[5] == 0,
            'rim arm census: claimed %s replaced %s verify %s bad %s layout refused %s; mismatch / variant 0 (wrong 0)' % tuple(on_d[i] for i in (0, 1, 4, 5, 6)) if on_d else 'rim census line missing')
    _, _, ns_rows, ns_d, _ = sweep('no_short_maps', ['--rim-ours', '--replace-fault', 'no_short_maps'])
    verdict(on_d is not None and ns_d is not None and on_d[1] - ns_d[1] > 0 and (on_d[1] - ns_d[1]) % 2 == 0 and ns_d[3] == on_d[1] - ns_d[1] and not moved(off_rows, ns_rows),
            "the pieces' rim draws (the patch and the alpha piece, one each a frame) are REPLACED when joined: %s more replaced than with the short maps off (%s kept unjoined there); the rim target stays byte-equal in both" % ((on_d[1] - ns_d[1]) if on_d and ns_d else '?', ns_d[3] if ns_d else '?'))
    _, vtext = fview('validation', '', 200, ['--validation', '1'])
    verdict('validation active: yes' in vtext and 'validation errors 0' in vtext, 'frame 200 with Vulkan validation on: errors 0')
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
