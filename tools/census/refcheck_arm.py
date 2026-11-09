r"""Session 98 step 15: the REFERENCE-CHECK census arm. A separate pass (out\census\refcheck\), OFF unless asked: it is not part
of run.py's sweeps and nothing else calls it.

What it answers: "for every arena, does the main renderer's picture line up with the Arena Viewer's picture of the same
arena at the game's own camera?" Per census item (one arena, a 1v1 from out\census\items.json) it plays the entrances to the
bell, and at fixed moments takes a screenshot followed by ONE frame of the SDK draw probe (all 256 vertex constants = the
view-projection c56..c59 + eye c72), then closes the game. Afterwards `--compare` runs tools\assets\refcheck.py on every
captured sample (no game, ~3-5 s each) and writes out\refcheck\<id>_<tag>\report.html + a sheet.

  py tools\census\refcheck_arm.py --capture [--only arena_05,arena_07] [--lane N] [--exe X] [--dry]   (game runs: ONE per arena)
  py tools\census\refcheck_arm.py --compare [--only ...]                                          (0 game runs)
  py tools\census\refcheck_arm.py --recipe                                                        (print the capture recipe)

Samples per arena (tags): ent1 = the first entrance step frame, ent2 = ~600 guest frames later (still in the entrance, if any),
match = 180 guest frames after the bell. `--finish` adds `cel`: the match is played to a pinfall / finish (CPU match, pace=quick)
and the celebration frame is sampled (slow: use for the few arenas that matter).

Capture recipe by hand (what this script does inside, for one live game; MCP game_match / game_run / game_screenshot):
  1. start the game with  --gpu_debug_draw_probe=*  --gpu_debug_draw_probe_consts=0-255  --gpu_debug_draw_probe_path=<csv>
     (the draw probe writes ~13 MB per probed frame: keep the number of samples small, delete the csv after --compare)
  2. build the match (game_match "A vs B; type=one on one; arena=N; players=0; pace=quick"), wait for the entrance events
  3. at each sample moment:  screenshot LABEL   then   cvar gpu_debug_draw_probe_frames 1   then  wait_frames 6
     (the probe frame is the one AFTER the shot, so the probe's own draws cannot change the judged picture; the camera must
     not move between them: sample while the camera is steady, not on a cut)
  4. refcheck reads sample k of the csv as `--swap index:k` (the k-th distinct guest frame in the file)
Status: written and unit-checked against Session 97 files only (0 game runs); the first live capture is the milestone's.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))

PASS = 'refcheck'
NON_MATCH_ARENAS = set(range(66, 81))  # non-match backgrounds (black / rooms are correct there)


def pdir():
    import common as C
    return os.path.join(C.CEN, PASS)


def load_items(only=None):
    """one item per arena: the census 'arena' / 'roster' items (each arena once), out\\census\\items.json"""
    import common as C
    path = os.path.join(C.CEN, 'items.json')
    if not os.path.exists(path):
        import items
        items.main()
    with open(path, encoding='utf-8') as f:
        allitems = json.load(f)
    seen, out = set(), []
    for it in allitems:
        if it.get('family') in ('arena', 'roster') and it.get('arena') not in seen and it['arena'] not in NON_MATCH_ARENAS:
            seen.add(it['arena'])
            w = it.get('wrestlers') or [0, 1]
            # one 1v1 per arena (the six-mans of the roster family cost 3x the entrances)
            out.append(dict(it, id='arena_%02d' % it['arena'], entrances='full',
                            spec='#%d, #%d; type=0; arena=%d; pace=quick; players=0' % (w[0], w[1], it['arena'])))
    if only:
        out = [i for i in out if i['id'] in only]
    return out


def probe_flags(csv_path):
    return ['--gpu_debug_draw_probe=*', '--gpu_debug_draw_probe_consts=0-255', '--gpu_debug_draw_probe_path=%s' % csv_path]


def sample(g, man, tag, look):
    """screenshot, then one probed frame, then a few frames of margin; the manifest gets the sample"""
    rec = g.shot()
    k = len(man['samples'])
    g.batch(['cvar gpu_debug_draw_probe_frames 1', 'wait_frames 6'], timeout=120)
    man['samples'].append({'tag': tag, 'look': look, 'index': k, 'shot': rec['path'], 'phase': g.tr.phase})
    g.say('refcheck sample %s k=%d phase=%s' % (tag, k, g.tr.phase))


def capture_item(item, lane, exe, say, finish=False):
    import common as C
    from common import Crashed, Stuck, Unbuildable
    d = os.path.join(pdir(), item['id'])
    os.makedirs(d, exist_ok=True)
    csv_path = os.path.join(d, 'probe.csv')
    if os.path.exists(csv_path):
        os.remove(csv_path)
    man = {'id': item['id'], 'arena': item['arena'], 'probe': csv_path, 'samples': [], 'status': 'ok', 'note': ''}
    g = C.Game(lane, dict(item, timeout_s=1500 if not finish else 2700), exe, say)
    try:
        import random
        random.seed(item['id'])
        spec = C.match.build_spec(item['spec'])[0]
        g.start(extra_flags=probe_flags(csv_path))
        g.build_match(spec)
        # entrances: wait for the first entrance step, sample, 600 frames on sample again (steady camera: the entrance walk)
        ev = g.wait_for(lambda e: e.get('kind') == 'entrance' and (e.get('step') or 0) > 0, 300)
        if ev:
            g.batch(['wait_frames 120'], timeout=120)
            sample(g, man, 'ent1', 'entrance')
            g.batch(['wait_frames 600'], timeout=300)
            if g.tr.phase == 'entrance':
                sample(g, man, 'ent2', 'entrance')
        else:
            man['note'] = 'no entrance step within 5 min'
        if not g.entrances_to_bell(900):
            raise Stuck('no bell within 15 min of entrances')
        g.batch(['wait_frames 180'], timeout=300)
        sample(g, man, 'match', 'match')
        if finish:
            if not g.skip_to_bell():
                pass
            end = time.time() + 1500
            while time.time() < end and not any(e.get('what') == 'finish' for e in g.events if e.get('kind') == 'match'):
                g.batch(['wait_frames 600'], timeout=300)
                g.pump()
            g.batch(['wait_frames 240'], timeout=120)
            sample(g, man, 'cel', 'match')
    except Unbuildable as x:
        man['status'], man['note'] = 'unbuildable', str(x)
    except Stuck as x:
        man['status'], man['note'] = 'stuck', str(x)
    except (Crashed, C.live.NoGame) as x:
        man['status'], man['note'] = 'crash', str(x)[:200]
    finally:
        try:
            g.close()
        except Exception:
            pass
        with open(os.path.join(d, 'manifest.json'), 'w', encoding='utf-8') as f:
            json.dump(man, f, indent=1)
    return man


def compare_item(man):
    import refcheck
    out = []
    for s in man['samples']:
        if not os.path.exists(s['shot']):
            out.append({'id': man['id'], 'tag': s['tag'], 'error': 'screenshot missing'})
            continue
        it = {'name': '%s_%s' % (man['id'], s['tag']), 'shot': s['shot'], 'probe': man['probe'], 'swap': 'index:%d' % s['index'],
              'arena': man['arena'], 'look': s['look'],
              'ignore': []}  # wrestlers are not placed yet: the cells that hold them show as flags (mask by hand with --ignore)
        try:
            rep, _ = refcheck.run_item(it)
            out.append({'id': man['id'], 'tag': s['tag'], 'report': os.path.join(refcheck.OUT, it['name'], 'report.html'),
                        'flags': len(rep['flags']), 'align': rep['alignment'].get('applied'),
                        'worst_region': max(((n, r['ratio']) for n, r in rep['regions'].items() if r.get('pixels')),
                                            key=lambda x: abs(x[1] - 1), default=None)})
        except SystemExit as e:
            out.append({'id': man['id'], 'tag': s['tag'], 'error': str(e)[:200]})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture', action='store_true')
    ap.add_argument('--compare', action='store_true')
    ap.add_argument('--recipe', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--lane', default='refc1')
    ap.add_argument('--exe', default='')
    ap.add_argument('--finish', action='store_true')
    ap.add_argument('--dry', action='store_true')
    a = ap.parse_args()
    if a.recipe:
        print(__doc__)
        return 0
    only = set(x for x in a.only.split(',') if x)
    if a.capture:
        items = load_items(only)
        print('refcheck capture: %d arenas, one game each: %s' % (len(items), ' '.join(i['id'] for i in items)))
        if a.dry:
            for i in items:
                print(' ', i['id'], i['spec'])
            return 0
        import common as C
        os.makedirs(pdir(), exist_ok=True)
        for i in items:
            m = capture_item(i, a.lane, a.exe, lambda s: print(s, flush=True), finish=a.finish)
            print('%s: %s %s (%d samples)' % (i['id'], m['status'], m['note'], len(m['samples'])), flush=True)
    if a.compare:
        rows = []
        for sub in sorted(os.listdir(pdir())) if os.path.isdir(pdir()) else []:
            mp = os.path.join(pdir(), sub, 'manifest.json')
            if only and sub not in only:
                continue
            if os.path.exists(mp):
                with open(mp, encoding='utf-8') as f:
                    rows += compare_item(json.load(f))
        for r in rows:
            print(r)
        with open(os.path.join(pdir(), 'compare.json'), 'w', encoding='utf-8') as f:
            json.dump(rows, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
