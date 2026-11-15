r"""Lane G4b (S102) offline proof: the blank plane shadow chain left out of the stream's walk (src\renderer_adapter\plane_blank.h), replayed by fview on captures. 0 game runs.
  py -I tools\renderer\g4b_check.py [--sheets] [--census-only]
Arms (fview --replay, validation on, per-frame picture hashes; switch --skip-blank-plane):
  bryan   lane86\bryan (91 frames, entrance, plane weight 0 in every frame): picture hashes byte-equal off / on; the chain is skipped in 90 of 91 frames (the first one runs once: the ledger);
          bound draws fall by exactly the skipped draws; per frame 4 targets, 3 clears, 35 draws, 4 copies left out.
  brock   lane86\brock (Check B, 120 frames, weight 0.22 .. 0.46): hashes byte-equal off / on; the rule says "not blank" in every frame: nothing skipped.
  census  the capture's own constants (an independent read in Python, tools\shadow\shadow_census.py): Bryan weight 0 in 91 of 91 frames, Brock > 0 in every frame.
Planted twins (must be CAUGHT, one of: the picture hashes differ):
  always   the rule says blank on every frame (Check B): the chain is left out where it draws: the hashes must differ on at least half the frames.
  window   the rule says blank on frames [70, 80) of Check B only: pictures differ inside the window (after the one executed run) and NOWHERE else (the ledger: a wrong skip does not outlive itself).
Exit 1 on any failure or when an input cannot be read."""
# S102 K4: D3_SKIP=every2 / every3 on a capture recorded live (J1) needs a teach list (D3_SKIP_TEACH_OUT on a no-skip run, D3_SKIP_TEACH_IN on the skip arms), else the ring is never learned and pictures are not a baseline: SKIP_COST.md finding 8. fview prints "WARN K4" when it is missing.
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT')
if not MAIN:
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        MAIN = os.path.dirname(common) if common else ROOT
    except Exception:
        MAIN = ROOT
FVIEW = os.path.join(ROOT, 'out', 'renderer', 'dev', 'fview.exe')
if not os.path.exists(FVIEW):
    FVIEW = os.path.join(MAIN, 'out', 'renderer', 'dev', 'fview.exe')
CAPS = {'bryan': ('lane86/bryan/capture', 91), 'brock': ('lane86/brock/capture', 120)}
OUT = os.path.join(ROOT, 'out', 's102', 'g4b')
FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        FAILS.append(text)


def run(tag, cap, frames, extra, env=None):
    os.makedirs(OUT, exist_ok=True)
    csv = os.path.join(OUT, tag + '.csv')
    if os.path.exists(csv):
        os.remove(csv)
    cmd = [FVIEW, '--capture', os.path.join(MAIN, 'out', cap), '--replay', '--frames', str(frames), '--validation', '1', '--hash-frames', csv] + extra
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=e)
    text = p.stdout + p.stderr
    if p.returncode != 0 or not os.path.exists(csv):
        print(text[-1500:])
        say(False, '%s: fview exit %d' % (tag, p.returncode))
        return None
    hashes = [l.strip() for l in open(csv) if l.strip() and not l.startswith('format')]
    m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', text)
    stats = dict(frames=int(m.group(1)), draws=int(m.group(2)), bound=int(m.group(3))) if m else None
    rep = None
    m = re.search(r'\[rdr\]\[plane_blank\] frames (\d+) \(unplanned (\d+), no chain (\d+), no origins (\d+), not guest (\d+)\), chains (\d+): live (\d+), outside (\d+), declined \(overlap (\d+), taken (\d+)\), '
                  r'irregular (\d+), first run (\d+), skipped (\d+) \(targets (\d+), clears (\d+), draws (\d+), copies (\d+)\), ledger cleared (\d+)', text)
    if m:
        keys = ('frames', 'unplanned', 'no_chain', 'no_origins', 'not_guest', 'chains', 'live', 'outside', 'declined_overlap', 'declined_taken', 'irregular', 'first_run', 'skipped', 'targets', 'clears', 'draws', 'copies', 'cleared')
        rep = dict(zip(keys, (int(x) for x in m.groups())))
    m = re.search(r'\[rdr\]\[plane_shadow\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+)', text)
    hook = int(m.group(4)) + int(m.group(3)) if m else None
    errs = re.search(r'validation errors (\d+)', text)
    return dict(hashes=hashes, stats=stats, rep=rep, hook=hook, errors=int(errs.group(1)) if errs else -1)


