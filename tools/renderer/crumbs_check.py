r"""S102 lane D1 (docs\renderer\DEVICE_LOST.md): the lost-device breadcrumbs and present accounting, proved offline with the replay player. 0 game runs.
  py -I tools\renderer\crumbs_check.py [--root DIR] [--capture DIR]
Runs this checkout's rplay (out\renderer\dev\rplay.exe) on the Bryan entrance capture of the MAIN checkout (out\lane86\bryan\capture), 30 frames, an offscreen
present (--present-size 1280x720) so presents are counted as in the game:
  selftest     --crumbs-selftest 20:10 --gpu-marker 3: the renderer stalls the GPU at step 10 of frame 20 (a VkEvent), waits for the marker word, runs the
               lost-device handler as if lost. Its own line must say PASS (the decode names step 10 of frame 20; the ring's 161 steps match the plan), the dump
               must hold the loud [rdr][DEVICE_LOST] line, a VERDICT naming frame 20 and the step, and NO present may count after the loss (the bug this lane
               fixes: a lost device's frames counted as presented, benches read ~60 fps): presents at the loss == presents at the end.
  class runs   the same with --gpu-marker 1 (the first step of each GPU-class run): the stall lands on the first marked step >= 10 and must decode exactly.
  bundle       the selftest under the shipped default bundle (--shadows-ours ... the switches wwe2k14_app.h turns on) on lane 92's case capture (it has scene records)
               with the player's game files (--game): the ring still matches the plan.
  plants       (each must FAIL, or the check is blind): 1 the stalled step's crumb names the next marker word (the decode must miss the step), 2 one step's
               crumb is left out (the ring must not match the plan), 3 presents are counted after the loss (the old accounting: the end count must exceed the
               loss count), 4 the END words without their barriers (the first version's marker stream: the decode must miss the step; review F1).
               The selftest records exactly the production marker stream (no flush of its own).
  fatal        --crumbs-fatal: the dev build's stop on a Problem stays on; the dump must be complete before it (review F4).
  no change    --validation 1, crumbs on + markers every step vs crumbs off + markers off vs the shipped default (crumbs on, markers off) vs --robust-buffers
               (the job C switch: the device with robustBufferAccess, its log line must say ON): the same picture
               (PNG bytes), 0 validation messages, exit code 0 in all three. (The selftest arms run WITHOUT validation: a host-set event waited on by a
               pending command buffer is invalid usage under today's spec; that path exists only in this test.)
Exit 1 on any failure."""
import argparse, hashlib, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--post-ours', '--small-passes-ours', '--rim-ours', '--rim-chain-ours', '--circles-ours', '--movie-ours', '--screens-ours']
SELF_RE = re.compile(r'\[rdr\]\[crumbs\]\[selftest\] frame (\d+) stalled at step (\d+) \(marker (\d+)\); decoded: slot (\d+) frame word ([0-9a-f]+), marker reached (-?\d+), step (-?\d+); '
                     r'ring: steps recorded (\d+), crumbs (\d+), mismatched (\d+); (PASS|FAIL)')
