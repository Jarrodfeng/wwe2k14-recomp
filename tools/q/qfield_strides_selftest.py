"""Self-test of qfield_strides (S111 QFIX4): `py -I tools/q/qfield_strides_selftest.py`   (0 game runs)

A. planted store: a stride row (K = 1392, stride 5292) is found by OFF 1392; a row with K = 6684 (= 1392 + 5292) is found by OFF 1392 as `K mod stride`; OFF 1396 /
   1388 find nothing; a row without a stride is never stored.
B. record / load round trip in a temp file; a class whose answer has no stride row is removed from the store.
C. print_rows names the stride; note() says how many classes were searched.
D. planted twins that must FAIL the same checks: matching on K only (the mod rule dropped), matching any offset inside the stride (a wide match).
E. the real store (skipped when no `q struct` / `q field --strides` run has filled it): the SmartMenu rows are found at 1392 with stride 5292.
Exit 0 = pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfield_strides as S  # noqa: E402

ROW = {'off': 1392, 'stride': [5292], 'widths': [4], 'kinds': ['u32'], 'reads': 1, 'writes': 1, 'passed': 0, 'name': 'a_0x570_x5292_u32', 'index': ['5292*[this+268]'],
       'readers': ['sub_8283C7A0'], 'writers': ['sub_8283A1C8']}
SHIFTED = dict(ROW, off=1392 + 5292, name='shifted')
FLAT = {'off': 1392, 'stride': [], 'widths': [4], 'readers': [], 'writers': []}


def checks(matches=S.matches):
    bad = []
    keep = S.matches
    S.matches = matches
    try:
        d = {'SM': {'target': 'sub_8283EAD8', 'rows': [ROW, SHIFTED]}}
        r = S.lookup(1392, d=d)
        if sorted(x['match'] for x in r['rows']) != ['K', 'K mod stride'] or r['classes'] != 1:
            bad.append(f"lookup 1392: want one K and one K mod stride row, got {[(x['name'], x['match']) for x in r['rows']]}")
        for off in (1388, 1396, 5292, 0):
            if S.lookup(off, d=d)['rows']:
                bad.append(f'lookup {off}: nothing holds it, got rows')
    finally:
        S.matches = keep
    return bad


def main():
    fails = checks()
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'strides.json')
        S.record('SM', 'sub_8283EAD8', [ROW, FLAT], path=p)
        d = S.load(p)
        if list(d) != ['SM'] or len(d['SM']['rows']) != 1:
            fails.append(f'record: a stride row kept, a flat row dropped; got {d}')
        if len(S.lookup(1392, path=p)['rows']) != 1:
            fails.append('record -> lookup 1392 by path')
        S.record('SM', 'sub_8283EAD8', [FLAT], path=p)
        if S.load(p):
            fails.append('record: an answer with no stride row removes the class')
    out = []
    S.print_rows({'classes': 1, 'rows': [dict(ROW, **{'class': 'SM', 'target': 't', 'match': 'K'})]}, out.append)
    if not any('5292*k' in x and 'stride 5292' in x for x in out) or not any('sub_8283C7A0' in x for x in out):
        fails.append(f'print_rows: stride and readers shown, got {out}')
    if '3 classes' not in S.note({'classes': 3, 'rows': []}, 1392):
        fails.append('note: the number of classes searched')
    twins = {'K only (no K mod stride)': lambda row, off: 'K' if row['off'] == off else None,
             'any offset inside the stride': lambda row, off: 'K' if any(row['off'] <= off < row['off'] + g for g in row['stride']) else None}
    missed = [n for n, f in twins.items() if not checks(f)]
    store = S.load()
    real = [x for x in S.lookup(1392, d=store)['rows']]
    skipped = None
    if not store:
        skipped = 'no stride store yet (run q struct sub_8283EAD8 or q field 1392 --strides sub_8283EAD8)'
    elif not any(5292 in x['stride'] and x['readers'] for x in real):
        fails.append(f"real store ({len(store)} classes): want the SmartMenu +1392 + 5292*k row with readers, got {[x['class'] for x in real]}")
    for f in fails:
        print('FAIL', f)
    for m in missed:
        print('NOT CAUGHT', m)
    print(f"qfield_strides_selftest: {'ok' if not fails else 'FAILED'}; {len(twins) - len(missed)}/{len(twins)} planted twins caught; "
          + (f'real store skipped ({skipped})' if skipped else 'real store ok'))
    return 1 if fails else (2 if missed else 0)


if __name__ == '__main__':
    sys.exit(main())
