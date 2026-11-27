r"""Self-test of `q overlap` (S111 QOVERLAP): `py -I tools/q/qoverlap_selftest.py`   (0 game runs)

A. Planted geometry through the production test qoverlap.overlap_rays (a camera looking down +z, boxes of 12 triangles):
   two boxes in line (A in front of B) = overlap; side by side = none; an opaque wall in front of both = none; a wall BETWEEN the two
   (A hidden behind it) = none; a family with no file geometry = "unknown" through qoverlap.run (a fake scene); a family with
   no triangles = no.
   Planted twins, each seen to FAIL its check: no_occluders (the walls ignored: the wall cases turn yes), any_a (every ray counts as an A hit:
   side by side turns yes).
B. Real re-find (SKIP without the game files): arena 21, `stage_a` vs `chamber_glass` = yes (CHAMBERGLASS's [C]: 0.88 % of the rays in the
   frustum count, 702 of 91300; here the share must lie in 0.3..2 %); stage_a vs an unknown family = unknown.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qoverlap as O   # noqa: E402

FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


def box(c, h):
    """an axis-aligned box centre c, half size h (scalar or 3) -> (12, 3, 3)"""
    c, h = np.array(c, float), np.array(h, float) * np.ones(3)
    v = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)], float) * h + c
    quads = [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)]
    out = []
    for a, b, cc, d in quads:
        out += [(v[a], v[b], v[cc]), (v[a], v[cc], v[d])]
    return np.array(out)


def wall(z, half=2000.0):
    """a big square wall at depth z (two triangles)"""
    return np.array([[(-half, -half, z), (half, -half, z), (half, half, z)], [(-half, -half, z), (half, half, z), (-half, half, z)]], float)


def cam():
    return [dict(eye=np.array([0.0, 0.0, -1000.0]), fwd=np.array([0.0, 0.0, 1.0]), right=np.array([1.0, 0.0, 0.0]), up=np.array([0.0, 1.0, 0.0]),
                 tanv=0.3, tanh=0.5, D=1000.0, yaw=0.0)]


def both(A, B, OCC, plant=None):
    return O.overlap_rays(A, B, OCC, cam(), 50, plant)['both']


class FakeScene:
    def __init__(self, sets):
        self.sets = sets

    def fam(self, n):
        return self.sets.get(n)

    def occluders(self, skip):
        return np.zeros((0, 3, 3))


def part_a():
    none = np.zeros((0, 3, 3))
    A_front, B_back = box((0, 0, 0), 100), box((0, 0, 400), 100)
    check('in line: A in front of B overlaps', both(A_front, B_back, none) > 0, str(both(A_front, B_back, none)))
    left, right = box((-200, 0, 0), 60), box((200, 0, 0), 60)
    check('side by side: no overlap', both(left, right, none) == 0, str(both(left, right, none)))
    w_front = wall(-500)
    check('an opaque wall in front of both: none', both(A_front, B_back, w_front) == 0, str(both(A_front, B_back, w_front)))
    w_mid = wall(-200)
    check('a wall between the camera and A: none', both(A_front, B_back, w_mid) == 0)
    w_behind = wall(900)
    check('a wall behind both: still overlap', both(A_front, B_back, w_behind) > 0)
    twin('no_occluders (the wall case turns yes)', both(A_front, B_back, w_front, 'no_occluders') > 0, 'the wall was ignored and the case did not change')
    twin('any_a (side by side turns yes)', both(left, right, none, 'any_a') > 0, 'every ray counted as an A hit and side by side did not change')
    sc = FakeScene({'stage_a': A_front, 'chamber_glass': B_back, 'empty': none})
    r = O.run(7, 'stage_a', 'chamber_glass', 8, rule='match', scene=sc)
    check('run: a planted scene answers yes or no, not unknown', r['answer'] in ('yes', 'no'), r['text'])
    r = O.run(7, 'stage_a', 'bodies', 8, rule='match', scene=sc)
    check('run: a family with no file geometry is unknown, named', r['answer'] == 'unknown' and 'bodies' in r['text'], r['text'])
    r = O.run(7, 'empty', 'stage_a', 8, rule='match', scene=sc)
    check('run: a family with no triangles answers no', r['answer'] == 'no', r['text'])
    cs = O.make_cameras('chamber', 12)
    check('cameras: 12 across the rule, distances inside the range, eyes distinct',
          len(cs) == 12 and all(1200 <= c['D'] <= 3200 for c in cs) and len({tuple(np.round(c['eye'], 1)) for c in cs}) == 12)


def part_b():
    try:
        sc = O.load_scene(21)
    except Exception as e:   # no game files
        print(f'SKIP part B (no arena 21: {type(e).__name__}: {e})')
        return
    r = O.run(21, 'stage_a', 'chamber_glass', 24, scene=sc)
    share = 100.0 * r['both'] / max(r['in_frustum'], 1)
    check('arena 21 stage_a vs chamber_glass overlaps', r['answer'] == 'yes', r['text'])
    check('the share of counting rays matches CHAMBERGLASS (0.3..2 %)', 0.3 <= share <= 2.0, '%.2f %%' % share)
    r = O.run(21, 'stage_a', 'bodies', 4, scene=sc)
    check('arena 21 stage_a vs bodies is unknown', r['answer'] == 'unknown', r['text'])


def main():
    part_a()
    part_b()
    print(f'overlap selftest: {len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 1 if FAILS else (2 if CAUGHT else 0)


if __name__ == '__main__':
    sys.exit(main())
