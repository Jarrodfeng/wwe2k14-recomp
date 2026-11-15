r"""S102 lane S0-3 (GPU stamps per run of steps, by role; the start gap apart): offline proof with the replay player.
  py -I tools\renderer\gpu_stamps_check.py [--root DIR] [--capture NAME_UNDER_MAIN]
Runs this checkout's rplay (out\renderer\dev\rplay.exe) on the Bryan entrance capture of the MAIN checkout (out\lane86\bryan\capture, 91 frames). The stamps default OFF in rplay as in the
game, so every arm that needs them says `--gpu-stamps 1`:
  base    : stamps on;
  off     : `--gpu-stamps 0`, the two-timestamp bracket of lane 269 (the cost twin: the roles line shows 0 frames and the bracket still reads);
  empty   : --gpu-plant 3, an EMPTY pass planted once a frame in the PLANE role: plane reads ~0 more, every other role the same;
  slow    : --gpu-plant 4, a SLOW pass planted in the RIM role;
  late    : --gpu-plant 5, a slow copy loop at the END of every frame, no window (does the NEXT frame's start gap [0]-[1] rise? reported);
  mapped  : --gpu-class-plant scene_to_post: the scene's images named as the post chain: the by-class pass counts must show it.
Checks (PASS / FAIL, exit 1 on any FAIL, 3 attempts for GPU noise):
  identity   roles sum = work; work + start gap = the old bracket (the line's own numbers);
  bracket    the stamps do not break the old bracket: stamps on vs `--gpu-stamps 0` within 0.3 ms (the stamps' own drain costs ~0.1 ms a frame);
  roles      colour, shadow, cards and post each carry passes AND time on Bryan (the role mapping works), and at least two roles carry time;
  empty/slow the planted passes show in their own role and nowhere else; twin verdicts against a wrong expectation must be rejected;
  late       plant 5: the end-of-frame slow work shows in `other` and the bracket (> 5 ms); the start gap is reported, not asserted (NVIDIA offline: it stays ~0);
  mapped     with scene_to_post the colour role has no passes (the same assertion that passes on base fails): the mapping check has teeth;
  off        with stamps off the roles line has no frames and `no pass` counts nothing (the default gpu_ms is unchanged: every render is a sample).
"""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLASSES = ['colour', 'shadow', 'rim', 'plane', 'cards', 'post', 'output', 'ui', 'frame', 'copies', 'prelight', 'feedback', 'other']  # S102 G6: the roles line order (tools\renderer\gpu_class_lint.py keeps it equal to renderer.cpp)
ROLES_RE = re.compile(r'\[rplay\]\[gpu\] roles: frames (\d+), mean ms a frame: ' + ' '.join(r'%s ([\d.]+)' % c for c in CLASSES) +
                      r'; work ([\d.]+) gap ([\d.]+) total ([\d.]+); roles sum ([\d.]+); no pass (\d+), no present (\d+), overflow (\d+); stamps a frame ([\d.]+); passes a frame: ' +
                      ' '.join(r'%s ([\d.]+)' % c for c in CLASSES) + r'; acquire ([\d.]+) ms, Present call ([\d.]+) ms a render; rdr_submit busy ([\d.]+) ms a render over (\d+)')
CHECK_RE = re.compile(r'\[rplay\]\[gpu\] check (PASS|FAIL): (\d+) frames rendered, (\d+) collected, (\d+) implausible, (\d+) not ready \(mean ([\d.]+) ms a frame\)')


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def run(exe, capture, extra, tag, tmp):
    log_csv = os.path.join(tmp, tag + '.csv')
    png = os.path.join(tmp, tag + '.png')
    cmd = [exe, '--capture', capture, '--frames', '91', '--gpu-log', log_csv, '--out', png] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = p.stdout + p.stderr
    roles = ROLES_RE.search(text)
    check = CHECK_RE.search(text)
    d = {'rc': p.returncode, 'text': text, 'tag': tag}
    if roles:
        g = roles.groups()
        d['frames'] = int(g[0])
        n = len(CLASSES)
        d['cls'] = dict(zip(CLASSES, map(float, g[1:1 + n])))
        d['work'], d['gap'], d['total'], d['sum'] = map(float, g[1 + n:5 + n])
        d['no_pass'], d['no_present'], d['overflow'] = map(int, g[5 + n:8 + n])
        d['stamps'] = float(g[8 + n])
        d['passes'] = dict(zip(CLASSES, map(float, g[9 + n:9 + 2 * n])))
        d['acquire_ms'], d['present_call_ms'], d['busy_ms'], d['busy_calls'] = float(g[9 + 2 * n]), float(g[10 + 2 * n]), float(g[11 + 2 * n]), int(g[12 + 2 * n])
    if check:
        d['check'] = check.group(1)
        d['rendered'], d['collected'], d['implausible'], d['not_ready'] = map(int, check.groups()[1:5])
        d['plain_mean'] = float(check.group(6))
    return d


