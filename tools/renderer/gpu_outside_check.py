r"""S102 lane G5 (the GPU load outside our command buffers; docs\renderer\GPU_OUTSIDE.md): offline proof, 0 game runs, no window.
  py -I tools\renderer\gpu_outside_check.py [--root DIR] [--no-fview] [--only rplay|fview]

PART A, rplay on the Bryan entrance capture of the MAIN checkout (out\lane86\bryan\capture, 91 frames), `--gpu-stamps 1 --gpu-outside 1` unless said:
  base    : the meter on;
  off     : `--gpu-outside 0` (the default): the CPU side still counts, the GPU side reads 0 samples;
  plant6  : `--gpu-plant 6`, an EXTRA queue submit of its own small buffer (a slow copy loop) before every frame's buffer = GPU work OUTSIDE the stamped buffer: the line's "outside" must rise
            (> 3 ms) while the stamped bracket stays within 0.3 ms of base;
  plant5  : `--gpu-plant 5`, the same slow loop INSIDE the buffer (the last thing recorded): the stamped bracket must rise > 3 ms and "outside" must NOT (< 1 ms): the meter tells the two places apart;
  batch   : `--upload-batch 1`: the upload barriers a frame fall by at least 15, batched frames > 50 %, the same uploads, the uploads' GPU time no worse than base + 0.03 ms.
  Twins (the verdict functions must REJECT a wrong claim): base against base for plant6's rise; plant5 against the plant6 expectation.
PART B, fview on two captures (Bryan's entrance, a match; the same case zips as tools\crowd\members_instanced_check.py), replay with `--hash-frames`, `--upload-batch off` against `on`: every
  frame's presented-surface hash must be equal (the batch changes the barriers, never the picture). Twin: one hash of the batch arm changed by hand must be reported different.
Exit 0 all PASS, 1 any FAIL (3 attempts for GPU timing noise in part A). A check that cannot read its input fails loudly."""
import argparse, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
POINTS = ['fence wait', 'drain retired', 'destroys', 'pool resets', 'gpu results read', 'pipeline store', 'ring copy', 'begin + reset', 'uploads (cpu)', 'compile', 'texture sets',
          'framebuffers', 'pipelines built', 'record', 'end buffer', 'submit call', 'acquire', 'present call']
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
OUT_RE = re.compile(r'\[rplay\]\[outside\] (\d+) frames, CPU ms a frame outside the buffers: (.*?) \| GPU outside the stamps: submit-to-signal wall ([\d.]+) - stamped ([\d.]+) = outside ([\d.]+) ms a frame \((\d+) samples\); '
                    r'uploads on the GPU ([\d.]+) ms a frame \((\d+) samples\); a frame: ([\d.]+) uploads \(([\d.]+) scaled, (\d+) KB\), ([\d.]+) upload barriers, ([\d.]+) pipelines built, batched on (\d+)% of frames')
BUSY_RE = re.compile(r'rdr_submit busy ([\d.]+) ms a render')
RPLAY_VAL_RE = re.compile(r'\[rplay\] device .*?validation errors (\d+)')
FVIEW_VAL_RE = re.compile(r'\[fview\] device .*?, validation errors (\d+)')
FVIEW_BATCH_RE = re.compile(r'\[fview\] upload batch: (on|off), batched frames (\d+), duplicate fallbacks (\d+), frames with more than one image upload (\d+)')
ROLES_TOTAL_RE = re.compile(r'\[rplay\]\[gpu\] roles: frames (\d+),.*? work ([\d.]+) gap ([\d.]+) total ([\d.]+);')


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def parse_out(text, rc=0):
    m = OUT_RE.search(text)
    if not m:
        return None
    g = m.groups()
    cpu = {}
    for part in g[1].split(','):
        part = part.strip()
        if not part:
            continue
        name, _, val = part.rpartition(' ')
        cpu[name] = float(val)
    d = {'frames': int(g[0]), 'cpu': cpu, 'wall': float(g[2]), 'stamped': float(g[3]), 'outside': float(g[4]), 'samples': int(g[5]), 'up_gpu': float(g[6]), 'up_gpu_n': int(g[7]),
         'uploads': float(g[8]), 'scaled': float(g[9]), 'kb': int(g[10]), 'up_barriers': float(g[11]), 'pipes': float(g[12]), 'batched': int(g[13])}
    r = ROLES_TOTAL_RE.search(text)
    d['bracket'] = float(r.group(4)) if r else 0.0
    b = BUSY_RE.search(text)
    d['busy'] = float(b.group(1)) if b else -1.0
    v = RPLAY_VAL_RE.search(text)
    d['val'] = int(v.group(1)) if v else -1
    d['rc'] = rc
    return d


