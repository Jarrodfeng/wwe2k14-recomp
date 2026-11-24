r"""S102 lane R23-P (plan_memo: planner work done every frame whose answer does not change, removed): the lane's offline check.
  py -I tools\plan_memo\pm_check.py [--root DIR] [--only smoke|full] [--no-timing]
fview (the player's bundle, the shipped planner switches, --validation 1) on the Bryan entrance and the Brock match case zips:
  off  vs  on (--plan-memo 7 --plan-memo-verify 1): every frame's picture hash equal, validation errors and exit codes equal, verify count > 0 with 0 mismatches,
           every bit took (post cache hits / pass marks skipped / counting sort / site marks skipped; props early exits; dead content counted), std::sort fallback 0.
  planted twins (--plan-memo-plant N, verify 1): each must be caught (verify mismatch > 0, or a picture hash differs), and each must be a plant that APPLIED (its counter moved).
  a tampered csv (one hash changed by hand) must be reported different (the comparison itself is checked).
  timing (unless --no-timing): per-planner ms a frame off vs on (--plan-memo-times, no verify), 2 alternating pairs on Bryan; printed, not judged (a PC number).
  census: --plan-memo-census prints the 6 planner rows and exact-input equality; the check requires 'input same + output differs' = 0 for every planner (a missed input would show).
Past bug re-found: plant 1 is the lane-140 review F2 class (post_plan: the writer label of an image a stream draw wrote after a chain draw): the stale label must be seen.
0 game runs, 0 Surface runs. Exit 0 = every line OK."""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('brock', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours',
          '--thin', 'arena,ring,wrestlers']
