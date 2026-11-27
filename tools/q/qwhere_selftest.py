"""qwhere_selftest: `--where REGEX [--context N]` on any q verb and `q asm FUNC --range A..B` (S111 QWHERE). 0 game runs.
A PLANTED verb (WWE_Q_VERBS_EXTRA, like qverbs_selftest) prints a header, rows, a totals line and a --json document: --where keeps the header and totals, shows the
`where: shown K of M lines` line, honours --context, works inside `q multi` (per question, the others untouched) and filters --json rows keeping the totals keys.
The asm case needs the local index + generated listing (skipped with a line when absent): a real function longer than the default 400-row cut prints whole with
--range, and its --where prints no `... N more` cut. Exit 1 = a check failed."""
import os
import re
import sqlite3
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:400]))
    if not ok:
        fails.append(what)


def q(*args, extra=None):
    env = dict(os.environ)
    env.pop('WWE_Q_VERBS_EXTRA', None)
    if extra:
        env['WWE_Q_VERBS_EXTRA'] = extra
    r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout, r.stderr


PLANTED_PY = '''import json
import sys


def main(argv):
    if '--json' in argv:
        print(json.dumps({'total': 5, 'rows': [{'f': 'alpha', 'n': 1}, {'f': 'beta', 'n': 2}, {'f': 'gamma', 'n': 3}], 'note': 'x'}))
        return 0
    print('planted header: 6 rows')
    for w in ('alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta'):
        print('  row ' + w)
    print('-- 6 rows in 1 file')
    print('to-stderr', file=sys.stderr)
    return 0
'''
PLANTED_TOML = """name = "plantedwhere"
module = "qplantedwhere"
"""


def planted():
    with tempfile.TemporaryDirectory() as d:
        for n, t in (('qplantedwhere.py', PLANTED_PY), ('plantedwhere.toml', PLANTED_TOML)):
            with open(os.path.join(d, n), 'w', encoding='utf-8', newline='\n') as f:
                f.write(t)
        rc, so, se = q('plantedwhere', '--where', 'gamma|zeta', extra=d)
        L = so.rstrip('\n').split('\n')
        check('only the matching rows + the header + the totals line are kept', L[:5] == ['planted header: 6 rows', '  row gamma', '  row zeta', '-- 6 rows in 1 file'][:4] + L[4:5]
              and 'row alpha' not in so and 'row delta' not in so, so)
        check('the last line says shown K of M lines, the regex and the call without --where',
              L[-1] == 'where: shown 4 of 8 lines (gamma|zeta); the rest: q plantedwhere', L[-1:])
        check('stderr passes through', 'to-stderr' in se and rc == 0, (rc, se))
        rc, so, se = q('plantedwhere', '--where=beta', '--context', '1', extra=d)
        check('--context 1 adds the lines around each match', '  row alpha' in so and '  row beta' in so and '  row gamma' in so and 'row delta' not in so, so)
        rc, so, se = q('multi', 'plantedwhere --where epsilon', 'plantedwhere', extra=d)
        i = so.index('[2/2]')
        check('inside multi: question 1 is filtered, question 2 is whole', 'where: shown 3 of 8 lines (epsilon)' in so[:i] and 'row alpha' not in so[:i]
              and 'row alpha' in so[i:] and 'where:' not in so[i:], so)
        rc, so, se = q('plantedwhere', '--json', '--where', 'beta', extra=d)
        import json
        try:
            doc = json.loads(so)
        except ValueError:
            doc = None
        check('--json: rows filtered, the totals keys kept, a where key says shown 1 of 3',
              bool(doc) and doc['total'] == 5 and doc['note'] == 'x' and doc['rows'] == [{'f': 'beta', 'n': 2}]
              and doc['where']['shown'] == 1 and doc['where']['of'] == 3, so)
        rc, so, se = q('multi', "plantedwhere --where 'row g'", extra=d)
        check("inside multi: a quoted regex with a space ('row g') is one word: only the gamma row, no bad-regex error",
              rc == 0 and '  row gamma' in so and 'row alpha' not in so and 'not a regular expression' not in so and "(row g)" in so, (rc, so, se))
        rc, so, se = q('multi', 'plantedwhere --where "row g"', extra=d)
        check('inside multi: the same with double quotes', rc == 0 and '  row gamma' in so and 'row alpha' not in so, (rc, so, se))
        rc, so, se = q('multi', r"plantedwhere --where 'gam\(?ma'", r'plantedwhere --where gam\(?ma', extra=d)
        check(r'inside multi: a regex with an escaped paren (gam\(?ma), quoted and bare, filters and is no error',
              rc == 0 and so.count('  row gamma') == 2 and 'row alpha' not in so and 'not a regular expression' not in so, (rc, so, se))
        rc, so, se = q('plantedwhere', 'two words', '--where', 'row g', extra=d)
        check('plain command line: a regex with a space; the rest-call line quotes a word with a space',
              rc == 0 and '  row gamma' in so and 'the rest: q plantedwhere "two words"' in so, (rc, so, se))
        rc, so, se = q('plantedwhere', '--where', '(', extra=d)
        check('a bad regex is `q: ...` exit 2', rc == 2 and 'not a regular expression' in se, (rc, se))
        rc, so, se = q('plantedwhere', '--where', extra=d)
        check('--where without a value is `q: ...` exit 2', rc == 2 and 'needs a value' in se, (rc, se))


def asm_case():
    import q as qmod
    try:
        con = qmod.connect(qmod.DB)
        row = con.execute('SELECT name, addr, size FROM funcs WHERE file IS NOT NULL AND size BETWEEN 2400 AND 6000 ORDER BY addr LIMIT 1').fetchone()
    except (sqlite3.Error, Exception) as e:      # no local index
        print('skip asm --range: no index (%s)' % str(e)[:80])
        return
    if row is None:
        print('skip asm --range: no function over 600 instructions in the index')
        return
    name, addr, size = row['name'], row['addr'], row['size']
    n = size // 4
    rc, so, se = q('asm', name)
    if rc != 0 and 'missing' in se:
        print('skip asm --range: generated listing not local')
        return
    check('default asm of %s (%d insns) is cut at 400 rows' % (name, n), re.search(r'\.\.\. %d more' % (n - 400), so) is not None, so[-200:])
    rc, so, se = q('asm', name, '--range', '0..')
    rows = [ln for ln in so.split('\n') if re.match(r'^[0-9A-F]{8}  ', ln)]
    check('--range 0.. prints all %d instructions, no cut line' % n, len(rows) == n and ' more (--limit' not in so, (rc, len(rows), so[-200:]))
    rc, so, se = q('asm', name, '--range', '+0x10..+0x1C')
    rows = [ln for ln in so.split('\n') if re.match(r'^[0-9A-F]{8}  ', ln)]
    check('--range +0x10..+0x1C = 4 instructions from +0x10', len(rows) == 4 and rows[0].startswith('%08X' % (addr + 0x10)), (rc, so[-300:], se))
    rc, so, se = q('asm', name, '--where', r'\bbl\b')
    check('--where prints the whole listing through the filter: no `more (--limit` cut line, a where: line', ' more (--limit' not in so and 'where: shown' in so, (rc, so[-300:]))
    rc, so, se = q('asm', name, '--range', '5..1')
    check('a backwards range is `q: ...` exit 2', rc == 2 and 'backwards' in se, (rc, se))


if __name__ == '__main__':
    planted()
    asm_case()
    print('qwhere selftest: %d problems' % len(fails))
    sys.exit(1 if fails else 0)
