r"""S102 lane G6 (name what the GPU stamps call "other"): offline proof on the Bryan entrance capture (the standard `bryan` input, 100 frames), 0 game runs.
  py -I tools\renderer\gpu_other_check.py [--root DIR] [--capture out/s101/lane92/case_.../capture]
rplay (this checkout's out\renderer\dev\rplay.exe) plays the capture with `--gpu-stamps 1`; the roles line gives mean ms a frame per GPU class.
DETERMINISTIC checks (run ONCE, never inside the retry loop: the same capture and exe give the same answer):
  identity      the classes add up to the work (the line's own numbers);
  pictures      the final frame's PNG is byte-equal with and without stamps, the plants and the census (the lane changes names, never pixels);
  verdict twin  the verdict function fed a made-up 50 % `other` reports a problem; fed 3 % none;
  census        `rplay --counts` + tools\renderer\gpu_other_census.py: under the shipped role table the pass-pixel share of `other` is under 10 %, the draw-class purity holds
                (frame_buffer images only get 2D / post draws, the crowd only goes into colour-class images); a planted table with no scene colour pushes `other` over 50 %;
                `--plant scene_unnamed` (the scene_color record rewritten to role 0) must FAIL the purity check;
  copies        the census run's `copies by source class` lines add up to the copies class, and name where the copies come from.
TIMING checks (GPU noise: 3 attempts, the PC's GPU is shared):
  base          `other` holds under 10 % of the stamped work, `copies`, `ui` and `frame` carry time;
  drop_copy     --gpu-class-plant drop_copy_dest: `other` must read over 10 % (the same assertion that passes on base fails): the twin proving it has teeth;
  drop_frame    --gpu-class-plant drop_frame_buffer: `other` must rise by at least 40 % of the frame class's time (the share moves ~5 points, but a 1.3 ms frame is noisy).
Exit 1 on any FAIL."""
import argparse, hashlib, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLASSES = ['colour', 'shadow', 'rim', 'plane', 'cards', 'post', 'output', 'ui', 'frame', 'copies', 'prelight', 'feedback', 'other']  # S102 G6: the roles line order (gpu_class_lint.py keeps it equal to renderer.cpp)
ROLES_RE = re.compile(r'\[rplay\]\[gpu\] roles: frames (\d+), mean ms a frame: ' + ' '.join(r'%s ([\d.]+)' % c for c in CLASSES) + r'; work ([\d.]+) gap ([\d.]+) total ([\d.]+); roles sum ([\d.]+)')
OTHER_BAR = 0.10


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def run(exe, capture, extra, tmp, tag):
    png = os.path.join(tmp, tag + '.png')
    p = subprocess.run([exe, '--capture', capture, '--frames', '100', '--gpu-log', os.path.join(tmp, tag + '.csv'), '--out', png] + extra, capture_output=True, text=True, timeout=900)  # (--gpu-log makes rplay print the roles line)
    text = p.stdout + p.stderr
    d = {'rc': p.returncode, 'tag': tag, 'png': png, 'text': text}
    m = ROLES_RE.search(text)
    if m:
        g = m.groups()
        n = len(CLASSES)
        d['cls'] = dict(zip(CLASSES, map(float, g[1:1 + n])))
        d['work'], d['gap'], d['total'], d['sum'] = map(float, g[1 + n:5 + n])
    return d


def other_share(r):
    return r['cls']['other'] / r['work'] if r['work'] > 0 else 1.0


def verdict(share):
    """-> list of problems for an `other` share of the stamped work."""
    return [] if share < OTHER_BAR else ['other holds %.1f %% of the stamped work (bar: under %.0f %%)' % (100 * share, 100 * OTHER_BAR)]


def census(rplay, capture, tmp, table=None, plant=None):
    counts = os.path.join(tmp, 'counts.json')
    if not os.path.exists(counts):
        p = subprocess.run([rplay, '--capture', capture, '--frames', '100', '--counts', counts, '--out', os.path.join(tmp, 'c.png')], capture_output=True, text=True, timeout=900)
        if p.returncode != 0 or not os.path.exists(counts):
            return None, p.stdout[-800:] + p.stderr[-800:], 1
    cmd = [sys.executable, '-I', os.path.join(HERE, 'gpu_other_census.py'), capture, '--counts', counts, '--frames', '100', '--top', '0']
    if table:
        cmd += ['--table', table]
    if plant:
        cmd += ['--plant', plant]
    q = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    m = re.search(r'area_px a frame \d+ \(other \d+ = ([\d.]+) %\)', q.stdout)
    return (float(m.group(1)) / 100.0 if m else None), q.stdout[-900:] + q.stderr[-600:], q.returncode


