#!/usr/bin/env python3
"""S110 PIPECENSUS: classify what pseed_proof P1 says the census is MISSING (0 game runs). For every FrontEnd-family record of the reference run's list that the census lacks by FULL key:
  stale   = its vertex or fragment hash is not in the current catalog (a shader changed since the reference run: no census of this tree can hold it; the reference is what is old)
  current = both hashes resolve: a real miss, the census did not draw that state; counted by the program's family (yBG_ / yProp_ / yCh_ / yAudience / yParticle) and by the effect name
  py -I tools\\renderer\\pseed_p1_classes.py --census out\\pipeseed\\census --catalog out\\tools\\pseed_catalog.txt [--game-list PATH] [--list-misses]
Exit 0 always (it reports; pseed_proof.py is the verdict)."""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pipeline_seed as P   # noqa: E402
import pseed_proof as PP   # noqa: E402


def effect(name):
    """'yBG_Default_vertex_3 + ...' -> 'yBG_Default'; the family is its prefix up to the first '_' (yAudienceInst has none: the whole word)."""
    m = re.match(r'([A-Za-z0-9]+(?:_[A-Za-z0-9]+)?)', name)
    return m.group(1) if m else name


def classify(game, census, catalog):
    """Returns (fe, stale, current): the FE-family records, the missing ones with a stale hash, the missing ones that resolve."""
    have = {P.key_of(r) for r in census}
    fe = [r for r in game if PP.name_of(r).startswith(PP.FE_PREFIX)]
    stale, current = [], []
    for r in fe:
        if P.key_of(r) in have:
            continue
        (current if P.resolvable(r, catalog) else stale).append(r)
    return fe, stale, current


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--census', required=True)
    ap.add_argument('--catalog', required=True)
    ap.add_argument('--game-list', default='')
    ap.add_argument('--list-misses', action='store_true')
    a = ap.parse_args()
    game = P.decode_list(a.game_list or PP.default_game_list())
    census = PP.load_census(a.census)
    fe, stale, current = classify(game, census, P.load_catalog(a.catalog))
    print('P1 classes: %d FE-family reference keys; in census %d; missing %d = %d stale (a hash no current blob has) + %d current (the census did not draw the state)' % (
        len(fe), len(fe) - len(stale) - len(current), len(stale) + len(current), len(stale), len(current)))
    for label, recs in (('stale', stale), ('current', current)):
        c = collections.Counter(effect(PP.name_of(r).decode('latin1')) for r in recs)
        for k, v in sorted(c.items(), key=lambda kv: (-kv[1], kv[0])):
            print('  %-8s %-28s %d' % (label, k, v))
        if a.list_misses:
            for r in recs:
                print('    %s %s  %s' % (label, PP.name_of(r).decode('latin1'), P.key_of(r).hex()[:24]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
