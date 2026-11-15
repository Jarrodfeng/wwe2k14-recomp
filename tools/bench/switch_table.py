"""S102 S0-4: reads the switch table (WWE2K14_RDR_SWITCHES in src\\renderer_adapter\\rdr_switches.h) so the Python tools know each switch's
cvar name, kind (PerFrame / StartOnly) and default without a second list to keep in step.
  py -I tools\\bench\\switch_table.py [NAME]
"""
import os
import re
import sys

HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'src', 'renderer_adapter', 'rdr_switches.h')
ENTRY = re.compile(r'^\s*X\((\w+),\s*"([^"]+)",\s*(PerFrame|StartOnly),\s*(-?\d+)\)')


def load(path=HEADER):
    table = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = ENTRY.match(line)
            if m:
                table[m.group(2)] = {'id': m.group(1), 'kind': m.group(3), 'default': int(m.group(4))}
    if not table:
        raise RuntimeError('no X(...) entries found in %s' % path)
    return table


def norm(value):
    """A setting's text as a comparable string: true / false / 1 / 0 / '4' / '0x4' all become a decimal."""
    v = str(value).strip().lower()
    if v == 'true':
        return '1'
    if v == 'false':
        return '0'
    try:
        return str(int(v, 0))
    except ValueError:
        return v


if __name__ == '__main__':
    t = load()
    for name in sys.argv[1:] or sorted(t):
        print(name, t.get(name))
