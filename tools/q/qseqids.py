"""q seqids [--json]   (S111 CALLARGS): every sequence constructor -> the sequence id it passes to the base ctor sub_82C3A298 in r4.

= `q callargs sub_82C3A298 r4` with each calling function taken as the constructor, joined to the vtable(s) the field index records it
storing (the last one a ctor stores is its own class, an earlier one a base class's), sorted by sequence id. A ctor that passes more
than one id, or a value the walk cannot make constant, is printed with its set / its definition, never guessed.
"""
import json
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qcallargs  # noqa: E402
import qslice  # noqa: E402

BASE_CTOR = 0x82C3A298


class SeqIdsError(Exception):
    pass


def vtables_of(src, func):
    db = src.fdb()
    if not db:
        return []
    return sorted({v for (v,) in db.execute('SELECT vt FROM vstores WHERE func = ?', (func,))})


def seqids(src, base=BASE_CTOR, con=None):
    """-> [{'ctor', 'ctor_addr', 'site', 'ids': [...], 'kind', 'def', 'vtables': [...]}] sorted by id"""
    res = qcallargs.callargs(src, base, ['r4'], con=con, indirect=False)
    rows = []
    for s in res['sites']:
        a = s['args']['r4']
        rows.append({'ctor': s['caller'], 'ctor_addr': s['caller_addr'], 'site': s['site'], 'kind': a['kind'], 'ids': a['values'],
                     'def': a['unknown'][0] if a['unknown'] else '', 'vtables': vtables_of(src, s['caller_addr'])})
    rows.sort(key=lambda r: (r['ids'][0] if r['ids'] else 10 ** 6, r['ctor_addr']))
    return rows


def print_table(rows, out=print):
    out(f'q seqids: {len(rows)} sequence constructors (callers of sub_{BASE_CTOR:08X}, r4 = the sequence id)')
    out(f'{"id":>6}  {"ctor":<16} {"call site":<12} vtable(s)')
    for r in rows:
        ids = '/'.join(str(i) for i in r['ids']) if r['ids'] else '?'
        if r['kind'] == 'unknown' or r['def']:
            ids += f' (? {r["def"]})' if r['def'] else ''
        out(f'{ids:>6}  {r["ctor"]:<16} 0x{r["site"]:08X}   ' + (', '.join(f'0x{v:08X}' for v in r['vtables']) or '-'))


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q seqids', description=__doc__.split('\n')[0])
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    src = qslice.QSource(con, use_cache=not a.no_cache)
    rows = seqids(src, con=con)
    if a.json:
        print(json.dumps([{**r, 'ctor_addr': f'0x{r["ctor_addr"]:08X}', 'site': f'0x{r["site"]:08X}',
                           'vtables': [f'0x{v:08X}' for v in r['vtables']]} for r in rows], indent=1))
    else:
        print_table(rows)
    return 0
