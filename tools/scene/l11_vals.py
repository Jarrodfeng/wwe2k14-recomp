#!/usr/bin/env python3
r"""S103 L11: the arena structure's lighting VALUES, game vs ours, per variant class (0 game runs).
  py -I tools\scene\l11_vals.py CASE.zip --frame 641 --dump out\s103\L11\fd\ours_641_dump.jsonl
Prints, for the arena (yBG_) draws of one stream frame and the rig's feature dump of the same record, the distinct value tuples of each light / material constant with draw counts."""
import argparse, collections, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import feature_diff as fd  # noqa: E402

KEYS = ['DifLghCol', 'DifLghVec', 'SkyCol', 'GroundCol', 'SkyDir', 'MatAmbCol', 'MatDifCol', 'LightMin', 'AmbCol', 'LightMapParam', 'MdlAlpha', 'SpecularLev', 'SpecularCol']


def fmt(v):
    return '(' + ' '.join('%.3g' % x for x in v) + ')'


def summarise(draws, label, pick):
    print('==== %s' % label)
    for cls, sel in pick:
        ds = [d for d in draws if sel(d)]
        print('-- %s: %d draws' % (cls, len(ds)))
        for k in KEYS:
            c = collections.Counter(fmt(d['vals'][k]) for d in ds if k in d['vals'])
            if c:
                print('   %-13s %s' % (k, '  '.join('%s x%d' % (s, n) for s, n in c.most_common(6))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('--frame', type=int, default=641)
    ap.add_argument('--dump', required=True)
    a = ap.parse_args()
    fx = fd.Fx()
    case = fd.Case(a.case)
    fr, g, _ = fd.game_frame(case, fx, a.frame)
    o = []
    begins = 0
    for line in open(a.dump, encoding='utf-8'):
        j = json.loads(line)
        if j.get('k') == 'pass' and j.get('name') == 'frame_begin':
            begins += 1
            continue
        if j.get('k') == 'draw' and begins <= 1:
            o.append(j)
    pick = [('arena vertex_lm', lambda d: (d.get('vs') or '').startswith('yBG_') and 'vertex_lm' in d['vs']),
            ('arena plain', lambda d: (d.get('vs') or '').startswith('yBG_') and 'vertex_lm' not in d['vs'])]
    summarise([d for d in g], 'GAME frame %d' % a.frame, pick)
    summarise(o, 'OURS', pick)


if __name__ == '__main__':
    main()
