r"""S110 SCENETOOL: the tools' view of the ONE table of cold-launch modes, read from the C++ table the exe obeys (src\mode_table.h kModes).

    py -I tools\scene\mode_table.py              print the rows (name, exit, sequence, screen, check, menu path)
    py -I tools\scene\mode_table.py --selftest   the parser against the real header and planted shape changes (exit 0 = all pass)

There is no second list of mode names: tools\run\scene.py (`scene mode NAME`) and tools\scene\catalogue.py (kind `mode`) both call rows().
The parser is strict on purpose: a header whose Mode struct or kModes rows change shape raises ValueError naming the line, so the tools fail
loudly instead of reading a wrong table. The launch log lines a route expects (src\mode_launch.h) are built here from the same row.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
HEADER = os.path.join('src', 'mode_table.h')
LAUNCH = os.path.join('src', 'mode_launch.h')
FIELDS = ['name', 'exit', 'seq', 'screen', 'check', 'path']     # struct Mode, in order
ROW_RX = re.compile(r'^\s*\{"([a-z0-9_]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*Check::(k[A-Za-z]+),\s*"((?:[^"\\]|\\.)*)"\},\s*$')
# the log lines of src\mode_launch.h Step, as format strings: the arrival a route waits for is printed by them (pinned by the selftest)
LOG_GATES = '"[wwe2k14][launch] {}: enter gates pass ({})"'
LOG_SENT = '"[wwe2k14][launch] {}: exit {} sent at frame {}'
LOG_SEQ = '"[wwe2k14][launch] {}: sequence {} is current at frame {}'


def parse(text, where=HEADER):
    """-> [row dict] from the header text. ValueError when the struct's fields or any line of the kModes block change shape."""
    m = re.search(r'struct Mode \{(.*?)\n\};', text, re.S)
    if not m:
        raise ValueError('%s: no `struct Mode {` block' % where)
    names = re.findall(r'^\s*[\w:<>]+\s+(\w+);', m.group(1), re.M)
    if names != FIELDS:
        raise ValueError('%s: struct Mode fields are %s, the parser reads %s: update tools\\scene\\mode_table.py' % (where, names, FIELDS))
    m = re.search(r'inline constexpr Mode kModes\[\] = \{\n(.*?)\n\};', text, re.S)
    if not m:
        raise ValueError('%s: no `inline constexpr Mode kModes[] = {` block' % where)
    rows = []
    for ln in m.group(1).splitlines():
        s = ln.strip()
        if not s or s.startswith('//'):
            continue
        r = ROW_RX.match(ln)
        if not r:
            raise ValueError('%s: a kModes line the parser does not read: %s' % (where, s))
        g = r.groups()
        rows.append({'name': g[0], 'exit': int(g[1]), 'seq': int(g[2]), 'screen': int(g[3]), 'check': g[4], 'path': g[5]})
    if not rows:
        raise ValueError('%s: kModes has no rows' % where)
    seen = [r['name'] for r in rows]
    if len(set(seen)) != len(seen):
        raise ValueError('%s: a mode name twice: %s' % (where, seen))
    return rows


def rows(root=ROOT):
    with open(os.path.join(root, HEADER), encoding='utf-8') as fh:
        return parse(fh.read(), HEADER)


def find(name, root=ROOT):
    return next((r for r in rows(root) if r['name'] == name), None)


def launch_value(row):
    """The --wwe2k14_launch value of a row: `@cas` for the cas row (src\\mode_table.h FromLaunch), `@mode:NAME` for every other."""
    return '@cas' if row['name'] == 'cas' else '@mode:' + row['name']


def expect_lines(row):
    """The game lines a launch of this row prints, in order (src\\mode_launch.h Step): the gates passed, the exit sent, the sequence current."""
    v = launch_value(row)
    return ['[wwe2k14][launch] %s: enter gates pass (' % v, '[wwe2k14][launch] %s: exit %d sent at frame' % (v, row['exit']),
            '[wwe2k14][launch] %s: sequence %d is current at frame' % (v, row['seq'])]


def log_formats_missing(root=ROOT):
    """The format strings expect_lines() follows that src\\mode_launch.h no longer holds ([] = all there)."""
    with open(os.path.join(root, LAUNCH), encoding='utf-8') as fh:
        t = fh.read()
    return [f for f in (LOG_GATES, LOG_SENT, LOG_SEQ) if f not in t]


def selftest():
    fails = []

    def check(ok, what, detail=''):
        print('%s  %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok or not detail else '\n      ' + str(detail)))
        if not ok:
            fails.append(what)
    with open(os.path.join(ROOT, HEADER), encoding='utf-8') as fh:
        real = fh.read()
    try:
        rs = parse(real)
        block = real[real.index('kModes[] = {'):]
        block = block[:block.index('\n};')]
        check(len(rs) == block.count('\n    {"'), 'every row line of kModes is read (%d rows)' % len(rs), [r['name'] for r in rs])
        cas = next((r for r in rs if r['name'] == 'cas'), None)
        check(cas is not None and cas['seq'] == 76 and cas['exit'] == 8, 'the cas row (the scene tool\'s `cas new`) is exit 8 -> sequence 76', cas)
        check(expect_lines(cas)[2] == '[wwe2k14][launch] @cas: sequence 76 is current at frame', 'the cas row launches as @cas', expect_lines(cas))
        ed = next((r for r in rs if r['name'] == 'cas_edit'), None)
        check(ed is not None and (ed['exit'], ed['seq'], ed['screen'], ed['check']) == (10, 76, 1505, 'kStorageCreatedSuperstar')
              and launch_value(ed) == '@mode:cas_edit', 'the cas_edit row (S110 CASEDIT) is exit 10 -> sequence 76 from screen 1505', ed)
    except ValueError as x:
        check(False, 'the real header parses', x)
    check(not log_formats_missing(), 'src\\mode_launch.h prints the three lines the routes expect', log_formats_missing())
    planted = [
        ('a seventh struct field', real.replace('  std::string_view path;', '  std::string_view path;\n  uint32_t extra;'), 'struct Mode fields'),
        ('a row split over two lines', real.replace('{"threads", 20, 77, 1504, Check::kNone, ', '{"threads", 20, 77, 1504,\n     Check::kNone, '), 'does not read'),
        ('a row with a seventh value', real.replace('{"logo", 19, 78, 1522, ', '{"logo", 19, 78, 1522, 0, '), 'does not read'),
        ('the array renamed', real.replace('Mode kModes[]', 'Mode kRows[]'), 'kModes'),
        ('a name twice', real.replace('{"shop", 25,', '{"logo", 25,'), 'twice'),
    ]
    for name, text, want in planted:
        if text == real:
            check(False, 'planted: %s (the twin did not apply: the header moved)' % name)
            continue
        try:
            parse(text)
            check(False, 'planted: %s is refused' % name, 'parsed')
        except ValueError as x:
            check(want in str(x), 'planted: %s is refused' % name, x)
    print('mode_table selftest: %d failed' % len(fails) if fails else 'mode_table selftest: all passed')
    return 1 if fails else 0


def main(argv):
    if '--selftest' in argv:
        return selftest()
    for r in rows():
        print('%-18s exit %-3d seq %-3d screen %-5d %-22s %s' % (r['name'], r['exit'], r['seq'], r['screen'], r['check'], r['path']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
