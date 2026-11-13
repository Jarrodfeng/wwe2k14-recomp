r"""Lane 184 (S101): the yPreLight bake-slot census. Offline, 0 game runs. Counts only.

The light-cube bake (`sub_82C20BC8` -> `sub_82C20AB8`, effect yPreLight, 384x64 target + 6 copies) runs once per light SLOT; the stream order of a frame's bakes is the slot order
(slot 0 first, then slots 1-4; docs\systems\prelight_slots.md). For every captured frame this tool reads the bake passes, whether each one's draw and copies are LIVE (dead_passes.py's
backward liveness over the whole capture) and which model objects (origins `object`) read the bake's cube images afterwards. It prints, per capture, the distinct frame shapes and per slot:
frames baked, dead draws and copies, the draws that read it, the reader objects, and whether the objects of different slots are disjoint.

  py -I tools\prelight\slots_census.py CAPTURE.zip [...] [--check tools\prelight\slots_expect.json] [--json OUT]
--check: every capture named in the expect file that was given must reproduce its row exactly (exit 1 otherwise). Rows are keyed by the capture's file name."""
import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
sys.path.insert(0, os.path.join(HERE, '..', 'crowd'))
import dead_passes as dp  # noqa: E402

cc = dp.cc


def census(path):
    cap = cc.Cap(path)
    per = dp.run(cap, list(range(len(cap.frames))))
    shapes = collections.OrderedDict()
    slot = collections.defaultdict(lambda: {'baked': 0, 'dead_draw': 0, 'dead_copies': 0, 'copies': 0, 'readers': set(), 'reads': 0})
    frames_with = 0
    for fi, f, nodes, passes in per[:-1]:
        lab = dp.classify(cap, f, passes)
        bakes = [pi for pi in range(len(passes)) if 'yPreLight' in lab[pi][3]]
        if not bakes:
            continue
        frames_with += 1
        key = len(bakes)
        shapes[key] = shapes.get(key, 0) + 1
        for s, pi in enumerate(bakes):
            ops = passes[pi]['ops']
            draws = [n for n in ops if n['kind'] == 'draw']
            cps = [n for n in ops if n['kind'] == 'copy']
            d = slot[s]
            d['baked'] += 1
            d['dead_draw'] += sum(1 for n in draws if not n['alive'])
            d['dead_copies'] += sum(1 for n in cps if not n['alive'])
            d['copies'] += len(cps)
            dst = {n['w'][0] for n in cps}
            for n in nodes:
                if n['kind'] == 'draw' and dst & set(n['r']):
                    d['reads'] += 1
                    if f.has_origins:
                        d['readers'].add(int(f.origins['object'][n['d']]))
    rows = {}
    for s in sorted(slot):
        d = slot[s]
        rows[str(s)] = {'baked': d['baked'], 'dead_draw': d['dead_draw'], 'dead_copies': d['dead_copies'], 'copies': d['copies'], 'reads': d['reads'], 'reader_objects': len(d['readers'])}
    objs = [slot[s]['readers'] for s in sorted(slot)]
    disjoint = all(not (objs[i] & objs[j]) for i in range(len(objs)) for j in range(i + 1, len(objs)))
    return {'frames': len(per) - 1, 'frames_with_bakes': frames_with, 'shapes': {str(k): v for k, v in shapes.items()},
            'slots': rows, 'reader_objects_disjoint': disjoint}


def cut_violations(r, cut):
    """The slots of `cut` (slot indices of the stream order) that a cut would break on this capture: a slot whose draw or copies were live in some frame, or that some draw
    reads. An empty list = the cut is invisible on this capture (necessary, not sufficient: the live rule is not built, docs\\systems\\prelight_slots.md)."""
    bad = []
    for s in cut:
        d = r['slots'].get(str(s))
        if d is None:
            continue
        if d['reads'] or d['dead_draw'] != d['baked'] or d['dead_copies'] != d['copies']:
            bad.append(s)
    return bad


def show(name, r):
    print('%s: %d frames counted, %d with bakes; passes per baking frame: %s' % (name, r['frames'], r['frames_with_bakes'], ', '.join('%s x %d frames' % (k, v) for k, v in r['shapes'].items())))
    for s, d in r['slots'].items():
        print('  slot %s: baked in %d frames, dead draws %d, dead copies %d of %d, read by %d draws of %d objects' % (s, d['baked'], d['dead_draw'], d['dead_copies'], d['copies'], d['reads'], d['reader_objects']))
    print('  reader objects disjoint between slots: %s' % r['reader_objects_disjoint'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--check')
    ap.add_argument('--json')
    ap.add_argument('--cut', default='', help='comma list of slots: print the ones whose cut would change a capture (exit 1 if any)')
    a = ap.parse_args()
    out = {}
    for p in a.captures:
        if not os.path.exists(p):   # a worktree has no out\: the captures live in the main checkout
            import crowd_cards_check as ccc
            alt = os.path.join(ccc.main_checkout(os.path.join(HERE, '..', '..')), p)
            if not os.path.exists(alt):
                print('MISSING capture %s (tried %s)' % (p, alt))
                return 2
            p = alt
        r = census(p)
        out[os.path.basename(p)] = r
        show(os.path.basename(p), r)
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1, sort_keys=True)
    rc = 0
    if a.cut:
        cut = [int(x) for x in a.cut.split(',')]
        for k, r in out.items():
            b = cut_violations(r, cut)
            print('cut %s on %s: %s' % (cut, k, 'breaks slots %s' % b if b else 'invisible on this capture'))
            rc |= 1 if b else 0
    if a.check:
        want = json.load(open(a.check))
        bad = 0
        for k, w in want.items():
            if k not in out:
                continue
            if out[k] != w:
                print('CHECK FAIL %s' % k)
                print('  want', json.dumps(w, sort_keys=True))
                print('  got ', json.dumps(out[k], sort_keys=True))
                bad += 1
            else:
                print('check ok: %s' % k)
        rc |= 1 if bad else 0
    return rc


if __name__ == '__main__':
    sys.exit(main())
