r"""S102 CEN1: selftest of the census-by-phase reader (tools\surface\census_phase.py, printed by flip_line.py) on a synthetic flip log.
  py -I tools\tests\census_phase_test.py
Case A: every phase has its census after the marker -> no NO CENSUS line, the lines sit under the phase that ended.
Case B (the planted twin, the present_fetch / record_skip bug): the ON phase 2 has no census line -> the report says NO CENSUS IN PHASE for it.
Case C: a log without markers (an old build) -> the report says so and prints no per-phase verdict."""
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'surface'))
import flip_line

STAMP = '[10:00:00.000] '
# phases: -1 pre (ref), 0 ON, 1 ref, 2 ON, 3 ref
INFO = {'phases': [(100, 'ref', -1, None, -60), (160, 'on', 0, None, 0), (220, 'ref', 1, None, 60), (280, 'on', 2, None, 120), (340, 'ref', 3, None, 180)]}
CEN = '[rdr][present_fetch] frames %d, taken %d, declined 0 (by reason: fit 0, sgsr/dlss 0, cap 0)'


def log(parts):
    lines = []
    for kind, v in parts:
        if kind == 'marker':
            lines.append(STAMP + '[census] flush gen %d phase end: wwe2k14_rdr_present_fetch=%d' % (v[0], v[1]))
        elif kind == 'census':
            lines.append(STAMP + CEN % (v, v // 2))
        else:
            lines.append(STAMP + v)
    return '\n'.join(lines) + '\n'


def run(text, info=INFO):
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / 'wwe2k14_1.log').write_text(text, encoding='utf-8')
        buf = io.StringIO()
        with redirect_stdout(buf):
            flip_line.print_census(d, 1, info, 'ref')
        return buf.getvalue()


def main():
    fails = 0
    full = log([('x', '[switch] frame 100 wwe2k14_rdr_present_fetch=false'), ('marker', (1, 0)), ('census', 60), ('marker', (2, 1)), ('census', 120), ('marker', (3, 0)), ('census', 180),
                ('marker', (4, 1)), ('census', 240), ('marker', (5, 0)), ('census', 300)])
    a = run(full)
    if 'NO CENSUS' in a or a.count('1 line(s)') != 5:
        print('FAIL case A: every phase has its census\n' + a)
        fails += 1
    two = log([('marker', (1, 0)), ('census', 60), ('marker', (2, 1)), ('census', 120), ('marker', (3, 0)), ('census', 180), ('marker', (4, 1)), ('marker', (5, 0)), ('census', 300)])
    b = run(two)
    if 'phase 2 on [ON]' not in b or 'NO CENSUS IN PHASE' not in b or b.count('NO CENSUS IN PHASE') != 1:
        print('FAIL case B: the ON phase without a census line is not named\n' + b)
        fails += 1
    merged = log([('marker', (1, 0)), ('marker', (2, 5)), ('census', 60), ('marker', (3, 1)), ('census', 120)])
    m = run(merged, {'phases': [(100, 'ref', -1, None, -60), (160, 'on', 0, None, 0)]})
    if '(ended: wwe2k14_rdr_present_fetch=0 + wwe2k14_rdr_present_fetch=5)' not in m:
        print('FAIL case M: two switches flipping together are one group\n' + m)
        fails += 1
    c = run(log([('census', 60)]))
    if 'NO "[census] flush" MARKER' not in c:
        print('FAIL case C: a log without markers is not said\n' + c)
        fails += 1
    print('census_phase selftest: ' + ('FAILED %d' % fails if fails else 'ok (every phase read, the ON phase without a census named, merged flips, old-build log)'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
