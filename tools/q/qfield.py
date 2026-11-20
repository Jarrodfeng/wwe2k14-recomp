r"""q field: who reads and who writes offset +N of an object, across the whole recompiled game (S103 lane QF).

    py tools/q/q.py field OFF [--type T | --vtable V | --global G | --root R] [--writes | --reads] [--lea]
                              [--func F] [--width N] [--float | --int] [--limit N] [--json]
    py tools/q/q.py field fn FUNC          the fields a function touches (root, offset, R / W counts, widths, types)
    py tools/q/q.py field type T           the layout seen for a type: offset, widths, int / float, readers / writers
    py tools/q/q.py field info | build [--rebuild]

OFF: decimal or hex (0x6C), negative ok. It is the EFFECTIVE offset (the register's folded delta + the displacement), so
`addi r11,r3,600` then `16(r11)` is 616 on a0. Root 'K' (a constant address) rows carry the absolute address as OFF.
  --type T      T = a vtable address (0x8201F294), an RTTI class-name substring, or ctor:sub_X (the type a constructor stores)
  --vtable V    the same, address only
  --global G    base = the pointer stored at the global word G (`lis; lwz` fold): `field 32 --global 0x83930050` = [0x83930050]+32
  --root R      a root exactly as printed: a0 | g:83930050 | p(a0,8) | r:82250010 | K | u
Roots: aN = argument N at entry, g:ADDR = pointer loaded from the global word ADDR, p(ROOT,D) = pointer loaded from ROOT+D,
r:SITE = the return value of the call at SITE, K = absolute address, u = base the fold could not name.
Types come from the vptr a constructor stores at +0 (see build_fields.py): a function that is a vtable slot has a0 of that
type; a constructor call types its argument and its return. `how` = vptr | slot | ctor_call | ctor_ret | global.
The index (out/harness/index/fields.s<SCHEMA>.db, ~1 GB-class cache, rebuilt on first use after generated/ or image.bin change) is
read-only here. Not modelled: indexed accesses (lwzx...), types passed to callees, subobject vptrs at non-zero offsets.
A hit is a lead to confirm in the listing (tools/analysis/list_function.py), never a proof by itself (CLAUDE.md rule 4).
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402


class FieldError(Exception):
    pass


def h(a):
    return '0x%08X' % a if a is not None else '?'


def parse_num(s):
    try:
        return int(s, 0)
    except ValueError:
        raise FieldError(f'not a number: {s!r}')


def parse_func(s):
    s = s.strip()
    m = re.match(r'^(?:sub_)?(?:0x)?([0-9A-Fa-f]{8})$', s)
    if not m:
        raise FieldError(f'FUNC must be sub_XXXXXXXX or an 8-digit hex address, got {s!r}')
    return int(m.group(1), 16)


def open_db(a, quiet=False):
    gen, image, db = bf.default_paths(a.root, a.gen)
    image = a.image or image
    db = a.db or db
    if not getattr(a, 'no_rebuild', False):
        sig = bf.signature(gen, image)
        forced = getattr(a, 'rebuild', False)
        waited = False
        t_wait = 0.0
        while forced or bf.current_signature(db) != sig:
            say = None if quiet else 'q field: building the field index (generated/ or image changed, or no index yet) ...'
            try:
                bf.build(gen, image, db, quiet=quiet, say=say, if_stale=not forced)      # S105 D (F2): one rebuilder at a time (db.lock); the others wait or answer from the old file
                break
            except bf.Busy as e:
                if os.path.exists(db):
                    print(f'q field: answering from the old index: another process (pid {e.holder}) is rebuilding the field index (lock {db}.lock)', file=sys.stderr)
                    break
                if not waited:
                    print(f'q field: no index yet and another process (pid {e.holder}) is building it: waiting for it', file=sys.stderr)
                    waited = True
                    t_wait = time.time()
                if time.time() - t_wait > float(os.environ.get('WWE_QFIELD_WAIT', '90')):      # S105 D (F-1): never wait past an agent's tool timeout
                    raise FieldError(f'pid {e.holder} is still building the field index ({db}): try again in a minute')
                forced = False
                time.sleep(0.5)
    if not os.path.exists(db):
        raise FieldError(f'no field index at {db}: py tools\\q\\build_fields.py')
    con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    return con


_NAMES = {}


def fname(f):
    if not _NAMES:
        _NAMES[0] = None
        try:
            import q as qmod
            for addr, _file, e in qmod.load_db_entries():
                _NAMES[addr] = e.get('name')
        except Exception:
            pass
    n = _NAMES.get(f)
    return f'sub_{f:08X}' + (f' ({n})' if n else '')


def rtti_names(index_db_dir):
    """vtable address -> RTTI class name from index.db (optional)."""
    out = {}
    p = os.path.join(index_db_dir, 'index.db')
    if not os.path.exists(p):
        return out
    try:
        c = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
        for vt, name in c.execute('select v.vt, c.name from rtti_vtable v join rtti_class c on c.td=v.td'):
            out[vt] = name
        c.close()
    except sqlite3.Error:
        pass
    return out


def resolve_types(con, a, spec):
    """spec -> list of vtable addresses (known to the field index)."""
    known = {r[0] for r in con.execute('select vt from vtables')}
    if spec.lower().startswith('ctor:'):
        f = parse_func(spec[5:])
        r = con.execute('select vt from ctors where func=?', (f,)).fetchone()
        if not r:
            raise FieldError(f'{spec}: that function stores no vptr at a0+0 (not a constructor in the index)')
        return [r[0]]
    if re.match(r'^(0x)?[0-9A-Fa-f]{8}$', spec):
        v = int(spec, 16)
        if v not in known:
            raise FieldError(f'{h(v)} is not a vtable the index saw a constructor store (q field info lists the count; '
                             f'q rtti NAME finds class vtables)')
        return [v]
    _gen, image, db = bf.default_paths(a.root, a.gen)
    names = rtti_names(os.path.dirname(a.image or image))
    hits = sorted(v for v, n in names.items() if spec.lower() in (n or '').lower() and v in known)
    if not hits:
        raise FieldError(f'--type {spec!r}: no RTTI class name containing it among the {len(known)} typed vtables '
                         f'(untyped vtables have no RTTI name: pass the vtable address)')
    return hits


TYPE_SQL = ('(exists (select 1 from objtype o where o.vt in ({vts}) and o.root=r.root and (o.func=r.func or o.func=0){strict}))')
# a row whose base is an argument (aN) that callers pass a global in: matches that global (inferred from callers)
FLOW_SQL = ('exists (select 1 from argflow af join roots gr on gr.id=af.groot where af.func=r.func and af.argroot=r.root and gr.text=?)')


def strict_sql(a):
    return " and o.how not in ('callarg')" if a.strict else ''


def build_where(con, a, off):
    where, args = [], []
    if off is not None:
        where.append('r.off=?'); args.append(off)
    rw = []
    if a.writes:
        rw.append('W')
    if a.reads:
        rw.append('R')
    if a.lea:
        rw.append('A')
    if not rw:
        rw = ['R', 'W']
    where.append('r.rw in (%s)' % ','.join('?' * len(rw))); args += rw
    spec = a.type or a.vtable
    if spec:
        vts = resolve_types(con, a, spec)
        where.append(TYPE_SQL.format(vts=','.join(str(v) for v in vts), strict=strict_sql(a)))
    gtext = 'g:%08X' % parse_num(a.glob) if a.glob else a.rootname if a.rootname and a.rootname.startswith('g:') else None
    if a.glob:
        where.append('(ro.text=?' + ('' if a.strict else ' or ' + FLOW_SQL) + ')'); args.append(gtext)
        if not a.strict:
            args.append(gtext)
    if a.rootname and not a.glob:
        if gtext and not a.strict:
            where.append('(ro.text=? or ' + FLOW_SQL + ')'); args += [a.rootname, a.rootname]
        else:
            where.append('ro.text=?'); args.append(a.rootname)
    if a.func:
        where.append('r.func=?'); args.append(parse_func(a.func))
    if a.width:
        where.append('r.width=?'); args.append(a.width)
    if a.float:
        where.append('r.fl=1')
    if a.int:
        where.append('r.fl=0 and r.rw<>"A"')
    return where, args


def types_of(con, func, root_id, names):
    out = []
    for (g,) in con.execute('select gr.text from argflow af join roots gr on gr.id=af.groot where af.func=? and af.argroot=? limit 4',
                            (func, root_id)):
        out.append(f'via callers: {g}')
    for vt, how in con.execute('select vt, how from objtype where root=? and (func=? or func=0)', (root_id, func)):
        out.append(f'{h(vt)}' + (f' {names[vt]}' if vt in names else '') + f' [{how}]')
    return out


def cmd_off(con, a, out):
    off = parse_num(a.arg)
    where, args = build_where(con, a, off)
    sql = ('select r.func, r.addr, r.rw, r.width, r.fl, r.off, ro.text, r.root from rows r join roots ro on ro.id=r.root '
           'where ' + ' and '.join(where) + ' order by r.func, r.addr limit ?')
    rows = con.execute(sql, args + [a.limit]).fetchall()
    _gen, image, _db = bf.default_paths(a.root, a.gen)
    names = rtti_names(os.path.dirname(a.image or image))
    res = []
    for f, addr, rw, w, fl, o, root, rid in rows:
        res.append({'func': f'sub_{f:08X}', 'addr': h(addr), 'rw': rw, 'width': w, 'float': bool(fl), 'off': o, 'root': root,
                    'types': types_of(con, f, rid, names)})
    out['rows'] = res
    out['count'] = len(res)
    out['functions'] = len({r['func'] for r in res})
    out['truncated'] = len(res) >= a.limit
    out['note'] = 'indexed accesses (lwzx...) are not indexed; confirm each hit in the listing'
    return out


def print_off(out, js):
    if js:
        print(json.dumps(out, indent=1))
        return
    cur = None
    for r in out['rows']:
        if r['func'] != cur:
            cur = r['func']
            print(fname(int(cur[4:], 16)))
        t = ('  ' + '; '.join(r['types'])) if r['types'] else ''
        print(f"  {r['addr']}  {r['rw']} w{r['width']}{'f' if r['float'] else ''} +{r['off']}  root {r['root']}{t}")
    print(f"-- {out['count']} accesses in {out['functions']} functions" + (' (truncated: --limit)' if out['truncated'] else ''),
          file=sys.stderr)


def cmd_fn(con, a, out):
    f = parse_func(a.arg)
    _gen, image, _db = bf.default_paths(a.root, a.gen)
    names = rtti_names(os.path.dirname(a.image or image))
    q = ('select ro.text, r.off, r.rw, r.width, r.fl, count(*), min(r.addr), r.root from rows r join roots ro on ro.id=r.root '
         'where r.func=? group by r.root, r.off, r.rw, r.width, r.fl order by ro.text, r.off, r.rw')
    res = []
    for root, off, rw, w, fl, n, first, rid in con.execute(q, (f,)):
        res.append({'root': root, 'off': off, 'rw': rw, 'width': w, 'float': bool(fl), 'count': n, 'first': h(first),
                    'types': types_of(con, f, rid, names)})
    out['func'] = f'sub_{f:08X}'
    out['fields'] = res
    ct = con.execute('select vt from ctors where func=?', (f,)).fetchone()
    out['constructor_of'] = h(ct[0]) if ct else None
    out['slot_of'] = [h(r[0]) + f'[{r[1]}]' for r in con.execute('select vt, idx from slots where func=?', (f,))]
    return out


def print_fn(out, js):
    if js:
        print(json.dumps(out, indent=1))
        return
    print(fname(int(out['func'][4:], 16)))
    if out['constructor_of']:
        print(f"  constructor of vtable {out['constructor_of']}")
    if out['slot_of']:
        print(f"  vtable slot: {', '.join(out['slot_of'][:6])}" + (' ...' if len(out['slot_of']) > 6 else ''))
    cur = None
    for r in out['fields']:
        if r['root'] != cur:
            cur = r['root']
            t = ('  ' + '; '.join(r['types'])) if r['types'] else ''
            print(f'  root {cur}{t}')
        print(f"    +{r['off']:<6} {r['rw']} w{r['width']}{'f' if r['float'] else ''} x{r['count']}  first {r['first']}")


def cmd_type(con, a, out):
    vts = resolve_types(con, a, a.arg)
    _gen, image, _db = bf.default_paths(a.root, a.gen)
    names = rtti_names(os.path.dirname(a.image or image))
    sql = ('select r.off, r.rw, r.width, r.fl, count(*), count(distinct r.func), min(r.func) from rows r join roots ro on ro.id=r.root '
           'where r.rw<>"A" and ' + TYPE_SQL.format(vts=','.join(str(v) for v in vts), strict=strict_sql(a)) + ' group by r.off, r.rw, r.width, r.fl '
           'order by r.off, r.rw')
    lay = {}
    for off, rw, w, fl, n, nf, f0 in con.execute(sql):
        e = lay.setdefault(off, {'off': off, 'widths': set(), 'float': set(), 'readers': 0, 'writers': 0, 'reads': 0, 'writes': 0,
                                 'eg_r': None, 'eg_w': None})
        e['widths'].add(w)
        e['float'].add(bool(fl))
        if rw == 'R':
            e['reads'] += n; e['readers'] = max(e['readers'], nf); e['eg_r'] = e['eg_r'] or f0
        else:
            e['writes'] += n; e['writers'] = max(e['writers'], nf); e['eg_w'] = e['eg_w'] or f0
    out['types'] = [{'vt': h(v), 'name': names.get(v), 'constructors': [f'sub_{r[0]:08X}' for r in
                     con.execute('select func from ctors where vt=?', (v,))],
                     'slots': con.execute('select nslots from vtables where vt=?', (v,)).fetchone()[0]} for v in vts]
    out['layout'] = [{'off': e['off'], 'widths': sorted(e['widths']), 'kind': 'float' if e['float'] == {True} else
                      ('int' if e['float'] == {False} else 'both'), 'readers': e['readers'], 'writers': e['writers'],
                      'reads': e['reads'], 'writes': e['writes'],
                      'e_reader': f"sub_{e['eg_r']:08X}" if e['eg_r'] else None,
                      'e_writer': f"sub_{e['eg_w']:08X}" if e['eg_w'] else None} for _o, e in sorted(lay.items())]
    out['note'] = 'readers / writers = distinct functions (per width/kind group, max shown); the type is seen only where the fold typed the base'
    return out


def print_type(out, js):
    if js:
        print(json.dumps(out, indent=1))
        return
    for t in out['types']:
        print(f"type {t['vt']} {t['name'] or ''}  ctors {', '.join(t['constructors']) or '-'}  slots {t['slots']}")
    for e in out['layout']:
        print(f"  +{e['off']:<6} w{'/'.join(map(str, e['widths']))} {e['kind']:5} readers {e['readers']:<3} writers {e['writers']:<3} "
              f"eg R {e['e_reader'] or '-'}  W {e['e_writer'] or '-'}")


def cmd_info(con, a, out):
    out['meta'] = {k: v for k, v in con.execute("select k, v from meta where k<>'signature'")}
    for t in ('rows', 'roots', 'vtables', 'slots', 'ctors', 'objtype'):
        out[t] = con.execute(f'select count(*) from {t}').fetchone()[0]
    out['db'] = bf.default_paths(a.root, a.gen)[2] if not a.db else a.db
    out['other_index_files'] = bf.other_index_files(out['db'])      # S105 D: other-schema files beside ours, never opened or rebuilt by this q
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(prog='q field', description=__doc__.split('\n')[0])
    ap.add_argument('arg', nargs='?', help='OFF | fn | type | info | build; then the argument of fn / type')
    ap.add_argument('arg2', nargs='?')
    ap.add_argument('--type'); ap.add_argument('--vtable'); ap.add_argument('--global', dest='glob')
    ap.add_argument('--root-name', dest='rootname', help='a root as printed (a0, g:83930050, p(a0,8), K)')
    ap.add_argument('--writes', action='store_true'); ap.add_argument('--reads', action='store_true')
    ap.add_argument('--lea', action='store_true')
    ap.add_argument('--strict', action='store_true', help='drop what is only inferred from callers (how=callarg, via callers)')
    ap.add_argument('--func'); ap.add_argument('--width', type=int)
    ap.add_argument('--float', action='store_true'); ap.add_argument('--int', action='store_true')
    ap.add_argument('--limit', type=int, default=500); ap.add_argument('--json', action='store_true')
    ap.add_argument('--rebuild', action='store_true')
    ap.add_argument('--root'); ap.add_argument('--gen'); ap.add_argument('--image'); ap.add_argument('--db')
    ap.add_argument('--no-rebuild', dest='no_rebuild', action='store_true')
    a = ap.parse_args(argv)
    con = None
    try:
        if a.arg is None:
            ap.error('give OFF, fn FUNC, type T, info or build')
        if a.arg == 'build':
            gen, image, db = bf.default_paths(a.root, a.gen)
            try:
                bf.build(gen, a.image or image, a.db or db)
            except bf.Busy as e:
                print(f'q field: another process (pid {e.holder}) is rebuilding {a.db or db}: not building in parallel', file=sys.stderr)
                return 3
            return 0
        con = open_db(a)
        out = {}
        if a.arg == 'info':
            cmd_info(con, a, out)
            print(json.dumps(out, indent=1))
            return 0
        if a.arg == 'fn':
            a.arg = a.arg2 or ap.error('fn needs FUNC')
            print_fn(cmd_fn(con, a, out), a.json)
            return 0
        if a.arg == 'type':
            a.arg = a.arg2 or ap.error('type needs T')
            print_type(cmd_type(con, a, out), a.json)
            return 0
        print_off(cmd_off(con, a, out), a.json)
        return 0
    except FieldError as e:
        if a.json:
            print(json.dumps({'error': str(e)}))
        else:
            print(f'q field: {e}', file=sys.stderr)
        return 2
    finally:
        if con is not None:
            con.close()      # S105 D: an open reader makes a rebuilder's replace of the file fail on Windows


if __name__ == '__main__':
    sys.exit(main())