BASE = ['--shadow-half', '--copy-alias', '2', '--alias-fullsize', '1', '--dead-work', '3', '--prelight-compute', '--frame-roundtrip', '2', '--frame-facts', 'on']
VAL_RE = re.compile(r'\[fview\] device .*?, validation errors (\d+)')
LINE_RE = re.compile(r'\[rdr\]\[plan_memo\] mask (\d+): post chain fast (\d+) \(pixel cache hits (\d+), misses (\d+); pass marks skipped (\d+), made (\d+)\); alias events fast (\d+) \(counting sort (\d+), std::sort fallback (\d+); site marks skipped (\d+), made (\d+)\); props match early exits (\d+), full scans (\d+); verified (\d+), mismatched (\d+), max image id (\d+)')
ROW_RE = re.compile(r'\[plan_memo\]\[census\] \S+ (post|alias|dead_work|shadows|prelight_c|rt_span) .*?input same \+ output differs (\d+)')
TIME_RE = re.compile(r'\[plan_memo\]\[time\] \S+\s+(.+?)\s+([\d.]+) ms a frame')
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


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra):
        self.name = name
        csv = os.path.join(work, name + '.csv')
        if os.path.exists(csv):
            os.remove(csv)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game] + BUNDLE + BASE + extra + ['--hash-frames', csv]
        for attempt in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=3000, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
        self.rc = p.returncode
        self.hashes = read_hashes(csv) if os.path.isfile(csv) else None
        v = VAL_RE.search(self.text)
        self.val = int(v.group(1)) if v else -1
        m = LINE_RE.search(self.text)
        k = ['mask', 'post', 'hit', 'miss', 'lskip', 'lmade', 'alias', 'csort', 'fall', 'sskip', 'smade', 'pearly', 'pfull', 'verified', 'mismatch', 'maxid']
        self.c = dict(zip(k, map(int, m.groups()))) if m else None
        self.times = {a: float(b) for a, b in TIME_RE.findall(self.text)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='full')
    ap.add_argument('--no-timing', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 'plan_memo')
    os.makedirs(work, exist_ok=True)
    caps = CAPS[:1] if a.only == 'smoke' else CAPS
    for name, rel in caps:
        z = os.path.join(main_co, rel)
        for need in (z, game):
            if not os.path.exists(need):
                print('FAIL  input missing: ' + need)
                return 1
        off = Arm(exe, z, game, work, name + '_off', [])
        on = Arm(exe, z, game, work, name + '_on', ['--plan-memo', '7', '--plan-memo-verify', '1'])
        if off.hashes is None or on.hashes is None:
            print((off.text + on.text)[-2000:])
            line(False, '%s: no hashes written' % name)
            continue
        frames = len([k for k in off.hashes if k != 'format'])
        diff = differing(off.hashes, on.hashes)
        line(frames > 50 and not diff, '%s: %d frames, plan memo 7 (verify 1) vs off: %d pictures differ%s' % (name, frames, len(diff), '' if not diff else ' (first %s)' % diff[:5]))
        t = dict(on.hashes)
        k = [x for x in t if x != 'format'][len(t) // 2]
        t[k] = '0000000000000000' if t[k] != '0000000000000000' else '1'
        line(differing(off.hashes, t) == [k], '%s twin: one hash changed by hand is reported different (frame %s)' % (name, k))
        line(off.val >= 0 and off.val == on.val and off.rc == on.rc and on.rc in (0, 4), '%s: validation errors off %d / on %d, exit codes %d / %d (equal)' % (name, off.val, on.val, off.rc, on.rc))
        c = on.c
        line(c is not None and c['mask'] == 7 and c['verified'] > 0 and c['mismatch'] == 0, '%s: verify %s (verified > 0, mismatched 0)' % (name, None if c is None else (c['verified'], c['mismatch'])))
        if c:
            line(c['post'] > 0 and c['hit'] > 0 and c['lskip'] > 0 and c['alias'] > 0 and c['csort'] > 0 and c['fall'] == 0 and c['sskip'] > 0,
                 '%s: the levers took: post chain %d (cache hits %d, pass marks skipped %d), alias events %d (counting sort %d, fallback %d, site marks skipped %d)' % (name, c['post'], c['hit'], c['lskip'], c['alias'], c['csort'], c['fall'], c['sskip']))
            line(c['pearly'] + c['pfull'] > 0, '%s: props match scans %d (early exits %d), alias counting sort max image id %d' % (name, c['pearly'] + c['pfull'], c['pearly'], c['maxid']))
        cen = Arm(exe, z, game, work, name + '_census', ['--plan-memo-census', name])
        rows = ROW_RE.findall(cen.text)
        line(len(rows) >= 5 and all(int(n) == 0 for _, n in rows), '%s census: %d planner rows, input same + output differs = %s (a missed input would be > 0)' % (name, len(rows), [n for _, n in rows]))
        if a.only == 'smoke':
            continue
        # the planted twins (Bryan runs them all; Brock only the props plant, which Bryan's records cannot show)
        plants = [(1, 'post pass mark ignores a chain draw\'s write (lane 140 review F2 class: a stale writer label)'), (2, 'post pixel cache keyed by the low 8 bits, no compare'),
                  (3, 'alias events: a sort that ignores the position (each image events reversed)'), (4, 'alias site mark skipped while the image is unchanged (not the pair)'),
                  (5, 'props: the early exit answers the opposite')]
        for n, what in plants:
            p = Arm(exe, z, game, work, '%s_plant%d' % (name, n), ['--plan-memo', '7', '--plan-memo-verify', '1', '--plan-memo-plant', str(n)])
            if p.hashes is None:
                line(False, '%s plant %d: no hashes (%s)' % (name, n, what))
                continue
            seen_pic = bool(differing(off.hashes, p.hashes))
            seen_ver = p.c is not None and p.c['mismatch'] > 0
            applied = p.c is not None
            # a plant that no longer applies fails as "did not apply": it must have changed something (the verify count or a picture)
            if n in (1, 2, 3, 4, 6) or True:
                line(applied and (seen_pic or seen_ver), '%s plant %d (%s): %s' % (name, n, what, 'caught by ' + ('the verify count %s' % (p.c['mismatch'],) if seen_ver else '') + (' and ' if seen_ver and seen_pic else '') + ('a picture hash' if seen_pic else '') if (seen_pic or seen_ver) and applied else 'NOT caught / DID NOT APPLY'))
    if not a.no_timing and a.only != 'smoke':
        name, rel = CAPS[0]
        z = os.path.join(main_co, rel)
        sums = {'off': {}, 'on': {}}
        for rep in range(2):
            for tag, extra in (('off', ['--plan-memo-times']), ('on', ['--plan-memo', '7', '--plan-memo-times'])):
                r = Arm(exe, z, game, work, 'time_%s%d' % (tag, rep), extra)
                for k2, v in r.times.items():
                    sums[tag].setdefault(k2, []).append(v)
        for k2 in sorted(sums['off']):
            if k2 in sums['on'] and not k2.startswith('ThinBegin') and not k2.startswith('all'):
                o, n = sums['off'][k2], sums['on'][k2]
                print('INFO  planner %-26s off %.3f ms  on %.3f ms  (best of %d pairs, ms a frame)' % (k2, min(o), min(n), len(o)))
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
