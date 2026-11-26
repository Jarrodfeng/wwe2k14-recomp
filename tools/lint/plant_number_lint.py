#!/usr/bin/env python3
r"""S111 LAYERS review: the planted-twin numbers of tools\scene\guest_model_unit are each used ONCE (three lanes of S111 took the same numbers on parallel branches and
the merges kept both: GUESTVTX / EYECUBE 66, GUESTKEY / LAYERS 68 and 69, GUESTKEY / LAYERS draw plant 19; git merges two `if (plant == 68)` lines without a conflict).

  py -I tools\lint\plant_number_lint.py [--root DIR] [--selftest]

Reads guest_model_unit.cpp's plant dispatch (the `if (plant == N) X = V;` / `if (plant >= A && plant <= B) X = plant - K;` statements of main) and guest_model_unit.py's PLANTS list:
  1. every unit plant number N is dispatched by one statement only;
  2. every product plant value (the variable X and the value V it gets) is set by one unit plant only (two twins that plant the same product plant mean two faults);
  3. PLANTS holds no number twice, and it is exactly the dispatched set (a dispatched twin never run, or a run number nothing plants, is a finding).
A dispatch statement whose condition or value it cannot parse is a finding too (never a silent pass). Every run also feeds the rules planted twins (synthetic texts) and requires
each to fire. Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = the files are missing. 0 game runs."""
import argparse
import os
import re
import sys

UNIT_CPP = 'tools/scene/guest_model_unit.cpp'
UNIT_PY = 'tools/scene/guest_model_unit.py'
STMT = re.compile(r'^[ \t]*if\s*\((?P<cond>[^;{}]*?\bplant\s*(?:==|>=)[^;{}]*?)\)\s*(?P<body>[^;{}\n]+);', re.M)
EQ = re.compile(r'^\s*plant\s*==\s*(\d+)\s*$')
RANGE = re.compile(r'^\s*plant\s*>=\s*(\d+)\s*&&\s*plant\s*<=\s*(\d+)\s*$')
ASSIGN = re.compile(r'^\s*([\w:]+)\s*=\s*(?:(\d+)|plant\s*(?:([-+])\s*(\d+))?)\s*$')
PLANTS = re.compile(r'^PLANTS\s*=\s*\[([^\]]*)\]', re.M)


def dispatch(cpp):
    """([(line, numbers, var, values-by-number or None, text)], problems) of main()'s plant statements."""
    start = cpp.find('int main(')
    if start < 0:
        return [], ['%s: no main()' % UNIT_CPP]
    out, bad = [], []
    for m in STMT.finditer(cpp, start):
        line = cpp.count('\n', 0, m.start()) + 1
        nums = []
        for part in m.group('cond').split('||'):
            e, r = EQ.match(part), RANGE.match(part)
            if e:
                nums.append(int(e.group(1)))
            elif r and int(r.group(1)) <= int(r.group(2)):
                nums.extend(range(int(r.group(1)), int(r.group(2)) + 1))
            else:
                bad.append('%s:%d: a plant condition this lint cannot read (`%s`): write it as `plant == N` / `plant >= A && plant <= B` joined by ||' % (UNIT_CPP, line, part.strip()))
                nums = None
                break
        if nums is None:
            continue
        a = ASSIGN.match(m.group('body'))
        if not a:
            bad.append('%s:%d: a plant statement whose value this lint cannot read (`%s`): one `variable = N | plant | plant - K` per statement' % (UNIT_CPP, line, m.group('body').strip()))
            continue
        var = a.group(1).split('::')[-1]
        if a.group(2) is not None:
            vals = {n: int(a.group(2)) for n in nums}
        else:
            k = int(a.group(4) or 0) * (-1 if a.group(3) == '-' else 1)
            vals = {n: n + k for n in nums}
        out.append((line, nums, var, vals, m.group(0).strip()))
    return out, bad