def run_rplay(exe, capture, extra, tag, tmp):
    cmd = [exe, '--capture', capture, '--frames', '91', '--gpu-log', os.path.join(tmp, tag + '.csv'), '--out', os.path.join(tmp, tag + '.png'), '--gpu-stamps', '1'] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    d = parse_out(p.stdout + p.stderr, p.returncode)
    if d is None:
        print((p.stdout + p.stderr)[-2500:])
        raise RuntimeError('arm %s printed no [rplay][outside] line (exit %d)' % (tag, p.returncode))
    d['tag'] = tag
    return d


def verdict_plant_outside(base, arm):
    """GPU work planted OUTSIDE the stamped buffer: outside rises > 3 ms, the stamped bracket stays within 0.3 ms. -> problems (empty = shown as outside)."""
    problems = []
    if not arm['outside'] - base['outside'] > 3.0:
        problems.append('outside rose only %+.3f ms (must be > 3)' % (arm['outside'] - base['outside']))
    if abs(arm['stamped'] - base['stamped']) > 0.3:
        problems.append('the stamped bracket moved %+.3f ms (must stay within 0.3: the plant is not in the buffer)' % (arm['stamped'] - base['stamped']))
    return problems


def verdict_plant_inside(base, arm):
    """The same slow work INSIDE the buffer: the stamped bracket rises > 3 ms and outside does NOT (< 1 ms). -> problems."""
    problems = []
    if not arm['stamped'] - base['stamped'] > 3.0:
        problems.append('the stamped bracket rose only %+.3f ms (must be > 3)' % (arm['stamped'] - base['stamped']))
    if arm['outside'] - base['outside'] > 1.0:
        problems.append('outside rose %+.3f ms for work inside the buffer (must stay < 1)' % (arm['outside'] - base['outside']))
    return problems


def verdict_batch(base, arm):
    problems = []
    if base['uploads'] != arm['uploads']:
        problems.append('uploads a frame differ: %.1f vs %.1f' % (base['uploads'], arm['uploads']))
    if not base['up_barriers'] - arm['up_barriers'] >= 15.0:
        problems.append('upload barriers a frame fell only %.1f -> %.1f (must fall by at least 15)' % (base['up_barriers'], arm['up_barriers']))
    if not arm['batched'] > 50 or base['batched'] != 0:
        problems.append('batched frames %d%% (switch on) / %d%% (off): must be > 50 / 0' % (arm['batched'], base['batched']))
    if arm['up_gpu'] > base['up_gpu'] + 0.03:
        problems.append('the uploads on the GPU got slower: %.3f -> %.3f ms a frame' % (base['up_gpu'], arm['up_gpu']))
    return problems


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                a, b = line.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


def differing(a, b):
    return [k for k in sorted(set(a) | set(b), key=lambda x: (isinstance(x, str), x)) if a.get(k) != b.get(k)]


def fview_info(rc, text):
    v = FVIEW_VAL_RE.search(text)
    b = FVIEW_BATCH_RE.search(text)
    return {'rc': rc, 'val': int(v.group(1)) if v else -1, 'batched': int(b.group(2)) if b else -1, 'fallbacks': int(b.group(3)) if b else -1, 'multi': int(b.group(4)) if b else -1}


