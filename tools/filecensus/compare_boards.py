#!/usr/bin/env python3
"""Lane 21: reads a planted census board against the ground truth it carries (offline, 0 runs).

  py -I tools\\filecensus\\compare_boards.py BOARD.json --mirror        the planted "reader ignores the r / y flags": every arena with mirrored_slots > 0 must be
                                                                       flagged by the object-list check (obj_short), and no other arena may be
  py -I tools\\filecensus\\compare_boards.py BOARD.json --tex ITEMKEY    the planted missing texture: exactly ITEMKEY (e.g. arena_7) is flagged "load failure"
                                                                       with a texture warning, no other row
Exit 0 only when the board matches; prints each row that does not. A board that cannot be read, has no rows or no ground-truth column fails loudly."""
import argparse
import json
import sys

FLAGGED = ('tile fault', 'load failure')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('board')
    ap.add_argument('--mirror', action='store_true')
    ap.add_argument('--clean', action='store_true', help='with --mirror: nothing is planted; the draw-list count must equal the file count on every arena (then no arena is flagged)')
    ap.add_argument('--tex', default='')
    a = ap.parse_args()
    try:
        rows = json.load(open(a.board))['rows']
    except Exception as e:
        print('FAIL  cannot read the board %s: %s' % (a.board, e))
        return 1
    if not rows:
        print('FAIL  the board has no rows')
        return 1
    bad = []
    if a.mirror:
        arenas = [r for r in rows if r['kind'] == 'arena']
        if not arenas or any(r.get('file_slots') is None for r in arenas):
            print('FAIL  the board carries no file_slots ground truth (the r / y count read from the arena file; needs the census build)')
            return 1
        truth = {r['key'] for r in arenas if r['file_slots'] > 0}
        short = {r['key'] for r in arenas if r.get('obj_short')}
        if a.clean:
            truth_n, truth = len(truth), set()  # nothing planted: no arena may be flagged
        pic = {r['key'] for r in arenas if r.get('tile_ok') is False}  # what the picture check alone saw (all four views)
        for k in sorted(truth - short):
            bad.append('%s: the file flags r / y slots but the draw-list check did not flag it' % k)
        for k in sorted(short - truth):
            bad.append('%s was flagged by the draw-list check but the file flags no r / y slot' % k)
        if a.clean:
            for r in arenas:
                if r.get('submitted_flagged') != r['file_slots']:
                    bad.append('%s: file flags %s slots, draw list got %s (a clean build must match)' % (r['key'], r['file_slots'], r.get('submitted_flagged')))
        print('arenas %d, file flags r / y slots in %d, flagged by the draw-list check %d; the tile check alone flagged %d of the %d' %
              (len(arenas), len(truth), len(short), len(pic & truth), len(truth)))
    if a.tex:
        flagged = {r['key'] for r in rows if r['status'] == 'load failure' and any('texture' in n for n in r['notes'])}
        if a.tex not in {r['key'] for r in rows}:
            bad.append('item %s is not on the board' % a.tex)
        if flagged != {a.tex}:
            bad.append('texture-flagged items %s, expected only %s' % (sorted(flagged), a.tex))
        print('texture-flagged: %s' % sorted(flagged))
    for b in bad:
        print('FAIL  ' + b)
    if not bad:
        print('PASS  the board matches the planted fault')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