def census_weights(cap_rel, frames):
    """An independent read of the capture's constants: per frame the set of mask-draw c6.w values (tools\\shadow\\shadow_census.py)."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
    import shadow_census as sc
    import numpy as np
    cap = sc.Cap(os.path.join(MAIN, 'out', cap_rel))
    out = []
    for fi in range(min(frames, len(cap.frames))):
        f = cap.c.frame(fi)
        if not f.has_origins:
            out.append(None)
            continue
        ps = f.origins['pass_site'].astype(np.int64)
        mi = np.nonzero(ps == sc.SITE_PLANE)[0]
        if not len(mi):
            out.append(None)
            continue
        ws = set()
        for i in mi:
            vb = f.bank(int(f.draws[i]['vs_bank']))
            ws.add(float(sc.regs(vb, 6, 1)[0][3]))
        out.append(ws)
    return out


def main():
    if not os.path.exists(FVIEW):
        print('FAIL no fview.exe (tools\\fview\\build.ps1)')
        sys.exit(1)
    for name, (cap, frames) in CAPS.items():
        if not os.path.isdir(os.path.join(MAIN, 'out', cap)):
            print('FAIL cannot read capture', cap)
            sys.exit(1)
    # the census of the capture's own constants (independent of the planner)
    w = census_weights(*CAPS['bryan'])
    say(len(w) == 91 and all(x == {0.0} for x in w), 'census bryan: mask c6.w == 0 in %d of %d frames' % (sum(1 for x in w if x == {0.0}), len(w)))
    w = census_weights(*CAPS['brock'])
    say(len(w) == 120 and all(x is not None and min(x) > 0.0 for x in w), 'census brock: mask c6.w > 0 in %d of %d frames (min %.4f)' % (sum(1 for x in w if x and min(x) > 0.0), len(w), min(min(x) for x in w if x)))
    res = {}
    for name, (cap, frames) in CAPS.items():
        off = run(name + '_off', cap, frames, [])
        on = run(name + '_on', cap, frames, ['--skip-blank-plane'])
        if off is None or on is None:
            continue
        res[name] = (off, on)
        eq = off['hashes'] == on['hashes'] and len(off['hashes']) == frames
        say(eq, '%s: %d frames, picture hashes %s with the plane skip on (validation errors %d / %d)' % (name, frames, 'equal' if eq else 'DIFFER', off['errors'], on['errors']))
        say(off['errors'] == 0 and on['errors'] == 0, '%s: validation clean' % name)
        r = on['rep']
        say(r is not None and r['frames'] == frames and r['chains'] == frames, '%s: a regular chain in every frame (%s)' % (name, r))
        if r is None:
            continue
        cut = on['stats']['bound'] and off['stats']['bound'] - on['stats']['bound']
        say(off['stats']['bound'] - on['stats']['bound'] == r['draws'], '%s: bound draws %d -> %d (skipped %d)' % (name, off['stats']['bound'], on['stats']['bound'], r['draws']))
        say(r['unplanned'] == 0 and r['irregular'] == 0 and r['outside'] == 0 and r['declined_overlap'] == 0 and r['declined_taken'] == 0, '%s: no unplanned, irregular or touched-outside frame' % name)
    if 'bryan' in res:
        r = res['bryan'][1]['rep']
        say(r['skipped'] == 90 and r['first_run'] == 1 and r['live'] == 0, 'bryan: skipped %d of 91 (the first runs once: the ledger), live %d' % (r['skipped'], r['live']))
        say((r['targets'], r['clears'], r['draws'], r['copies']) == (4 * 90, 3 * 90, 35 * 90, 4 * 90), 'bryan: per skipped frame 4 targets, 3 clears, 35 draws (32 mask + downscale + 2 blurs), 4 copies: %s' % r)
    if 'brock' in res:
        r = res['brock'][1]['rep']
        say(r['skipped'] == 0 and r['first_run'] == 0 and r['live'] == 120, 'brock: the rule says not blank in %d of 120 frames, nothing skipped, nothing counted as run once' % r['live'])
    # extra arms on Bryan: each compared with the same arm WITHOUT the plane skip
    if 'bryan' in res:
        cap, frames = CAPS['bryan']
        bundle = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']   # F3: the default bundle
        arms = (('bundle', bundle, None, 'the default bundle (--shadows-ours --shadow-casters-ours --plane-shadow-ours)'),
                ('alias63', ['--copy-alias', '2', '--copy-alias-classes', '63'], None, 'F7: --copy-alias 2 --copy-alias-classes 63 (every class)'),
                ('persist', ['--thin', 'arena'], {'D3_SKIP': 'every2', 'D3_SKIP_LASTING': 'prune'}, 'F6: D3_SKIP=every2 D3_SKIP_LASTING=prune (persist-only binds)'))
        for tag, flags, env, what in arms:
            a = run('bryan_%s_off' % tag, cap, frames, flags, env)
            b = run('bryan_%s_on' % tag, cap, frames, flags + ['--skip-blank-plane'], env)
            if a is None or b is None:
                continue
            r = b['rep']
            say(a['hashes'] == b['hashes'] and len(a['hashes']) == frames and a['errors'] == 0 and b['errors'] == 0, 'bryan %s: hashes %s with the skip on, validation %d / %d' % (what, 'equal' if a['hashes'] == b['hashes'] else 'DIFFER', a['errors'], b['errors']))
            if tag == 'bundle':
                say(r is not None and r['skipped'] == 90 and r['first_run'] == 1, 'bryan bundle: skipped %s (90 required)' % (r and r['skipped']))
                # S102 main (04:45): the hook sees what reaches it per chain frame (34 with a scene record; 2, the blurs, when the capture's mask
                # draws are "not ready": the gate's Bryan case). So the fall is per-frame x skipped, with the per-frame count read from the off arm.
                per = (a['hook'] // r['chains']) if (a['hook'] is not None and r and r['chains']) else 0
                say(a['hook'] is not None and r is not None and per >= 1 and a['hook'] == per * r['chains'] and a['hook'] - b['hook'] == per * r['skipped'],
                    'bryan bundle: the plane hook sees %s draws off (%s a chain frame), %s on: falls by %s = %s x %s skipped' % (a['hook'], per, b['hook'], a['hook'] - b['hook'], per, r and r['skipped']))
            elif tag == 'alias63':
                say(r is not None and r['skipped'] == 90, 'bryan alias63: skipped %s (90 required: the chain images are excluded from the alias rule)' % (r and r['skipped']))
            else:
                print('  bryan persist: planner census %s' % r)
    # planted twins on Check B
    if 'brock' in res:
        cap, frames = CAPS['brock']
        base = res['brock'][0]['hashes']
        t = run('brock_always', cap, frames, ['--skip-blank-plane', '--skip-blank-plane-fault', 'always'])
        if t is not None:
            diff = [i for i in range(frames) if t['hashes'][i] != base[i]]
            # the pictures differ from the first frame the shadow is on screen to the end of the capture; before it the live chain draws nothing visible
            onset = diff[0] if diff else frames
            say(len(diff) >= 1 and diff == list(range(onset, frames)) and onset < frames // 2, 'twin always (rule says blank on Check B): CAUGHT, pictures differ on every frame from %d to %d (%d of %d)' % (onset, frames - 1, len(diff), frames))
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
        import shadow_census as sc
        gcap = sc.Cap(os.path.join(MAIN, 'out', cap))
        glo, ghi = int(gcap.c.frame(70).guest_frame), int(gcap.c.frame(80).guest_frame)   # the window is in GUEST frames (F15)
        t = run('brock_window', cap, frames, ['--skip-blank-plane', '--skip-blank-plane-fault', 'window', '--skip-blank-plane-window', str(glo), str(ghi)])
        if t is not None:
            diff = [i for i in range(frames) if t['hashes'][i] != base[i]]
            say(len(diff) >= 1 and all(70 <= i < 80 for i in diff), 'twin window guest frames [%d,%d) = capture frames [70,80): CAUGHT, pictures differ on %s and nowhere else (a wrong skip does not outlive itself)' % (glo, ghi, diff))
    # the planner's own unit test (synthetic frames): every ledger rule, and its two planted faults must fail it
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    exe = os.path.join(OUT, 'plane_blank_test.exe')
    b = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-I', os.path.join(ROOT, 'src'), '-I', os.path.join(ROOT, 'src', 'renderer_adapter'),
                        os.path.join(ROOT, 'src', 'renderer_adapter', 'plane_blank_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print(b.stdout + b.stderr)
        say(False, 'plane_blank_test.cpp did not compile')
    else:
        u = subprocess.run([exe], capture_output=True, text=True)
        say(u.returncode == 0, 'unit test (first run, skip, live clears the ledger, overlap, taken, switch-off, persist-only, no-chain writes, last clear, stray op, isolate, zero bank): %s' % u.stdout.strip().splitlines()[-1])
        for plant in ('plant_no_first_run', 'plant_ignore_overlap'):
            u = subprocess.run([exe, plant], capture_output=True, text=True)
            say(u.returncode != 0, 'unit test twin %s: CAUGHT (%d failing checks)' % (plant, u.stdout.count('FAIL ')))
    print('RESULT', 'FAIL' if FAILS else 'PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
