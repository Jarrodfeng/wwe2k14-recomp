r"""S101 lane 189: self-test of tools\ring\ring_ledger.py (offline, 0 game runs, about 1 minute). Runs the tool on a slice of Check B (run 7051) and of wm30 (run 7084) and asserts:
  clean       exit 0, 108 ring draws a frame, instances 4 / 12 / 12 / 1, the posts' palette rows never move, the ropes' do (Check B), nothing moves in wm30 but the skirts / ropes in bursts
  no_instance exit 1 (the instance-count check fails)           no_join     exit 1 (draws a frame leaves 95..125)
  no_npal_cut exit 1 (the stale bone slots past a mesh's palette make the never-moving posts move: the wrestlers' stale-palette bug, re-found)
  shift_prev  exit 0 but the ropes' palette =prev share MOVES (the plant must change the number it targets)
  missing     exit 1 with FAIL on a capture that does not exist (never an empty ledger)
  py -I tools\ring\ring_ledger_selftest.py [--b DIR] [--wm30 DIR]       exit 0 = all pass.
Captures are looked up under the main checkout's out\ (local game data; skipped with a SKIP line and exit 0 only when BOTH are absent)."""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def run(args, tag):
    out = os.path.join(main_checkout(), 'out', 's101', 'lane189')
    os.makedirs(out, exist_ok=True)
    js = os.path.join(out, 'selftest_%s.json' % tag)
    if os.path.exists(js):
        os.remove(js)
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'ring_ledger.py')] + args + ['--json', js, '-q'], capture_output=True, text=True, timeout=600)
    res = None
    if os.path.isfile(js):
        with open(js, encoding='utf-8') as fh:
            res = json.load(fh)
    return p.returncode, p.stdout + p.stderr, res


def share(res, group, key='held_prev'):
    c = res['groups_total'].get(group, {})
    rd = c.get('reads', 0) - c.get('first_seen', 0)
    return 100.0 * c.get(key, 0) / rd if rd else 0.0


def main():
    ap = argparse.ArgumentParser()
    mc = main_checkout()
    ap.add_argument('--b', default=os.path.join(mc, 'out', 's101', 'lane92', 'case_20261111_215233_217_0_pc_run_exit'))
    ap.add_argument('--wm30', default=os.path.join(mc, 'out', 'lane84', 'wm30r7084'))
    a = ap.parse_args()
    fails = []

    def check(name, ok, why=''):
        print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  (' + why + ')') if why and not ok else ''))
        if not ok:
            fails.append(name)

    have_b, have_w = os.path.isdir(a.b), os.path.isdir(a.wm30)
    if not have_b and not have_w:
        print('SKIP  neither capture is present (%s, %s)' % (a.b, a.wm30))
        return 0
    if have_b:
        rc, text, res = run([a.b, '--arena', '4', '--frames', '0:80', '--expect', 'checkB'], 'b_clean')
        check('b.clean exit 0', rc == 0 and res is not None, text[-300:])
        if res:
            check('b.clean 108 ring draws a frame', abs(res['draws_per_frame'] - 108) < 0.01, str(res['draws_per_frame']))
            pieces = {v['piece']: v['instances_max'] for v in res['objects'].values()}
            check('b.clean instances post 4 / turnbuckle 12 / cover 12 / canvas 1', pieces.get('post') == 4 and pieces.get('turnbuckle') == 12 and pieces.get('cover') == 12 and pieces.get('canvas') == 1, str(pieces))
            post = res['pieces'].get('post', {})
            check('b.clean the posts\' palette rows never move', post.get('pal_draws', 0) > 0 and post.get('pal_held') == post.get('pal_draws'), str(post))
            rope = res['pieces'].get('rope_low', {})
            check('b.clean the low ropes\' rows move in every comparable draw', rope.get('pal_draws', 0) > 0 and rope.get('pal_held', 0) == 0, str(rope))
        base = share(res, 'palette') if res else 0.0
        for plant, want_rc in (('no_instance', 1), ('no_join', 1), ('no_npal_cut', 1)):
            rc, text, r2 = run([a.b, '--arena', '4', '--frames', '0:80', '--expect', 'checkB', '--plant', plant], 'b_' + plant)
            check('b.%s exit %d' % (plant, want_rc), rc == want_rc, text[-300:])
        rc, text, r3 = run([a.b, '--arena', '4', '--frames', '0:80', '--plant', 'shift_prev'], 'b_shift')
        check('b.shift_prev moves the palette =prev share', r3 is not None and abs(share(r3, 'palette') - base) >= 0.5, '%.1f vs %.1f' % (share(r3, 'palette') if r3 else -1, base))
    if have_w:
        rc, text, res = run([a.wm30, '--arena', '29', '--frames', '0:120'], 'w_clean')
        check('wm30.clean exit 0, 108 draws a frame', rc == 0 and res is not None and abs(res['draws_per_frame'] - 108) < 0.01, text[-300:])
        if res:
            post = res['pieces'].get('post', {})
            check('wm30.clean posts and turnbuckles at rest', post.get('pal_draws', 0) > 0 and post.get('pal_held') == post.get('pal_draws') and res['pieces'].get('turnbuckle', {}).get('pal_held') == res['pieces'].get('turnbuckle', {}).get('pal_draws'),
                  str(res['pieces'].get('turnbuckle')))
    rc, text, _ = run([os.path.join(mc, 'out', 'does_not_exist_189'), '--arena', '4'], 'missing')
    check('missing input exits 1 with FAIL, never an empty ledger', rc == 1 and 'FAIL' in text, text[-200:])
    print('ring_ledger selftest: %s' % ('OK' if not fails else 'FAILED (%s)' % ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
