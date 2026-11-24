r"""Lane G4c (S102) offline proof: the blank plane shadow chain is not drawn under the players' config (shipped default bundle + the half rate).
  py -I tools\renderer\g4c_check.py [--root DIR] [--only unit|fview|counts]
It builds on lane G4b (src\renderer_adapter\plane_blank.h, switch wwe2k14_rdr_skip_blank_plane; docs\renderer\DEAD_WORK.md) and adds the half-rate rule: a frame whose half rate
already leaves the chain out is neither the ledger's "first executed run" nor counted as taken.
0 game runs, 0 Surface runs. Counts and picture hashes only.

  unit     plane_blank_test.cpp compiles and passes; the planted twins (no_first_run, ignore_overlap, ignore_half, drop_mask, ignore_readers) each FAIL it.
  fview    fview --replay --validation 1 --hash-frames, bundle + --shadow-half, switch off vs on (exit code AND `validation errors N` are read):
             the standard Bryan capture (237 frames, scene records), lane86 Bryan (91) and lane86 Check B (120): picture hashes equal on EVERY frame; Bryan: the lever TOOK frames
             (skipped > 0 and every skipped frame is a drawn one: bound draws fall by exactly the census' draws); Check B: nothing skipped.
           Twins (must be CAUGHT, pictures differ): `always` (a chain that is not blank, skipped) on Check B under the bundle + half rate; `window` [70,80): the first window frame
             equals the off arm (the ledger's executed run), the later ones differ and no frame outside the window does.
  counts   rplay --counts (src\renderer\counts.h) on the standard Bryan capture, same bundle, off vs on: passes, draws, copies, clears, barriers, pipeline binds and area_px per frame
             must fall (none rises); the fall is printed per frame; --draw-census 2 adds the colour passes' fragment shader invocations.
Exit 1 on any failure or an input that cannot be read."""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--particles-ours', '--ui-ours', '--ui-text-ours', '--post-ours',
          '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--rim-ours', '--rim-chain-ours', '--small-passes-ours', '--circles-ours', '--caw-ours', '--movie-ours', '--screens-ours']
HALF = ['--shadow-half']
CENSUS_RE = re.compile(r'\[rdr\]\[plane_blank\] frames (\d+) \(unplanned (\d+), no chain (\d+), no origins (\d+), not guest (\d+)\), chains (\d+): live (\d+), outside (\d+), declined \(overlap (\d+), taken (\d+)\), '
                       r'irregular (\d+), first run (\d+), skipped (\d+) \(targets (\d+), clears (\d+), draws (\d+), copies (\d+)\), ledger cleared (\d+), half-rate frames (\d+), mask kept (\d+), planner ([\d.]+) us a frame')
CKEYS = ('frames', 'unplanned', 'no_chain', 'no_origins', 'not_guest', 'chains', 'live', 'outside', 'declined_overlap', 'declined_taken', 'irregular', 'first_run', 'skipped', 'targets', 'clears',
         'draws', 'copies', 'cleared', 'half', 'mask_kept', 'us')


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        return os.path.dirname(common) if common else root
    except Exception:
        return root


def census(text):
    m = CENSUS_RE.search(text)
    if not m:
        return None
    return dict(zip(CKEYS, (float(x) if k == 'us' else int(x) for k, x in zip(CKEYS, m.groups()))))


def fview(root, main, cap_rel, frames, tag, extra, game=True, timeout=2400):
    out = os.path.join(root, 'out', 's102', 'g4c')
    os.makedirs(out, exist_ok=True)
    csv = os.path.join(out, tag + '.csv')
    if os.path.exists(csv):
        os.remove(csv)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cmd = [exe, '--capture', os.path.join(main, 'out', cap_rel), '--replay', '--frames', str(frames), '--validation', '1', '--hash-frames', csv]
    if game:
        cmd += ['--game', os.path.join(main, 'game_data', 'extracted')]
    p = subprocess.run(cmd + extra, capture_output=True, text=True, timeout=timeout)
    text = p.stdout + p.stderr
    errs = re.search(r'validation errors (\d+)', text)
    m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', text)
    hashes = [l.strip() for l in open(csv) if l.strip() and not l.startswith('format')] if os.path.exists(csv) else []
    return dict(rc=p.returncode, errors=int(errs.group(1)) if errs else -1, hashes=hashes, bound=int(m.group(3)) if m else -1, c=census(text), text=text)


