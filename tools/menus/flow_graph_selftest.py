"""S110 CASENTRY: flow_graph.py --records (a raw 76-byte record dump, src/cas_launch.h --wwe2k14_cas_flow_dump) on planted records.

  py -I tools/menus/flow_graph_selftest.py        exit 0 = every check holds, 1 = a check failed (printed)

Plants three records (a top-level submenu item, a page leaf, a list-page leaf) in a temp file, reads them back through
read_records_file and checks the graph; then a file that is not a whole number of records must be refused, and the
JSON written by the command line must match. No game data: the words are made up here.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flow_graph  # noqa: E402

FAILS = []


def check(ok, what):
    if not ok:
        FAILS.append(what)
        print('FAIL:', what)


def record(rec, parent, menu, item, label, flags, code, rtype):
    w = [0] * 19
    w[0], w[1], w[2], w[3] = rec & 0xFFFFFFFF, parent & 0xFFFFFFFF, menu, item << 16
    w[6], w[10], w[11], w[13] = label, flags, code & 0xFFFFFFFF, rtype & 0xFFFFFFFF
    return struct.pack('>19I', *w)


def main():
    blob = (record(1, -1, 0, 0, 0x1A00, 0x00, 0, 0) +       # menu 0 item 0: a submenu (bit 0x10 clear) -> menu 5
            record(2, 1, 5, 0, 0x1A01, 0x10, 1002, 0) +     # menu 5 item 0: a page leaf, code 1002
            record(3, 1, 5, 1, 0x1A02, 0x10, 1001, 7))      # menu 5 item 1: code 1001 (list page in the editor; the reader tags it 'rules 7')
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'cas_flow_test.bin')
        with open(path, 'wb') as f:
            f.write(blob)
        g = flow_graph.read_records_file(path)
        check(g['records'] == 3, 'three records read (got %s)' % g['records'])
        check(g['run'] is None and g['snapshot'] == 'cas_flow_test.bin', 'run / snapshot keys of a dump')
        menus = {m['menu']: m for m in g['menus']}
        check(sorted(menus) == [0, 5], 'menus 0 and 5 (got %s)' % sorted(menus))
        top = menus[0]['items'][0]
        check(top['leads_to'] == 'menu 5' and top['submenu'] == 5, 'menu 0 item 0 leads to menu 5 (got %s)' % top.get('leads_to'))
        check(menus[5]['opened_from'] == {'menu': 0, 'item': 0}, 'menu 5 opened from menu 0 item 0 (got %s)' % menus[5]['opened_from'])
        leaf = menus[5]['items'][0]
        check(leaf['code'] == 1002 and leaf['leads_to'] == 'screen 1002' and leaf['label_id'] == '0x1A01', 'menu 5 item 0 = screen 1002')
        lst = menus[5]['items'][1]
        check(lst['code'] == 1001 and lst['match_type'] == 7, 'menu 5 item 1 = code 1001, type word 7')

        bad = os.path.join(d, 'bad.bin')
        with open(bad, 'wb') as f:
            f.write(blob[:-4])
        try:
            flow_graph.read_records_file(bad)
            check(False, 'a dump that is not whole records is refused')
        except SystemExit:
            pass

        out = os.path.join(d, 'g.json')
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'flow_graph.py'), '--records', path, '--json', out],
                           capture_output=True, text=True)
        check(r.returncode == 0, 'command line --records exits 0 (%s)' % r.stderr.strip())
        if r.returncode == 0:
            with open(out, encoding='utf-8') as f:
                check(json.load(f) == json.loads(json.dumps(g)), 'the command line writes the same graph')
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'flow_graph.py')], capture_output=True, text=True)
        check(r.returncode != 0, 'no RUN NAME and no --records is refused')
    print('flow_graph_selftest: %s (%d failed)' % ('OK' if not FAILS else 'FAILED', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