def deterministic(rplay, capture):
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
        if not ok:
            fails.append(name)

    with tempfile.TemporaryDirectory(prefix='gpu_other_det_') as tmp:
        runs = {}
        for tag, extra in (('plain', []), ('base', ['--gpu-stamps', '1']), ('drop_copy', ['--gpu-stamps', '1', '--gpu-class-plant', 'drop_copy_dest']),
                           ('drop_frame', ['--gpu-stamps', '1', '--gpu-class-plant', 'drop_frame_buffer']), ('census_on', ['--gpu-stamps', '1', '--gpu-census', '1'])):
            runs[tag] = run(rplay, capture, extra, tmp, tag)
            if runs[tag]['rc'] != 0:
                print(runs[tag]['text'][-1200:])
                return ['run ' + tag]
        base, cen = runs['base'], runs['census_on']
        if 'cls' not in base or 'cls' not in cen:
            check('roles line', False, 'the [rplay][gpu] roles line is missing')
            return fails
        check('identity: the classes add up to the work', abs(base['sum'] - base['work']) < 0.002 and abs(cen['sum'] - cen['work']) < 0.002, 'sum %.4f work %.4f' % (base['sum'], base['work']))
        check('twin: the verdict rejects a 50 % other and accepts a 3 % one', bool(verdict(0.5)) and not verdict(0.03), '')
        same = len({sha(runs[t]['png']) for t in runs}) == 1
        check('pictures: the final frame is byte-equal with stamps, plants and the census (names only)', same, ' '.join('%s %s' % (t, sha(runs[t]['png'])[:10]) for t in runs))
        share, text, rc = census(rplay, capture, tmp)
        check('census: `other` passes write under 10 % of the pass pixels, draw-class purity holds', share is not None and share < 0.10 and rc == 0,
              'share %s rc %d %s' % ('%.1f %%' % (100 * share) if share is not None else 'unreadable', rc, '' if rc == 0 else text[-300:]))
        table = os.path.join(tmp, 'table_no_scene.json')
        open(table, 'w').write('{"shadow_map": "shadow", "post_step": "post", "atlas_target": "cards"}')  # scene colour / depth left out: the scene's passes become `other`
        share2, text2, _ = census(rplay, capture, tmp, table)
        check('planted: a role table with no scene colour -> census `other` far over the bar', share2 is not None and share2 > 0.5, 'share %s' % ('%.1f %%' % (100 * share2) if share2 is not None else 'unreadable: ' + text2))
        share3, text3, rc3 = census(rplay, capture, tmp, plant='scene_unnamed')
        check('planted: --plant scene_unnamed (scene_color rewritten to role 0) FAILS the draw-class purity check', rc3 != 0 and 'FAIL purity' in text3, text3.strip().splitlines()[-1][:200] if text3.strip() else 'no output')
        # copies by source class (census run): the lines add up to the copies class
        lines = re.findall(r'\[rdr\]\[gpu\]\[census\] copies by source class (\w+): ([\d.]+) ms a frame, ([\d.]+) steps a frame', cen['text'])
        got = sum(float(ms) for _, ms, _ in lines)
        steps = sum(float(st) for _, _, st in lines)
        check('copies: the by-source-class lines name where the copies come from and add up to the copies class', len(lines) >= 3 and abs(got - cen['cls']['copies']) < 0.01 + 0.05 * cen['cls']['copies'],
              ', '.join('%s %.4f ms' % (c, float(ms)) for c, ms, _ in sorted(lines, key=lambda x: -float(x[1]))) + '; sum %.4f vs class copies %.4f; %.0f steps' % (got, cen['cls']['copies'], steps))
    return fails


def timing(rplay, capture):
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
        if not ok:
            fails.append(name)

    with tempfile.TemporaryDirectory(prefix='gpu_other_') as tmp:
        runs = {}
        for tag, extra in (('base', ['--gpu-stamps', '1']), ('drop_copy', ['--gpu-stamps', '1', '--gpu-class-plant', 'drop_copy_dest']), ('drop_frame', ['--gpu-stamps', '1', '--gpu-class-plant', 'drop_frame_buffer'])):
            runs[tag] = run(rplay, capture, extra, tmp, tag)
            if runs[tag]['rc'] != 0 or 'cls' not in runs[tag]:
                print(runs[tag]['text'][-1200:])
                return ['run ' + tag]
        base, dc, df = runs['base'], runs['drop_copy'], runs['drop_frame']
        print('base: ' + ', '.join('%s %.4f' % (c, base['cls'][c]) for c in CLASSES) + '; work %.4f' % base['work'])
        pv = verdict(other_share(base))
        check('other holds under %.0f %% of the stamped GPU time on Bryan' % (100 * OTHER_BAR), not pv, '; '.join(pv) or 'other %.4f of %.4f ms = %.1f %%' % (base['cls']['other'], base['work'], 100 * other_share(base)))
        check('the new groups carry time: copies and frame (ui has no picture on Bryan)', base['cls']['copies'] > 0.02 and base['cls']['frame'] > 0.01,
              'copies %.4f ms, frame %.4f ms' % (base['cls']['copies'], base['cls']['frame']))
        pc = verdict(other_share(dc))
        check('planted: the copies\' destinations left unnamed -> the same assertion FAILS', bool(pc), '; '.join(pc) or 'other only %.1f %%' % (100 * other_share(dc)))
        rise = df['cls']['other'] - base['cls']['other']
        check('planted: the frame buffer left unnamed moves its time into other (>= 40 % of the frame class)', rise >= 0.4 * base['cls']['frame'], 'other %.4f -> %.4f ms (frame class %.4f ms)' % (base['cls']['other'], df['cls']['other'], base['cls']['frame']))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture', default='out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', help='a capture dir under the MAIN checkout (the standard `bryan` input)')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capture = os.path.join(main_checkout(root), args.capture)
    for need in (rplay, os.path.join(capture, 'images.bin')):
        if not os.path.exists(need):
            print('FAIL input missing: ' + need)
            return 1
    print('[deterministic checks, once]')
    fails = deterministic(rplay, capture)
    if not fails:
        for attempt in range(1, 4):  # only the timing checks retry: the PC's GPU is shared, a real fault fails all three
            print('[timing attempt %d]' % attempt)
            fails = timing(rplay, capture)
            if not fails:
                break
    print('RESULT %s' % ('FAIL (' + ', '.join(fails) + ')' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