def noise_band(base, cls):
    return max(0.25, 0.25 * base['cls'][cls])


def verdict(base, other, kind, in_class):
    """kind 'empty' -> `in_class` must be within 0.05 ms of base; kind 'slow' -> `in_class` must rise > 1.0 ms. Every other class stays within its band.
    -> list of problems (empty = the planted pass shows up in `in_class` and nowhere else)."""
    problems = []
    for c in CLASSES:
        delta = other['cls'][c] - base['cls'][c]
        if c == in_class:
            if kind == 'empty' and not (-0.05 <= delta < 0.05):
                problems.append('%s moved %+.4f ms for an EMPTY pass (must be ~0)' % (c, delta))
            if kind == 'slow' and not delta > 1.0:
                problems.append('%s rose only %+.4f ms for the SLOW pass (must be > 1)' % (c, delta))
        elif abs(delta) > noise_band(base, c):
            problems.append('%s moved %+.4f ms (band %.3f): the planted pass leaked into the wrong role' % (c, delta, noise_band(base, c)))
    return problems


def mapping_problems(r):
    """The role mapping on Bryan: colour, shadow, cards, post each carry passes and time."""
    bad = []
    for c in ('colour', 'shadow', 'cards', 'post'):
        if not r['passes'][c] > 0:
            bad.append('%s has no passes' % c)
        if not r['cls'][c] > 0.0005:
            bad.append('%s has no time' % c)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture', default='out/lane86/bryan/capture', help='a capture dir under the MAIN checkout')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capture = os.path.join(main_co, args.capture)
    for need in (exe, capture):
        if not os.path.exists(need):
            print('FAIL input missing: ' + need)
            return 1
    for attempt in range(1, 4):  # the PC's GPU is shared with other lanes: a noisy window gets two more tries; a real fault fails all three
        print('[attempt %d]' % attempt)
        fails = once(exe, capture)
        if not fails:
            break
    print('RESULT %s' % ('FAIL (' + ', '.join(fails) + ')' if fails else 'PASS'))
    return 1 if fails else 0