def judge(cpp, py):
    """The findings (empty = pass)."""
    stmts, bad = dispatch(cpp)
    if not stmts and not bad:
        bad.append('%s: no plant dispatch statement found in main()' % UNIT_CPP)
    by_num, by_val = {}, {}
    for line, nums, var, vals, _text in stmts:
        for n in nums:
            if n in by_num:
                bad.append('%s:%d: unit plant %d is dispatched again (first at line %d): two twins share one number' % (UNIT_CPP, line, n, by_num[n]))
            by_num.setdefault(n, line)
            key = (var, vals[n])
            if key in by_val and by_val[key][0] != n:
                bad.append('%s:%d: unit plant %d sets %s = %d, which unit plant %d (line %d) sets too: two twins plant one product plant' % (UNIT_CPP, line, n, var, vals[n], by_val[key][0], by_val[key][1]))
            by_val.setdefault(key, (n, line))
    m = PLANTS.search(py)
    if not m:
        bad.append('%s: no `PLANTS = [...]` line' % UNIT_PY)
        return bad
    try:
        run = [int(x) for x in m.group(1).replace(' ', '').split(',') if x]
    except ValueError:
        return bad + ['%s: PLANTS is not a list of integers' % UNIT_PY]
    seen = set()
    for n in run:
        if n in seen:
            bad.append('%s: PLANTS holds %d twice' % (UNIT_PY, n))
        seen.add(n)
    for n in sorted(set(by_num) - seen):
        bad.append('%s: unit plant %d is dispatched (cpp line %d) but PLANTS never runs it' % (UNIT_PY, n, by_num[n]))
    for n in sorted(seen - set(by_num)):
        bad.append('%s: PLANTS runs %d, which no cpp statement dispatches (the run proves nothing)' % (UNIT_PY, n))
    return bad


GOOD_CPP = ('int main(int argc, char** argv) {\n  int plant = 0;\n  if (plant >= 1 && plant <= 3) gm::g_gm_plant = plant;\n  if (plant == 41) guestmodel::g_draw_plant = 11;\n'
            '  if (plant == 61 || plant == 62) guestmodel::g_draw_plant = plant - 46;\n}\n')
GOOD_PY = 'PLANTS = [1, 2, 3, 41, 61, 62]   # comment\n'
TWINS = [
    ('a unit number dispatched twice (the GUESTKEY / LAYERS 68)', GOOD_CPP.replace('  if (plant == 41)', '  if (plant == 41) rdr_scene::g_guest_key_plant = 1;\n  if (plant == 41)'), GOOD_PY),
    ('two unit plants that set one product plant (draw plant 19 twice)', GOOD_CPP.replace('}\n', '  if (plant == 70) guestmodel::g_draw_plant = 16;\n}\n'), GOOD_PY.replace('62]', '62, 70]')),
    ('a range that overlaps a single number', GOOD_CPP.replace('plant == 41', 'plant == 2'), GOOD_PY.replace('41, ', '')),
    ('PLANTS holds a number twice', GOOD_CPP, GOOD_PY.replace('62]', '62, 41]')),
    ('PLANTS lacks a dispatched number', GOOD_CPP, GOOD_PY.replace('41, ', '')),
    ('PLANTS runs a number nothing dispatches', GOOD_CPP, GOOD_PY.replace('62]', '62, 99]')),
    ('an unreadable plant condition', GOOD_CPP.replace('plant == 41', 'plant == 41 && argc > 1'), GOOD_PY),
]


def selftest():
    """[failure lines] of the planted twins: each must fire, and the good pair must pass."""
    out = []
    if judge(GOOD_CPP, GOOD_PY):
        out.append('selftest: the good synthetic pair fails: %s' % judge(GOOD_CPP, GOOD_PY))
    for name, cpp, py in TWINS:
        if not judge(cpp, py):
            out.append('selftest: the planted twin "%s" did not fire' % name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins')
    ap.add_argument('--cpp', default=None, help='read this file as the unit cpp (a merge result kept elsewhere)')
    ap.add_argument('--py', default=None, help='read this file as the unit py')
    a = ap.parse_args()
    st = selftest()
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('plant_number_lint selftest: %s' % ('FAIL' if st else 'every twin fires'))
        return 1 if st else 0
    texts = {}
    for rel, given in ((UNIT_CPP, a.cpp), (UNIT_PY, a.py)):
        try:
            with open(given or os.path.join(a.root, rel), encoding='utf-8', errors='replace') as f:
                texts[rel] = f.read()
        except OSError as e:
            print('plant_number_lint: cannot read %s: %s' % (rel, e))
            return 2
    bad = judge(texts[UNIT_CPP], texts[UNIT_PY])
    for b in bad:
        print('FAIL ' + b)
    n = len(dispatch(texts[UNIT_CPP])[0])
    print('plant_number_lint: %s (%d dispatch statements; %d twins fire)' % ('FAIL (%d)' % (len(bad) + len(st)) if bad or st else 'pass', n, len(TWINS) - len(st)))
    return 1 if bad or st else 0


if __name__ == '__main__':
    sys.exit(main())
