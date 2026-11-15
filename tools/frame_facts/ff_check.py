r"""S102 lane R21-A (frame facts once): the lane's offline checks.   py -I tools\frame_facts\ff_check.py [--root DIR] [--only unit|fview|rplay] [--smoke]
unit   tools\frame_facts\record_ids_test.cpp: the new arena-id parse against the old sscanf code over 528 ids; plants 1 (short prefix) and 2 (twin tail ignored) must be seen.
fview  Bryan's entrance and a Brock match (case zips), the player's bundle (--arena-ours ... --shadows-ours), --validation 1, --frame-facts off against on --frame-facts-verify:
       every frame's picture hash equal, validation errors and exit codes equal, the arena census 'record gaps ...' equal, verified records > 0 with 0 bad, blend draws skipped > 0
       and flat key tables > 0 on (0 off). Plant 2 (the twin tail ignored) must be caught by the verify count AND by `twin missing` rising: the S101 guard, no new code.
rplay  the draw counts of plant 3 (BlendRanges skipped while a consumer is on): kept keys 0 against the control's > 0; the step 5 census (--frame-shape) prints for the Bryan capture.
0 game runs, 0 Surface runs. Exit 0 = every line OK.
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
RPLAY_CAP = 'out/lane86/bryan/capture'
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
VAL_RE = re.compile(r'\[fview\] device .*?, validation errors (\d+)')
FF_RE = re.compile(r'\[fview\] frame facts: (on|off), records (\d+), record objects (\d+) \(arena-prefixed (\d+)\), blend skipped (\d+) draws \(kept (\d+)\), flat key tables (\d+), verified records (\d+) \(bad (\d+)\)')
GAPS_RE = re.compile(r'record gaps: slot missing (\d+), twin missing (\d+), world differs (\d+) \((\d+) slots\)')
RFF_RE = re.compile(r'\[rdr\]\[frame_facts\] (on|off): records (\d+), record objects (\d+) \(arena-prefixed (\d+)\), blend skipped (\d+) draws \(kept (\d+)\), flat key tables (\d+), verified records (\d+) \(bad (\d+)\)')
SHAPE_RE = re.compile(r'\[rplay\]\[frame_shape\] (\S+) (\S+): drawn frames (\d+), input hash = the previous frame\'s ([\d.]+) %, = one of the last 4 ([\d.]+) %')
TP_RE = re.compile(r'\[rplay\]\[tween_probe\] blended frames (\d+), draws matched (\d+)')
PART_RE = re.compile(r'blend keyed (\d+)')
fails = 0


def line(ok, text):
    global fails
    print(('OK    ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        fails += 1


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def read_hashes(path):
    out = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                a, b = ln.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


def differing(a, b):
    return [k for k in sorted(set(a) | set(b), key=lambda x: (isinstance(x, str), x)) if a.get(k) != b.get(k)]


def run(exe, cmd, timeout=3000):
    text = ''
    for attempt in range(12):   # the GPU is shared: retry only the logged start-up refusal
        p = subprocess.run([exe] + cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' not in text:
            return p.returncode, text
        print('NOTE  "Not enough graphics memory for the frame ring", attempt %d of 12' % (attempt + 1), flush=True)
        time.sleep(20)
    return 1, text


def info(rc, text):
    v, f, g = VAL_RE.search(text), FF_RE.search(text), GAPS_RE.search(text)
    d = {'rc': rc, 'val': int(v.group(1)) if v else -1, 'gaps': g.groups() if g else None}
    if f:
        d.update(on=f.group(1) == 'on', records=int(f.group(2)), objects=int(f.group(3)), arena=int(f.group(4)), skipped=int(f.group(5)), kept=int(f.group(6)), flat=int(f.group(7)), verified=int(f.group(8)), bad=int(f.group(9)))
    return d


def unit(root):
    exe = os.path.join(root, 'out', 'frame_facts', 'record_ids_test.exe')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    p = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-D_CRT_SECURE_NO_WARNINGS', os.path.join(root, 'tools', 'frame_facts', 'record_ids_test.cpp'), '-o', exe], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        line(False, 'unit: record_ids_test did not build')
        return
    for arg, what, want in (([], 'the new parse against the old sscanf code', 0), (['--plant1'], 'plant 1 (the prefix test accepts "arena:slo") is seen', 0), (['--plant2'], 'plant 2 (the :r twin tail ignored) is seen', 0)):
        p = subprocess.run([exe] + arg, capture_output=True, text=True)
        line(p.returncode == want and '[record_ids_test]' in p.stdout, 'unit: %s: %s' % (what, p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()[:200]))


def fview_part(root, main_co, smoke):
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 'frame_facts', 'fview')
    os.makedirs(work, exist_ok=True)
    for name, rel in (CAPS[:1] if smoke else CAPS):
        zip_path = os.path.join(main_co, rel)
        for need in (zip_path, game):
            if not os.path.exists(need):
                print('FAIL  input missing: ' + need)
                sys.exit(1)
        base = ['--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game] + BUNDLE + ['--thin', 'arena,ring,wrestlers']   # the thin classes the player's defaults turn on (ComputeKeys runs only with them)
        arms = {}
        for arm, extra in (('off', ['--frame-facts', 'off']), ('on', ['--frame-facts', 'on', '--frame-facts-verify']), ('plant2', ['--frame-facts', 'on', '--frame-facts-verify', '--frame-facts-plant', '2'])):
            if smoke and arm == 'plant2':
                continue
            csv = os.path.join(work, '%s_%s.csv' % (name, arm))
            if os.path.exists(csv):
                os.remove(csv)
            rc, text = run(exe, base + extra + ['--hash-frames', csv])
            if not os.path.isfile(csv):
                print(text[-2500:])
                line(False, 'fview %s %s: no hashes written (exit %d)' % (name, arm, rc))
                break
            arms[arm] = (read_hashes(csv), info(rc, text))
        if 'off' not in arms or 'on' not in arms:
            continue
        (h_off, i_off), (h_on, i_on) = arms['off'], arms['on']
        frames = len([k for k in h_off if k != 'format'])
        diff = differing(h_off, h_on)
        line(frames > 50 and not diff, 'fview %s: %d frames, frame facts on vs off: %d pictures differ%s' % (name, frames, len(diff), '' if not diff else ' (first %s)' % diff[:5]))
        tampered = dict(h_on)
        k = [x for x in tampered if x != 'format'][len(tampered) // 2]
        tampered[k] = '0000000000000000' if tampered[k] != '0000000000000000' else '1'
        line(differing(h_off, tampered) == [k], 'fview %s twin: one hash of the on arm changed by hand is reported different (frame %s)' % (name, k))
        line(i_off['val'] >= 0 and i_off['val'] == i_on['val'] and i_off['rc'] == i_on['rc'] and i_on['rc'] in (0, 4), 'fview %s: validation errors off %d / on %d, exit codes %d / %d (must be equal)' % (name, i_off['val'], i_on['val'], i_off['rc'], i_on['rc']))
        line(i_off['gaps'] is not None and i_off['gaps'] == i_on['gaps'], 'fview %s: the arena census "record gaps" off %s / on %s (must be equal and present)' % (name, i_off['gaps'], i_on['gaps']))
        line(i_on.get('verified', 0) > 0 and i_on.get('bad', 1) == 0 and i_on.get('arena', 0) > 0, 'fview %s: records verified against the old classification %s, bad %s, arena-prefixed objects %s (must be > 0 / 0 / > 0)' % (name, i_on.get('verified'), i_on.get('bad'), i_on.get('arena')))
        line(i_on.get('skipped', 0) > 0 and i_on.get('flat', 0) > 0 and i_on.get('kept', 1) == 0 and i_off.get('skipped', 1) == 0 and i_off.get('flat', 1) == 0,
             'fview %s: the levers took: blend draws skipped %s (kept %s), flat key tables %s on; %s / %s off (must be > 0, 0, > 0 on and 0 off)' % (name, i_on.get('skipped'), i_on.get('kept'), i_on.get('flat'), i_off.get('skipped'), i_off.get('flat')))
        if 'plant2' in arms:
            i_p = arms['plant2'][1]
            gaps_rise = i_p['gaps'] is not None and i_on['gaps'] is not None and int(i_p['gaps'][1]) > int(i_on['gaps'][1])
            line(i_p.get('bad', 0) > 0 and gaps_rise, 'fview %s plant 2 (the :r twin tail ignored): verify bad %s (must be > 0) and "twin missing" %s -> %s (the S101 guard must rise)' % (name, i_p.get('bad'), i_on['gaps'][1] if i_on['gaps'] else None, i_p['gaps'][1] if i_p['gaps'] else None))
            line(bool(differing(h_off, arms['plant2'][0])) or gaps_rise, 'fview %s plant 2: seen by the picture or by the census' % name)


def rplay_part(root, main_co):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    cap = os.path.join(main_co, RPLAY_CAP)
    for need in (exe, cap):
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            sys.exit(1)
    work = os.path.join(root, 'out', 'frame_facts', 'rplay')
    os.makedirs(work, exist_ok=True)
    res = {}
    for tag, extra in (('control', ['--frame-facts', 'on', '--frame-facts-want-keys', '--tween-probe']), ('plant3', ['--frame-facts', 'on', '--frame-facts-want-keys', '--tween-probe', '--frame-facts-plant', '3']),
                       ('unwanted', ['--frame-facts', 'on']), ('off', ['--frame-facts', 'off', '--tween-probe']),
                       ('particles', ['--frame-facts', 'on', '--particles-ours', '--particles-fault', 'tween_runs']), ('particles_off', ['--frame-facts', 'off', '--particles-ours', '--particles-fault', 'tween_runs'])):
        rc, text = run(exe, ['--capture', cap, '--frames', '91', '--validation', '1', '--out', os.path.join(work, tag + '.png'), '--frame-shape', 'bryan'] + extra, timeout=900)
        m = RFF_RE.search(text)
        res[tag] = (rc, m.groups() if m else None, text)
    def kept(t):
        g = res[t][1]
        return (int(g[5]), int(g[4])) if g else (-1, -1)
    line(all(res[t][0] == 0 and res[t][1] for t in res), 'rplay: every arm exits 0 and prints the frame_facts line (%s)' % {t: res[t][0] for t in res})
    line(kept('control')[0] > 0 and kept('control')[1] == 0, 'rplay control (a consumer wants the keys): keys made %d, skipped %d (must be > 0 / 0)' % kept('control'))
    line(kept('plant3')[0] == 0 and kept('plant3')[1] > 0, 'rplay plant 3 (BlendRanges skipped while a consumer is on): keys made %d, skipped %d (the plant must be seen as 0 / > 0 against the control)' % kept('plant3'))
    line(kept('unwanted')[0] == 0 and kept('unwanted')[1] > 0 and kept('off') == (0, 0), 'rplay no consumer: skipped %d draws; switch off: %s (must be 0 / 0)' % (kept('unwanted')[1], kept('off')))
    def matched(t):
        m = TP_RE.search(res[t][2])
        return int(m.group(2)) if m else -1
    line(matched('control') > 0 and matched('off') > 0 and matched('plant3') == 0, 'rplay tween probe (TickBlender Keep f / Blend f + 1): draws matched control %d, switch off %d (must be > 0), plant 3 %d (must be 0: the blend itself sees the plant)' % (matched('control'), matched('off'), matched('plant3')))
    def pk(t):
        m = PART_RE.search(res[t][2])
        return int(m.group(1)) if m else -1
    line(pk('particles_off') > 0 and pk('particles') == pk('particles_off'), 'rplay particles plant tween_runs with frame facts on: blend keyed %d (off %d): the plant still keys merged draws (runs_check keeps seeing it)' % (pk('particles'), pk('particles_off')))
    shapes = SHAPE_RE.findall(res['off'][2])
    line(len(shapes) == 5 and all(int(s[2]) > 50 for s in shapes), 'rplay --frame-shape prints the 5 planner rows over %s drawn frames' % (shapes[0][2] if shapes else '?'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    ap.add_argument('--smoke', action='store_true', help='Bryan only, no plant arm')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    if a.only in ('', 'unit'):
        unit(root)
    if a.only in ('', 'fview'):
        fview_part(root, main_co, a.smoke)
    if a.only in ('', 'rplay'):
        rplay_part(root, main_co)
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
