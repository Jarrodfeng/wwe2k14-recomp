#!/usr/bin/env python3
r"""Lane E8 (Session 103): the game's wrestler key light and caster-pass camera, read from a case's stream, against the scene record's light states. 0 game runs.

  py -I tools\scene\e8_game_key.py CASE.zip --frames 400,500,600 [--json OUT]
Per frame it prints: the stream's wrestler colour draws' KeyLightVec / DifLghVec[4] values (distinct), the shadow-pass (T_ShadowMap) draws' constants (every named field a caster draw carries), the record's light-state
dir[4] rows for the wrestler refs (e8_key_dump.exe does the record side).
"""
import argparse, collections, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import feature_diff as fd  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('--frames', default='400,500,600')
    ap.add_argument('--fields', default='KeyLightVec,DifLghVec,ShadowPrm,TexProjLightXY,TexProjLightW,ViewLightDepth')
    a = ap.parse_args()
    fx = fd.Fx()
    case = fd.Case(a.case)
    want = a.fields.split(',')
    for n in [int(x) for x in a.frames.split(',')]:
        fr, draws, passes = fd.game_frame(case, fx, n)
        print('== frame %d: %d draws, passes %s' % (n, len(draws), dict(passes)))
        seen = collections.OrderedDict()
        for d in draws:
            for k in want:
                if k in d['vals']:
                    key = (d['family'], d['tech'] or '', k, tuple(round(x, 4) for x in d['vals'][k]))
                    seen[key] = seen.get(key, 0) + 1
        for (fam, tech, k, v), c in seen.items():
            print('  %-8s %-18s %-14s x%-3d %s' % (fam, tech[:18], k, c, v))
        shad = [d for d in draws if d['tech'] == 'T_ShadowMap']
        print('  T_ShadowMap draws: %d' % len(shad))
        if shad:
            names = collections.Counter(k for d in shad for k in d['vals'])
            print('  shadow-pass fields:', dict(names))
            print('  first shadow draw vs/ps:', shad[0]['vs'], shad[0]['ps'])
            for k in names:
                vs = {tuple(round(x, 4) for x in d['vals'][k]) for d in shad if k in d['vals']}
                print('   %s: %d distinct, first %s' % (k, len(vs), sorted(vs)[0]))


if __name__ == '__main__':
    main()
