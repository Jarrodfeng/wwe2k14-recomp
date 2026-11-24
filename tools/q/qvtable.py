r"""q vtable: the slots of a vtable (a table of code addresses) and the reverse query (S103 lane QV).

    py tools/q/q.py vtable ADDR [--slots N] [--slot K] [--no-summary] [--json]
    py tools/q/q.py vtable --of FUNC [--json]          every vtable / slot whose word points at FUNC

ADDR is the first slot (offset +0 of the table). The table is read from the game image (out/harness/index/image.bin, the
initial values of read-only data) word by word and ends at the first word that is not the start of a function of the q
index (the index's `funcs`; the same set `q fn` uses for "data words pointing at it"). Each slot shows: index (and byte
offset), the address of the data word, the target function, its db name (db/*.json, if any), and a one-line summary of
`q decomp --fields` (reads / writes / calls and the first fields; built and cached by qdecomp).
--of FUNC: the data words that point at FUNC (the index's `ptrs`), each placed in its table. A table is the RTTI vtable
that holds the word when there is one; otherwise the maximal run of function-start words around it, which is a HEURISTIC
(two adjacent tables merge into one run; the slot index is then counted from the run start). The output says which.
Errors: `q: ...` on stderr, exit 2.
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE_BASE = 0x82000000
MAX_SLOTS = 4096


class VtError(Exception):
    pass


class Img:
    def __init__(self, path):
        try:
            with open(path, 'rb') as f:
                self.data = f.read()
        except OSError as e:
            raise VtError(f'cannot read the game image {path} ({e.strerror or e})')

    def word(self, a):
        o = a - IMAGE_BASE
        if a & 3 or o < 0 or o + 4 > len(self.data):
            return None
        return struct.unpack('>I', self.data[o:o + 4])[0]


def parse_addr(s):
    m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', s.strip())
    if not m:
        raise VtError(f'{s}: not an address (8 hex digits, 0x... or sub_...)')
    return int(m.group(1), 16)


def is_func(con, a, cache={}):
    if a is None:
        return False
    key = (id(con), a)
    if key not in cache:
        cache[key] = con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone() is not None
    return cache[key]


def read_table(con, img, base, limit=MAX_SLOTS):
    """-> [(slot, word_addr, target)] from base to the first non-function word."""
    out = []
    a = base
    while len(out) < limit:
        w = img.word(a)
        if not is_func(con, w):
            break
        out.append((len(out), a, w))
        a += 4
    return out


def db_names():
    import q as qm
    d = {}
    for a, _f, e in qm.load_db_entries():
        if e.get('name'):
            d.setdefault(a, e['name'])
    return d


def summary_of(con, addr):
    import qdecomp
    try:
        r = qdecomp.decompile(con, addr)
    except qdecomp.DecompError as e:
        return f'({e})'
    s = r['summary']
    bits = [f'{len(s["reads"])} fields read, {len(s["writes"])} written, {sum(s["calls"].values())} calls']
    for k, lab in (('reads', 'r'), ('writes', 'w')):
        fl = ', '.join(x['addr'] for x in s[k][:3])
        if fl:
            bits.append(f'{lab}: {fl}' + (' ...' if len(s[k]) > 3 else ''))
    return '; '.join(bits)


def fname(con, a):
    r = con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
    return r['name'] if r else f'0x{a:08X}'


def slots_of(con, img, base, nslots, only, summary):
    names = db_names()
    tbl = read_table(con, img, base)
    if not tbl:
        w = img.word(base)
        raise VtError(f'0x{base:08X}: the word there is {"unreadable" if w is None else "0x%08X" % w}, not a function start: '
                      'not a vtable (the table is read from the image: out/harness/index/image.bin)')
    total = len(tbl)
    if only is not None:
        if only < 0 or only >= total:
            raise VtError(f'0x{base:08X}: slot {only} is outside the table ({total} slots)')
        tbl = [tbl[only]]
    else:
        tbl = tbl[:nslots]
    rows, sums = [], {}
    for slot, at, tgt in tbl:
        if summary and tgt not in sums:
            sums[tgt] = summary_of(con, tgt)
        rows.append({'slot': slot, 'offset': slot * 4, 'word': f'0x{at:08X}', 'target': fname(con, tgt),
                     'target_addr': f'0x{tgt:08X}', 'db_name': names.get(tgt),
                     'summary': sums.get(tgt) if summary else None})
    return {'vtable': f'0x{base:08X}', 'table_slots': total, 'shown': len(rows), 'slots': rows}


def has_tables(con):
    """an index built before S110 ICALL has no `tables` catalog (the run-start heuristic below answers for it, said in the row)"""
    return con.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'tables'").fetchone() is not None


def of_func(con, img, addr):
    names = db_names()
    rows = []
    cat = has_tables(con)
    for r in con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func' ORDER BY at", (addr,)).fetchall():
        at = r['at']
        v = con.execute('''SELECT v.vt, v.nslots, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td
                           WHERE v.vt <= ? ORDER BY v.vt DESC LIMIT 1''', (at,)).fetchone()
        t = con.execute('SELECT start, rule, len FROM tables WHERE start <= ? ORDER BY start DESC LIMIT 1', (at,)).fetchone() if cat else None
        if t and at < t['start'] + 4 * t['len']:      # S110 ICALL: the table catalog (build_index.tables) bounds the table
            vt, how, cls = t['start'], f'table ({t["rule"]} start)', v['name'] if v and v['vt'] == t['start'] else None
        elif v and at < v['vt'] + 4 * max(1, v['nslots']):
            vt, how, cls = v['vt'], 'rtti', v['name']
        else:
            vt = at
            while at - vt < 4 * MAX_SLOTS and is_func(con, img.word(vt - 4)):
                vt -= 4
            how, cls = 'run-start (heuristic)', None
        rows.append({'vtable': f'0x{vt:08X}', 'slot': (at - vt) // 4, 'offset': at - vt, 'word': f'0x{at:08X}',
                     'table_by': how, 'class': cls})
    return {'function': fname(con, addr), 'addr': f'0x{addr:08X}', 'db_name': names.get(addr), 'points': rows, 'tables_catalog': cat}


def main(argv, con, resolve):
    ap = argparse.ArgumentParser(prog='q vtable', description=__doc__.split('\n\n')[0])
    ap.add_argument('addr', nargs='?', help='first slot of the table (+0)')
    ap.add_argument('--of', metavar='FUNC', help='reverse: every vtable slot that points at FUNC')
    ap.add_argument('--slots', type=int, default=64, metavar='N', help='show the first N slots (default 64)')
    ap.add_argument('--slot', type=int, default=None, metavar='K', help='show only slot K')
    ap.add_argument('--no-summary', action='store_true', help='skip the q decomp --fields line (faster on big tables)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    import q as qm
    img = Img(os.path.join(os.path.dirname(qm.DB), 'image.bin'))
    if a.of:
        addr, note = resolve(con, a.of)
        r = of_func(con, img, addr)
        if a.json:
            print(json.dumps(r, indent=1))
            return 0
        if note:
            print(f'({note})')
        if not r['tables_catalog']:
            print('(this index has no `tables` catalog (built before S110 ICALL): the bounds below are the old RTTI / run-start heuristics and can name the wrong table '
                  'and slot; rebuild: py tools\\q\\build_index.py)')
        print(f'{r["function"]} 0x{addr:08X}' + (f'  [{r["db_name"]}]' if r['db_name'] else '') + f': {len(r["points"])} data word(s) point at it')
        for p in r['points']:
            print(f'  vtable {p["vtable"]} slot {p["slot"]} (+{p["offset"]})  word {p["word"]}  table by {p["table_by"]}' + (f'  class {p["class"]}' if p['class'] else ''))
        return 0
    if not a.addr:
        raise VtError('usage: q vtable ADDR [--slots N] [--slot K] | q vtable --of FUNC')
    r = slots_of(con, img, parse_addr(a.addr), a.slots, a.slot, not a.no_summary)
    if a.json:
        print(json.dumps(r, indent=1))
        return 0
    print(f'vtable {r["vtable"]}: {r["table_slots"]} slots (ends at the first non-function word); showing {r["shown"]}')
    for s in r['slots']:
        print(f'  slot {s["slot"]:>3} (+{s["offset"]:<4}) word {s["word"]}  -> {s["target"]}' + (f'  [{s["db_name"]}]' if s['db_name'] else ''))
        if s['summary']:
            print(f'        {s["summary"]}')
    return 0