def fview(exe, cmd, timeout=3000):
    text = ''
    for attempt in range(12):   # the GPU is shared: retry only the logged start-up refusal
        p = subprocess.run([exe] + cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' not in text:
            return p.returncode, text
        print('NOTE  "Not enough graphics memory for the frame ring", attempt %d of 12' % (attempt + 1), flush=True)
        time.sleep(20)
    return 1, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-fview', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--exe', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1

    if args.only in ('', 'rplay'):
        exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
        capture = os.path.join(main_co, 'out', 'lane86', 'bryan', 'capture')
        for need in (exe, capture):
            if not os.path.exists(need):
                print('FAIL  input missing: ' + need)
                return 1
        tmp = os.path.join(root, 'out', 'g5', 'check')
        os.makedirs(tmp, exist_ok=True)
        ok = False
        for attempt in range(3):
            arms = {}
            for name, extra in (('base', ['--gpu-outside', '1']), ('off', ['--gpu-outside', '0']), ('plant6', ['--gpu-outside', '1', '--gpu-plant', '6']),
                                ('plant5', ['--gpu-outside', '1', '--gpu-plant', '5']), ('batch', ['--gpu-outside', '1', '--upload-batch', '1']), ('base_val', ['--gpu-outside', '1', '--validation', '1']),
                                ('batch_val', ['--gpu-outside', '1', '--upload-batch', '1', '--validation', '1']), ('off2', ['--gpu-outside', '0']), ('on2', ['--gpu-outside', '1']),
                                ('off3', ['--gpu-outside', '0']), ('on3', ['--gpu-outside', '1']), ('plant7', ['--gpu-outside', '1', '--gpu-plant', '7']), ('plant7b', ['--gpu-outside', '1', '--gpu-plant', '7']),
                                ('plant7c', ['--gpu-outside', '1', '--gpu-plant', '7'])):
                arms[name] = run_rplay(exe, capture, extra, name, tmp)
            b = arms['base']
            fmt_ok = all(p in b['cpu'] for p in POINTS) and len(b['cpu']) == len(POINTS)
            problems = []
            if not fmt_ok:
                problems.append('the line does not carry exactly the %d points: %s' % (len(POINTS), sorted(b['cpu'])))
            if b['samples'] < 60 or b['frames'] != 91:
                problems.append('base: %d GPU samples over %d frames (need >= 60 over 91)' % (b['samples'], b['frames']))
            if arms['off']['samples'] != 0 or arms['off']['frames'] != 91 or not arms['off']['cpu'].get('record', 0) > 0:
                problems.append('meter off: must count the CPU side (91 frames, record > 0) and 0 GPU samples, got %d frames %d samples' % (arms['off']['frames'], arms['off']['samples']))
            problems += ['plant6: ' + p for p in verdict_plant_outside(b, arms['plant6'])]
            problems += ['plant5: ' + p for p in verdict_plant_inside(b, arms['plant5'])]
            problems += ['batch: ' + p for p in verdict_batch(b, arms['batch'])]
            if arms['batch_val']['rc'] != arms['base_val']['rc'] or arms['base_val']['rc'] not in (0, 4):  # review F1: the validation layer (exit 4 = errors) with the batch must equal the base's
                problems.append('batch: exit code with validation %d vs base %d (must be equal)' % (arms['batch_val']['rc'], arms['base_val']['rc']))
            # review F3: the meter's own fence wait is idle time, not submit-busy time. Smallest of three runs a side (CPU noise is as big as the wait); plant 7 leaves the wait in the busy time.
            busy_on = min(arms[k]['busy'] for k in ('base', 'on2', 'on3'))
            busy_off = min(arms[k]['busy'] for k in ('off', 'off2', 'off3'))
            busy_p7 = min(arms[k]['busy'] for k in ('plant7', 'plant7b', 'plant7c'))
            if min(busy_on, busy_off, busy_p7) < 0 or abs(busy_on - busy_off) > 0.5:
                problems.append('rdr_submit busy: meter on %.3f vs off %.3f ms a render (must stay within 0.5: the meter wait counts as idle)' % (busy_on, busy_off))
            if not busy_p7 - busy_off > 0.5:
                problems.append('plant 7 (the meter wait left in the busy time): busy %.3f vs off %.3f: not shown (must rise > 0.5)' % (busy_p7, busy_off))
            twin_a = verdict_plant_outside(b, b)                  # base against itself: nothing was planted, the verdict must say so
            twin_b = verdict_plant_outside(b, arms['plant5'])     # the in-buffer plant judged as an outside one: must be rejected
            twin_c = verdict_batch(b, b)                          # batch against itself: no barrier fell
            if not twin_a:
                problems.append('twin: base against base was accepted as an outside plant')
            if not twin_b:
                problems.append('twin: the in-buffer plant was accepted as outside work')
            if not twin_c:
                problems.append('twin: base against base was accepted as a batch win')
            print('      attempt %d: base outside %.3f ms (wall %.3f - stamped %.3f), plant6 outside %.3f / stamped %.3f, plant5 outside %.3f / stamped %.3f, uploads %.1f a frame, '
                  'barriers %.1f -> %.1f batched, uploads on the GPU %.3f -> %.3f ms, bracket %.3f -> %.3f ms' % (
                      attempt + 1, b['outside'], b['wall'], b['stamped'], arms['plant6']['outside'], arms['plant6']['stamped'], arms['plant5']['outside'], arms['plant5']['stamped'],
                      b['uploads'], b['up_barriers'], arms['batch']['up_barriers'], b['up_gpu'], arms['batch']['up_gpu'], b['bracket'], arms['batch']['bracket']), flush=True)
            if not problems:
                ok = True
                break
            for p in problems:
                print('      attempt %d: %s' % (attempt + 1, p), flush=True)
        line(ok, 'rplay (Bryan): the line names all %d points; the extra submit shows as OUTSIDE, the in-buffer loop does not; the batch drops the barriers; the twins are rejected' % len(POINTS))

    if args.only in ('', 'fview') and not args.no_fview:
        sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
        import fview_exe as fe
        exe = fe.find_or_exit(prefer=args.exe, root=root, kind='dev')
        game = os.path.join(main_co, 'game_data', 'extracted')
        work = os.path.join(root, 'out', 'g5', 'fview')
        os.makedirs(work, exist_ok=True)
        for name, rel in CAPS:
            zip_path = os.path.join(main_co, rel)
            for need in (zip_path, game):
                if not os.path.exists(need):
                    print('FAIL  input missing: ' + need)
                    return 1
            hs, info = {}, {}
            for arm in ('off', 'on'):
                csv = os.path.join(work, '%s_%s.csv' % (name, arm))
                if os.path.exists(csv):
                    os.remove(csv)
                rc, text = fview(exe, ['--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game, '--hash-frames', csv, '--upload-batch', arm])
                if not os.path.isfile(csv):
                    print(text[-2500:])
                    line(False, 'fview %s %s: no hashes written (exit %d)' % (name, arm, rc))
                    break
                hs[arm] = read_hashes(csv)
                info[arm] = fview_info(rc, text)
            if len(hs) < 2:
                continue
            frames = len([k for k in hs['off'] if k != 'format'])
            diff = differing(hs['off'], hs['on'])
            line(frames > 50 and not diff, 'fview %s: %d frames, batch on vs off: %d differ%s' % (name, frames, len(diff), '' if not diff else ' (first %s)' % diff[:5]))
            tampered = dict(hs['on'])
            k = [x for x in tampered if x != 'format'][len(tampered) // 2]
            tampered[k] = '0000000000000000' if tampered[k] != '0000000000000000' else '1'
            line(differing(hs['off'], tampered) == [k], 'fview %s twin: one hash of the batch arm changed by hand is reported different (frame %s)' % (name, k))
            off, on = info['off'], info['on']  # review F1 / F2: the validation layer's count (sync validation on) and whether the batch ran at all
            line(off['val'] >= 0 and on['val'] >= 0 and off['val'] == on['val'] and off['rc'] == on['rc'] and on['rc'] in (0, 4),
                 'fview %s: validation errors off %d / on %d, exit codes %d / %d (must be equal)' % (name, off['val'], on['val'], off['rc'], on['rc']))
            line(on['batched'] > 0 and off['batched'] == 0 and on['multi'] > 0,
                 'fview %s: batched frames on %d (of %d with more than one image upload) / off %d (must be > 0 / 0); duplicate fallbacks %d' % (name, on['batched'], on['multi'], off['batched'], on['fallbacks']))
            if name == 'bryan':   # the planted faults of the batch: each must break the validation count (a proof blind to a missing barrier or a missing fallback proves nothing)
                for plant, what, must_fail in ((1, 'the batched groups\' last barrier dropped', True), (2, 'a duplicate upload with the fallback off', True), (3, 'the same duplicate with the fallback ON (control)', False)):
                    rc, text = fview(exe, ['--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game, '--upload-batch', 'on', '--upload-batch-plant', str(plant)])
                    pi = fview_info(rc, text)
                    broke = pi['val'] > on['val'] or pi['rc'] != on['rc']
                    line(broke == must_fail, 'fview bryan plant %d (%s): validation errors %d (clean arm %d), exit %d: %s' % (plant, what, pi['val'], on['val'], pi['rc'], 'caught' if broke else 'clean'))
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