def unit(root):
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    out = os.path.join(root, 'out', 's102', 'g4c')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'plane_blank_test.exe')
    b = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-I', os.path.join(root, 'src'), '-I', os.path.join(root, 'src', 'renderer_adapter'),
                        os.path.join(root, 'src', 'renderer_adapter', 'plane_blank_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print(b.stdout + b.stderr)
        say(False, 'plane_blank_test.cpp did not compile')
        return
    u = subprocess.run([exe], capture_output=True, text=True)
    say(u.returncode == 0 and 'all passed' in u.stdout, 'unit test (G4b ledger rules + G4c half-rate frame + reader of a chain image): %s' % u.stdout.strip().splitlines()[-1])
    for plant in ('plant_no_first_run', 'plant_ignore_overlap', 'plant_ignore_half', 'plant_drop_mask', 'plant_ignore_readers'):
        u = subprocess.run([exe, plant], capture_output=True, text=True)
        say(u.returncode != 0 and u.stdout.count('FAIL ') >= 1, 'unit twin %s: CAUGHT (%d failing checks)' % (plant, u.stdout.count('FAIL ')))


def fview_arms(root, main):
    caps = (('gate_bryan', 's101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237, True, BUNDLE),
            ('bryan', 'lane86/bryan/capture', 91, False, ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']),
            ('brock', 'lane86/brock/capture', 120, False, ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']))
    res = {}
    for name, cap, frames, game, flags in caps:
        if not os.path.isdir(os.path.join(main, 'out', cap)):
            say(False, 'cannot read capture %s' % cap)
            continue
        off = fview(root, main, cap, frames, name + '_off', flags + HALF, game)
        on = fview(root, main, cap, frames, name + '_on', flags + HALF + ['--skip-blank-plane'], game)
        res[name] = (off, on)
        say(off['rc'] == 0 and on['rc'] == 0 and off['errors'] == 0 and on['errors'] == 0, '%s: exit %d / %d, validation errors %d / %d' % (name, off['rc'], on['rc'], off['errors'], on['errors']))
        eq = off['hashes'] == on['hashes'] and len(off['hashes']) == frames
        diff = [i for i in range(min(len(off['hashes']), len(on['hashes']))) if off['hashes'][i] != on['hashes'][i]]
        say(eq, '%s (bundle + half rate): %d frames, picture hashes %s on every frame%s' % (name, frames, 'equal' if eq else 'DIFFER', '' if eq else ' (frames %s)' % diff[:10]))
        c = on['c']
        if c is None:
            say(False, '%s: no [rdr][plane_blank] census line' % name)
            continue
        print('  %s census: %s' % (name, c))
        if name.endswith('bryan'):
            say(c['skipped'] > 0 and c['draws'] > 0, '%s: the lever TOOK %d of %d frames (first run %d, half-rate frames %d, live %d, outside %d, declined %d): %d draws, %d passes, %d copies left out' %
                (name, c['skipped'], c['frames'], c['first_run'], c['half'], c['live'], c['outside'], c['declined_overlap'] + c['declined_taken'], c['draws'], c['targets'], c['copies']))
            say(c['skipped'] + c['first_run'] + c['half'] == c['chains'], '%s: every chain frame is exactly one of taken / first run / half-rate (%d + %d + %d = %d)' % (name, c['skipped'], c['first_run'], c['half'], c['chains']))
            say(off['bound'] - on['bound'] == c['draws'], '%s: bound draws %d -> %d, fall %d = the census draws %d (a taken frame is a drawn frame)' % (name, off['bound'], on['bound'], off['bound'] - on['bound'], c['draws']))
            say(c['us'] < 200.0, '%s: the planner costs %.1f us a frame (render thread, per frame, never per draw)' % (name, c['us']))
        else:
            say(c['skipped'] == 0, '%s (Check B, a live chain): nothing skipped (%d), live %d of %d' % (name, c['skipped'], c['live'], c['chains']))
    if 'brock' in res:
        off = res['brock'][0]
        for tag, fault in (('always', ['always']), ):
            t = fview(root, main, 'lane86/brock/capture', 120, 'brock_' + tag, ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours'] + HALF + ['--skip-blank-plane', '--skip-blank-plane-fault'] + fault, False)
            diff = [i for i in range(120) if t['hashes'][i] != off['hashes'][i]]
            say(len(diff) >= 1, 'twin always (a chain that is NOT blank, skipped; bundle + half): CAUGHT, pictures differ on %d of 120 frames (first %s)' % (len(diff), diff[:1]))
        sys.path.insert(0, os.path.join(root, 'tools', 'shadow'))
        import shadow_census as sc
        gcap = sc.Cap(os.path.join(main, 'out', 'lane86/brock/capture'))
        def half_frame(i):   # binder.h: bind N = capture frame i + 1 is a half-rate frame when N is odd and above 8 (the shadow set is learned by then)
            return (i + 1) % 2 == 1 and (i + 1) > 8
        flags = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours'] + HALF
        glo, ghi = int(gcap.c.frame(70).guest_frame), int(gcap.c.frame(80).guest_frame)
        t = fview(root, main, 'lane86/brock/capture', 120, 'brock_window', flags + ['--skip-blank-plane', '--skip-blank-plane-fault', 'window', '--skip-blank-plane-window', str(glo), str(ghi)], False)
        diff = [i for i in range(120) if t['hashes'][i] != off['hashes'][i]]
        allowed = set(range(71, 80)) | ({80} if half_frame(80) else set())   # F2: frame 80 only when it is a half frame (the half rate shows the last drawn picture one more frame)
        say(len(diff) >= 1 and set(diff) <= allowed, 'twin window [70,80): CAUGHT, pictures differ on %s, inside the allowed set %s (71..79, 80 only as a half frame: %s); frame 70 (the executed run) equals the off arm: %s' %
            (diff, sorted(allowed), half_frame(80), 70 not in diff))
        glo2 = int(gcap.c.frame(82).guest_frame)   # planted stop_late: the planner's window runs two frames past the end the picture check was derived from
        t = fview(root, main, 'lane86/brock/capture', 120, 'brock_stop_late', flags + ['--skip-blank-plane', '--skip-blank-plane-fault', 'window', '--skip-blank-plane-window', str(glo), str(glo2)], False)
        diff = [i for i in range(120) if t['hashes'][i] != off['hashes'][i]]
        say(not set(diff) <= allowed, 'twin stop_late (the skip window ends 2 frames late): CAUGHT, pictures differ on %s, outside the allowed set' % diff)
        # F3: G3's props turn on (--turns 4) with the bundle + half rate: the lever must still take frames (or say 0 on purpose)
        c = fview(root, main, 's101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237, 'gate_bryan_turns_off', BUNDLE + HALF + ['--turns', '4'], True)
        d = fview(root, main, 's101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237, 'gate_bryan_turns_on', BUNDLE + HALF + ['--turns', '4', '--skip-blank-plane'], True)
        say(c['rc'] == 0 and d['rc'] == 0 and c['errors'] == 0 and d['errors'] == 0 and c['hashes'] == d['hashes'] and len(c['hashes']) == 237, 'gate_bryan with G3 --turns 4: pictures equal on every frame, validation %d / %d, exit %d / %d' % (c['errors'], d['errors'], c['rc'], d['rc']))
        say(d['c'] is not None and d['c']['skipped'] > 0 and d['c']['declined_overlap'] == 0, 'gate_bryan with G3 --turns 4: the lever TOOK %s frames, declined as overlap %s (G3 turns are the same case as the half rate)' % (d['c'] and d['c']['skipped'], d['c'] and d['c']['declined_overlap']))
    # evidence for the ledger's A / B (docs): on Bryan the mask's copy (image 220) and B (221) are one constant picture in every frame (a sweep hash per frame), so the planner's "blank" frames
    # leave the pictures a frame of the off arm would show. NOT a proof for other scenes: the lane keeps the mask pass whenever a half rate / turn frame can leave it out (keep_mask).
    out = os.path.join(root, 'out', 's102', 'g4c')
    csv = os.path.join(out, 'sweep_bryan.csv')
    p = subprocess.run([os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe'), '--capture', os.path.join(main, 'out', 'lane86/bryan/capture'), '--sweep', csv, '--sweep-images', '220,221', '--validation', '1'],
                       capture_output=True, text=True, timeout=900)
    rows = [r for r in (l.strip().split(',') for l in open(csv)) if len(r) >= 4 and r[0].strip().isnumeric()] if p.returncode == 0 and os.path.exists(csv) else []
    say(len(rows) == 91 and len({r[2] for r in rows}) == 1 and len({r[3] for r in rows}) == 1, 'bryan: image 220 (the mask copy) and 221 (B) are one constant picture in all %d frames (%d / %d distinct hashes)' %
        (len(rows), len({r[2] for r in rows}), len({r[3] for r in rows})))
    return res


def counts_arms(root, main):
    cap = os.path.join(main, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    game = os.path.join(main, 'game_data', 'extracted')
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    out = os.path.join(root, 'out', 's102', 'g4c')
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        say(False, 'cannot read the standard capture %s' % cap)
        return

    def run(tag, extra, census_level=0):
        path = os.path.join(out, 'counts_%s.json' % tag)
        if os.path.exists(path):
            os.remove(path)
        cmd = [exe, '--capture', cap, '--frames', '237', '--game', game, '--counts', path] + BUNDLE + HALF + extra
        if census_level:
            cmd += ['--draw-census', str(census_level)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=2400)
        text = p.stdout + p.stderr
        if p.returncode != 0 or not os.path.exists(path):
            print(text[-1500:])
            say(False, 'rplay %s: exit %d' % (tag, p.returncode))
            return None
        d = json.load(open(path, encoding='utf-8'))
        return d, text

    a = run('off', [], 2)
    b = run('on', ['--skip-blank-plane'], 2)
    if a is None or b is None:
        return
    keys = ['draws', 'indexed_draws', 'copies', 'clears', 'barriers', 'barrier_images', 'pipeline_binds', 'descriptor_binds', 'area_px']
    fa, fb = a[0]['frames'], b[0]['frames']
    say(len(fa) == len(fb) and len(fa) > 200, 'counts: %d / %d frames' % (len(fa), len(fb)))
    n = len(fa)
    rows = []
    rose = []
    for k in ['passes'] + keys:
        sa = sum((f['passes'] if k == 'passes' else f['total'][k]) for f in fa)
        sb = sum((f['passes'] if k == 'passes' else f['total'][k]) for f in fb)
        rows.append((k, sa / n, sb / n))
        if sb > sa:
            rose.append(k)
    for k, x, y in rows:
        print('  per frame %-16s %14.1f -> %14.1f  (%+.1f)' % (k, x, y, y - x))
    say(not rose, 'counts: nothing rises under the bundle + half rate (%s)' % (rose or 'none'))
    say(rows[0][2] < rows[0][1] and rows[2][2] < rows[2][1], 'counts: passes %.2f -> %.2f a frame, indexed draws %.1f -> %.1f a frame (they fall)' % (rows[0][1], rows[0][2], rows[2][1], rows[2][2]))
    fs = []
    for tag, r in (('off', a), ('on', b)):
        m = re.search(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame', r[1])
        fs.append(m.groups() if m else None)
    print('  draw census (all colour passes) off %s / on %s' % tuple(fs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--only')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    if not a.only or a.only == 'unit':
        unit(root)
    if not a.only or a.only == 'fview':
        if not os.path.exists(os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')):
            say(False, 'no fview.exe (tools\\fview\\build.ps1)')
        else:
            fview_arms(root, main_co)
    if not a.only or a.only == 'counts':
        if not os.path.exists(os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')):
            say(False, 'no rplay.exe (tools\\renderer\\build.ps1)')
        else:
            counts_arms(root, main_co)
    print('RESULT', 'FAIL' if FAILS else 'PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