PRES_RE = re.compile(r'\[rplay\]\[crumbs\] presents: (\d+) at the loss, (\d+) at the end; device lost at serial (\d+)')


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def run(exe, capture, extra, tag, tmp):
    png = os.path.join(tmp, tag + '.png')
    cmd = [exe, '--capture', capture, '--frames', '30', '--present-size', '1280x720', '--out', png] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = p.stdout + p.stderr
    with open(os.path.join(tmp, tag + '.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    d = {'rc': p.returncode, 'text': text, 'png': png}
    m = SELF_RE.search(text)
    if m:
        g = m.groups()
        d['self'] = {'frame': int(g[0]), 'stall': int(g[1]), 'marker': int(g[2]), 'reached': int(g[5]), 'step': int(g[6]), 'recorded': int(g[7]), 'crumbs': int(g[8]),
                     'mismatched': int(g[9]), 'verdict': g[10]}
    m = PRES_RE.search(text)
    if m:
        d['pres'] = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    d['validation'] = len(re.findall(r'\[rdr\]\[validation\]', text))
    return d


def png_hash(path):
    try:
        return hashlib.sha1(open(path, 'rb').read()).hexdigest()
    except OSError:
        return None


def check_selftest(d, label, want_stall_min=10):
    s = d.get('self')
    if not s:
        say(False, '%s: no [rdr][crumbs][selftest] line (rc %d): the selftest did not run' % (label, d['rc']))
        return
    say(d['rc'] == 0, '%s: rplay exit code %d' % (label, d['rc']))
    say(s['verdict'] == 'PASS' and s['step'] == s['stall'] and s['stall'] >= want_stall_min and s['frame'] == 20,
        '%s: the decode names the stalled step: frame %d stalled at step %d (marker %d), decoded marker %d = step %d' % (label, s['frame'], s['stall'], s['marker'], s['reached'], s['step']))
    say(s['mismatched'] == 0 and s['crumbs'] == s['recorded'] and s['recorded'] > 50,
        '%s: the ring matches the plan: %d steps recorded, %d crumbs, %d mismatched' % (label, s['recorded'], s['crumbs'], s['mismatched']))
    t = d['text']
    say('[rdr][DEVICE_LOST] =====' in t and '[rdr][PROBLEM] device_lost:' in t, '%s: the loud line and the device_lost Problem are logged' % label)
    v = re.search(r'\[rdr\]\[crumbs\]   VERDICT frame (\d+) step (\d+):', t)
    say(bool(v) and int(v.group(1)) == 20 and int(v.group(2)) == s['stall'], '%s: the dump\'s VERDICT names frame 20 step %d (%s)' % (label, s['stall'], v.group(0) if v else 'none'))
    p = d.get('pres')
    say(bool(p) and p[0] == p[1] and p[0] > 0 and p[2] == 21, '%s: no present counts after the loss (presents at the loss %s, at the end %s, lost at serial %s)' %
        (label, p[0] if p else '?', p[1] if p else '?', p[2] if p else '?'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--capture', default=None)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    main_co = main_checkout(root)
    capture = a.capture or os.path.join(main_co, 'out', 'lane86', 'bryan', 'capture')
    bundle_capture = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isfile(exe):
        say(False, 'no rplay at %s (build: pwsh -NoProfile -File tools\\renderer\\build.ps1)' % exe)
        return 1
    if not os.path.isdir(capture):
        say(False, 'no capture at %s' % capture)
        return 1
    with tempfile.TemporaryDirectory(prefix='d1_crumbs_') as tmp:
        st = ['--crumbs-selftest', '20:10']
        check_selftest(run(exe, capture, st + ['--gpu-marker', '3'], 'selftest', tmp), 'selftest (every step)')
        check_selftest(run(exe, capture, st + ['--gpu-marker', '1'], 'classruns', tmp), 'class runs (marker mode 1)')
        # (the bundle needs a capture with scene records and the player's game files: lane 92's case capture, as colour_pass_rank.py uses)
        if not os.path.isdir(bundle_capture) or not os.path.isdir(game):
            say(False, 'default bundle: no capture %s or no game files %s' % (bundle_capture, game))
        else:
            check_selftest(run(exe, bundle_capture, st + ['--gpu-marker', '3', '--game', game] + BUNDLE, 'bundle', tmp), 'default bundle (lane 92 capture)')
        # The planted faults: each must be caught by the same assertions.
        p1 = run(exe, capture, st + ['--gpu-marker', '3', '--crumbs-plant', '1'], 'plant1', tmp)
        s1 = p1.get('self')
        say(bool(s1) and s1['verdict'] == 'FAIL' and s1['step'] != s1['stall'], 'plant 1 (a crumb names the next marker word) is caught: decoded step %s, stalled %s, verdict %s' %
            (s1['step'] if s1 else '?', s1['stall'] if s1 else '?', s1['verdict'] if s1 else 'no line'))
        p2 = run(exe, capture, st + ['--gpu-marker', '3', '--crumbs-plant', '2'], 'plant2', tmp)
        s2 = p2.get('self')
        say(bool(s2) and s2['verdict'] == 'FAIL' and (s2['mismatched'] > 0 or s2['crumbs'] != s2['recorded']), 'plant 2 (a crumb left out) is caught: crumbs %s of %s recorded, mismatched %s' %
            (s2['crumbs'] if s2 else '?', s2['recorded'] if s2 else '?', s2['mismatched'] if s2 else '?'))
        p3 = run(exe, capture, st + ['--gpu-marker', '3', '--crumbs-plant', '3'], 'plant3', tmp)
        pr = p3.get('pres')
        say(bool(pr) and pr[1] > pr[0], 'plant 3 (presents counted after the loss, the old accounting) is caught: %s at the loss, %s at the end' % (pr[0] if pr else '?', pr[1] if pr else '?'))
        p4 = run(exe, capture, st + ['--gpu-marker', '3', '--crumbs-plant', '4'], 'plant4', tmp)
        s4 = p4.get('self')
        say(bool(s4) and s4['verdict'] == 'FAIL', 'plant 4 (END words without their barriers: the first version\'s stream) is caught: decoded step %s, stalled %s, verdict %s' %
            (s4['step'] if s4 else '?', s4['stall'] if s4 else '?', s4['verdict'] if s4 else 'no line'))
        # Review F4: a dev build stops on a Problem; the dump must be out before it (no --crumbs-selftest leniency: --crumbs-fatal keeps the stop on).
        pf = run(exe, capture, st + ['--gpu-marker', '3', '--crumbs-fatal'], 'fatal', tmp)
        say('[rdr][crumbs] end of the dump' in pf['text'] and '[rdr][PROBLEM] device_lost:' in pf['text'] and pf['rc'] != 0,
            'a RDR_DEV build that stops on the device_lost Problem has the whole dump out first (rc %d: the stop, as built)' % pf['rc'])
        # The PC route's GPU-assisted validation (rplay --validation 3) must see a vertex fetch past its buffer: plant 5 makes one a frame (DEVICE_LOST.md section 9).
        p5 = run(exe, capture, ['--validation', '3', '--crumbs-plant', '5'], 'plant5', tmp)
        say(p5['rc'] != 0 and 'has caused an OOB access within a bound vertex buffer' in p5['text'],
            'plant 5 (a vertex fetch past its buffer) is flagged by GPU-assisted validation (rc %d): the PC route\'s index / vertex bounds check has teeth' % p5['rc'])
        # No change to the picture, no validation message, with the markers on every step, off, and the shipped default.
        arms = [('on', ['--crumbs', '1', '--gpu-marker', '3']), ('off', ['--crumbs', '0', '--gpu-marker', '0']), ('default', []), ('robust', ['--robust-buffers'])]
        hashes = {}
        for tag, extra in arms:
            d = run(exe, capture, extra + ['--validation', '1'], 'nc_' + tag, tmp)
            hashes[tag] = png_hash(d['png'])
            say(d['rc'] == 0 and d['validation'] == 0 and hashes[tag] is not None, 'no change (%s): exit code %d, %d validation messages, picture %s' %
                (tag, d['rc'], d['validation'], (hashes[tag] or 'MISSING')[:12]))
            say('device_lost' not in d['text'] and '[rdr][DEVICE_LOST]' not in d['text'], 'no change (%s): no lost-device line in a normal run' % tag)
            if tag == 'robust':
                say('[rdr] robustBufferAccess ON' in d['text'], 'robust: the device was made with robustBufferAccess (the diagnosis switch of job C works)')
        say(len(set(hashes.values())) == 1 and None not in hashes.values(), 'the picture is the same with the breadcrumbs and markers on, off and at the default: %s' %
            ', '.join('%s %s' % (k, (v or '-')[:12]) for k, v in hashes.items()))
    print('crumbs_check: %s (%d failure%s)' % ('PASS' if not FAILS else 'FAIL', len(FAILS), '' if len(FAILS) == 1 else 's'), flush=True)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