def once(exe, capture):
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
        if not ok:
            fails.append(name)

    with tempfile.TemporaryDirectory(prefix='gpu_stamps_') as tmp:
        runs = {}
        for tag, extra in (('base', ['--gpu-stamps', '1']), ('off', ['--gpu-stamps', '0']), ('empty', ['--gpu-stamps', '1', '--gpu-plant', '3']),
                           ('slow', ['--gpu-stamps', '1', '--gpu-plant', '4']), ('late', ['--gpu-stamps', '1', '--gpu-plant', '5']),
                           ('mapped', ['--gpu-stamps', '1', '--gpu-class-plant', 'scene_to_post'])):
            r = run(exe, capture, extra, tag, tmp)
            runs[tag] = r
            print('[run %s] exit %d, %s' % (tag, r['rc'], 'roles line ' + ('yes' if 'cls' in r else 'NO') + ', sanity ' + str(r.get('check'))))
            if r['rc'] != 0 and tag in ('base', 'off'):
                print(r['text'][-1500:])
                return ['run ' + tag]
        base, off, empty, slow, late, mapped = (runs[t] for t in ('base', 'off', 'empty', 'slow', 'late', 'mapped'))
        for tag in ('base', 'off', 'empty', 'slow', 'late', 'mapped'):
            if 'cls' not in runs[tag]:
                print(runs[tag]['text'][-2000:])
                check('roles line (%s)' % tag, False, 'the [rplay][gpu] roles line is missing')
                return fails
        for tag in ('base', 'empty', 'slow', 'late'):
            r = runs[tag]
            check('identity (%s): roles sum = work' % tag, abs(r['sum'] - r['work']) < 0.002, 'sum %.4f work %.4f' % (r['sum'], r['work']))
            check('identity (%s): work + start gap = the old bracket' % tag, abs(r['work'] + r['gap'] - r['total']) < 0.002, 'work %.4f + gap %.4f vs total %.4f' % (r['work'], r['gap'], r['total']))
        if 'plain_mean' in off and 'plain_mean' in base:
            check('bracket: stamps on vs the old two-stamp bracket', abs(base['plain_mean'] - off['plain_mean']) < 0.3,
                  'stamps on %.4f ms, off %.4f ms (the stamps cost ~0.1 ms a frame on the PC: the reason the switch ships off)' % (base['plain_mean'], off['plain_mean']))
        else:
            check('bracket', False, 'a run printed no [rplay][gpu] check line')
        check('sanity: no implausible / not-ready results', all(runs[t].get('implausible', 1) == 0 and runs[t].get('not_ready', 1) == 0 for t in runs),
              ', '.join('%s %s/%s' % (t, runs[t].get('implausible'), runs[t].get('not_ready')) for t in runs))
        print('base: %s' % ', '.join('%s %.4f' % (c, base['cls'][c]) for c in CLASSES) + '; work %.4f gap %.4f total %.4f; frames %d' % (base['work'], base['gap'], base['total'], base['frames']))
        print('base passes a frame: %s' % ', '.join('%s %.2f' % (c, base['passes'][c]) for c in CLASSES))
        nz = [c for c in CLASSES if base['cls'][c] > 0.0005]
        check('roles: at least two roles carry time', len(nz) >= 2 and base['sum'] > 0, 'non-zero: ' + ','.join(nz))
        mp = mapping_problems(base)
        check('role mapping: colour, shadow, cards, post each carry passes and time on Bryan', not mp, '; '.join(mp) or 'ok')
        mpm = mapping_problems(mapped)
        check('twin: scene mapped as post is caught by the same assertion', bool(mpm) and mapped['passes']['colour'] == 0, '; '.join(mpm))
        pe = verdict(base, empty, 'empty', 'plane')
        check('empty pass reads ~0 in its role and moves nothing else', not pe, '; '.join(pe) or 'plane %+.4f ms' % (empty['cls']['plane'] - base['cls']['plane']))
        ps = verdict(base, slow, 'slow', 'rim')
        check('slow pass shows in its role and nowhere else', not ps, '; '.join(ps) or 'rim %+.4f ms' % (slow['cls']['rim'] - base['cls']['rim']))
        wrong1 = verdict(base, slow, 'slow', 'plane')
        check('twin: the slow pass declared in the plane role is rejected', bool(wrong1), '; '.join(wrong1)[:200])
        wrong2 = verdict(base, empty, 'slow', 'plane')
        check('twin: an empty pass declared as a slow one is rejected', bool(wrong2), '; '.join(wrong2)[:200])
        # plant 5 (a slow copy loop at the END of every frame, no window). Measured on the PC (NVIDIA RTX 4090, offline): the slow work lands in the TAIL (role `other`, bracket +9 ms) and
        # the next frame's start gap does NOT rise: this driver writes [0] and [1] without waiting for the previous submit. So the gap is not an overlap measure here either; what the
        # check pins is the honest statement: end-of-frame work is charged to `other` and the bracket, never to the start gap, and a driver that behaves differently shows up as a gap rise
        # in the Surface row (reported, not asserted).
        rise = late['gap'] - base['gap']
        other_rise = late['cls']['other'] - base['cls']['other']
        check('plant 5: end-of-frame slow work is charged to the tail (other) and the bracket', other_rise > 5.0 and late['total'] - base['total'] > 5.0,
              'other %+.4f ms, bracket %.3f -> %.3f ms' % (other_rise, base['total'], late['total']))
        print('plant 5 start gap (reported): %.4f ms (base) -> %.4f ms: %+.4f ms (on this PC driver the previous submit does not delay [1]; the Surface may differ)' % (base['gap'], late['gap'], rise))
        check('stamps off: the roles line is empty and no render is dropped as "no pass"', off['frames'] == 0 and off['no_pass'] == 0 and off.get('collected', 0) >= 89,
              'frames %d, no pass %d, collected %s' % (off['frames'], off['no_pass'], off.get('collected')))
        check('skipped renders counted apart (stamps on)', base['frames'] > 0, 'frames %d, no pass %d, no present %d, overflow %d' % (base['frames'], base['no_pass'], base['no_present'], base['overflow']))
        check('stamp pool never overflowed', base['overflow'] == 0, 'overflow %d' % base['overflow'])
        print('stamps a frame %.1f; acquire %.4f ms and Present call %.4f ms a render (offline: no window); rdr_submit busy %.4f ms a render over %d renders' % (base['stamps'], base['acquire_ms'], base['present_call_ms'], base['busy_ms'], base['busy_calls']))
    return fails


if __name__ == '__main__':
    sys.exit(main())
