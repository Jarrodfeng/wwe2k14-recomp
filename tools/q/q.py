"""Query the code index (out/harness/index/index.db, built by tools/q/build_index.py). Session 8, task 5.

    py tools/q/q.py [--json] COMMAND ...

  callers FUNC                 who calls / tail-calls / branches into FUNC; data words pointing at it; vtable slots
  callees FUNC [--depth N]     direct callees (tree for N > 1); --all keeps __savegprlr_/__restgprlr_ helpers
  str REGEX [-i]               strings matching REGEX and the functions referencing them (+ data pointers to them)
  imm VALUE                    functions using an immediate (lis pairs give full 32-bit values)
  rtti NAME [--slots]          RTTI classes whose name contains NAME -> vtables -> slot functions, base classes
  addr ADDR | --host X         guest address -> function + offset (+ string/data info); --host wwe2k14.exe+0x..
                               or a host RVA -> host symbol -> guest function (needs the map of THAT build)
  db FUNC                      db/*.json entry + system + docs/systems|modules mentions (read live; FUNC may also
                               be a data address or name that has a db entry)
  fn FUNC                      one-screen summary: size, callers/callees, strings, constants, RTTI, db, coverage
  info                         what the index was built from, counts, staleness

FUNC: sub_82CB1F50 | 0x82CB1F50 | 82cb1f50 | a mapped name (__imp__XamInputGetState, xstart) | a db name
(gpu_wait_poll_step). An address inside a function resolves to that function (noted).
VALUE/ADDR: 0x... and anything with a hex letter are hex; a bare 8-digit token is hex (an address); for `addr` every
bare token is hex; for `imm` a bare 1-7 digit token is decimal (100, -1) and 010 is refused as ambiguous. The
output says how it was read.
imm/addr/str/fn hide uncertain lis pairs (the lis value may not reach the use on every path; op ends in '?') and
say how many; --maybe shows them.
Every command takes --json (machine-readable, for agents) and --limit N. Errors: {"error": ...} on stdout with
--json (else `q: ...` on stderr), exit 2; a bug in q itself: the same with "internal error: ...", exit 3.
"""
import argparse
import bisect
import glob
import json
import os
import re
import sqlite3
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
DB = os.path.join(REPO, 'out', 'harness', 'index', 'index.db')
COVERAGE = os.path.join(REPO, 'out', 'harness', 'coverage', 'coverage.json')
HELPER_RE = re.compile(r'^__(save|rest)(gpr|fpr|vmx)')
IMAGE_BASE = 0x82000000


class QError(Exception):
    pass


def h(a):
    return f'0x{a:08X}' if a is not None else '?'


def connect(path):
    if not os.path.exists(path):
        raise QError(f'no index at {path}: build it with  py tools\\q\\build_index.py')
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    con.row_factory = sqlite3.Row
    return con


def meta(con):
    return {r['key']: json.loads(r['value']) for r in con.execute('SELECT key, value FROM meta')}


def staleness(m, keys=('init_cpp', 'map', 'image')):
    """Warnings when the sources changed after the index was built (cheap: size/mtime + codegen fingerprint)."""
    warn = []
    gen = m.get('gen_dir') or os.path.join(REPO, 'generated', 'default')
    try:
        with open(os.path.join(gen, 'codegen.stamp'), encoding='utf-8') as f:
            fp = json.load(f).get('fingerprint', '')
        if fp != m.get('codegen_fingerprint'):
            warn.append(f'codegen changed since the index was built (fingerprint {m.get("codegen_fingerprint")} -> {fp})')
    except (OSError, ValueError):
        warn.append('cannot read codegen.stamp')
    for key in keys:
        sig = m.get(key)
        if not sig:
            continue
        try:
            st = os.stat(sig['path'])
            if st.st_size != sig['size'] or int(st.st_mtime) != sig['mtime']:
                warn.append(f'{key} changed since the index was built: {sig["path"]}')
        except OSError:
            warn.append(f'{key} missing: {sig["path"]}')
    if warn:
        warn.append('rebuild: py tools\\q\\build_index.py (~10 s)')
    return warn


_DB_CACHE = []


def load_db_entries():
    """[(addr, file, entry)] from db/*.json, read live (cached per process). A file that cannot be parsed (e.g. being
    written by a labeller right now) is reported on stderr, never skipped silently: 'no db entry' must mean none."""
    if _DB_CACHE:
        return _DB_CACHE[0]
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, 'db', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            print(f'q: WARNING cannot parse {os.path.relpath(p, REPO)} ({e}): its entries are missing from this answer',
                  file=sys.stderr)
            continue
        if not isinstance(d, (list, dict)):
            continue
        for e in d if isinstance(d, list) else d.get('functions', []):
            if isinstance(e, dict) and 'address' in e:
                try:
                    a = int(str(e['address']), 16)
                except ValueError:
                    continue
                out.append((a, os.path.basename(p), e))
    _DB_CACHE.append(out)
    return out


def resolve(con, s, allow_inside=True, allow_data=False):
    """FUNC argument -> (addr, note). allow_data: also accept an address that only a db entry names (a global or
    table, not a function: `q db` of a data entry)."""
    s = s.strip()
    row = con.execute('SELECT addr FROM funcs WHERE name = ?', (s,)).fetchone()
    if row:
        return row['addr'], None
    m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', s)
    if m:
        a = int(m.group(1), 16)
        if con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone():
            return a, None
        if allow_data and any(x == a for x, _, _ in load_db_entries()):
            return a, f'{h(a)} is a db data address, not a function'
        if allow_inside:
            r = containing(con, a)
            if r:
                return r['addr'], f'{h(a)} is inside {r["name"]} (+0x{a - r["addr"]:X})'
        raise QError(f'{s}: not a function start and not inside any function (for data: q addr {h(a)})')
    for a, fname, e in load_db_entries():
        if e.get('name') == s:
            if allow_data or con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone():
                return a, f'db name {s} ({fname})'
            raise QError(f'{s} is a db entry for a data address ({h(a)}, {fname}), not a function: '
                         f'use  q db {s}  or  q addr {h(a)}')
    raise QError(f'{s}: unknown function (use sub_XXXXXXXX, an address, a mapped name or a db name)')


def containing(con, a):
    r = con.execute('SELECT * FROM funcs WHERE addr <= ? AND has_body = 1 ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
    if r and a < r['addr'] + r['size']:
        return r
    return None


def fname(con, a, cache={}):
    if a is None:
        return '?'
    if a not in cache:
        r = con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
        cache[a] = r['name'] if r else h(a)
    return cache[a]


# ---------------------------------------------------------------- commands

def cmd_callers(con, a, args):
    rows = con.execute('SELECT src, target, site, kind FROM edges WHERE dst = ? ORDER BY src, site', (a,)).fetchall()
    by = {}
    for r in rows:
        by.setdefault(r['src'], []).append({'site': h(r['site']), 'kind': r['kind'],
                                            'target': h(r['target']) if r['target'] != a else None})
    data = con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func' ORDER BY at", (a,)).fetchall()
    slots = rtti_slots_of(con, a)
    cov = load_coverage()
    covf = cov.get('functions', {}) if isinstance(cov, dict) and 'functions' in cov else None
    return {'func': fname(con, a), 'addr': h(a), 'n_sites': len(rows), 'n_callers': len(by),
            'callers': [{'func': fname(con, s), 'addr': h(s), 'sites': v,
                         **({'ran_in': covf.get(h(s), [])} if covf is not None else {})} for s, v in by.items()],
            'data_pointers': [h(r['at']) for r in data], 'vtable_slots': slots}


def print_callers(r, args):
    print(f'{r["func"]}: {r["n_sites"]} call sites in {r["n_callers"]} functions')
    for c in r['callers'][:args.limit]:
        sites = ', '.join(f'{s["site"]} {s["kind"]}' + (f' (into {s["target"]})' if s['target'] else '') for s in c['sites'])
        ran = f'  [ran: {",".join(c["ran_in"]) or "never seen"}]' if 'ran_in' in c else ''
        print(f'  {c["func"]:<28} {sites}{ran}')
    if len(r['callers']) > args.limit:
        print(f'  ... {len(r["callers"]) - args.limit} more (--limit)')
    if r['data_pointers']:
        dp = r['data_pointers']
        print(f'  data words pointing at it: {len(dp)}: ' + ', '.join(dp[:12]) + (' ...' if len(dp) > 12 else ''))
    for s in r['vtable_slots']:
        print(f'  vtable slot: {s["class"]} vtable {s["vtable"]} slot {s["slot"]}')


def rtti_slots_of(con, a):
    rows = con.execute('''SELECT s.vt, s.idx, c.name FROM rtti_slot s JOIN rtti_vtable v ON v.vt = s.vt
                          JOIN rtti_class c ON c.td = v.td WHERE s.func = ? ORDER BY c.name, s.vt''', (a,)).fetchall()
    return [{'class': r['name'], 'vtable': h(r['vt']), 'slot': r['idx']} for r in rows]


def callees_of(con, a, include_helpers):
    rows = con.execute('SELECT dst, target, site, kind FROM edges WHERE src = ? ORDER BY site', (a,)).fetchall()
    out = {}
    for r in rows:
        n = fname(con, r['dst'])
        if not include_helpers and HELPER_RE.match(n):
            continue
        out.setdefault(r['dst'], []).append({'site': h(r['site']), 'kind': r['kind'],
                                             'target': h(r['target']) if r['target'] != r['dst'] else None})
    return out


def cmd_callees(con, a, args):
    depth = max(1, args.depth)
    seen = {a}

    def walk(f, d):
        kids = []
        for dst, sites in callees_of(con, f, args.all).items():
            node = {'func': fname(con, dst), 'addr': h(dst), 'sites': sites}
            if d < depth and dst is not None and dst not in seen:
                seen.add(dst)
                node['callees'] = walk(dst, d + 1)
            elif d < depth and dst in seen:
                node['seen'] = True
            kids.append(node)
        return kids

    r = con.execute('SELECT n_bctrl, n_bctr FROM funcs WHERE addr = ?', (a,)).fetchone()
    return {'func': fname(con, a), 'addr': h(a), 'depth': depth, 'indirect_calls': r['n_bctrl'] if r else 0,
            'indirect_jumps': r['n_bctr'] if r else 0, 'callees': walk(a, 1)}


def print_callees(r, args):
    print(f'{r["func"]}: indirect calls (bctrl) {r["indirect_calls"]}, indirect jumps (bctr) {r["indirect_jumps"]}')
    count = [0]

    def show(nodes, ind):
        for n in nodes:
            count[0] += 1
            if count[0] > args.limit * max(1, r['depth']):
                return
            kinds = sorted({s['kind'] for s in n['sites']})
            extra = ' (seen above)' if n.get('seen') else ''
            into = [s['target'] for s in n['sites'] if s['target']]
            into = f' into {",".join(into)}' if into else ''
            print(f'{"  " * ind}{n["func"]}  x{len(n["sites"])} {"/".join(kinds)}{into}{extra}')
            if n.get('callees'):
                show(n['callees'], ind + 1)

    show(r['callees'], 1)
    if count[0] > args.limit * max(1, r['depth']):
        print('  ... (--limit)')


def cmd_str(con, pattern, args):
    flags = re.I if args.ignore_case else 0
    try:
        rx = re.compile(pattern, flags)
    except re.error as e:
        raise QError(f'bad regex: {e}')
    con.create_function('rx', 1, lambda t: 1 if t is not None and rx.search(t) else 0, deterministic=True)
    srows = con.execute('SELECT addr, enc, text FROM strings WHERE rx(text) ORDER BY addr').fetchall()
    out = []
    hidden = 0
    for s in srows:
        refs = con.execute(f'SELECT func, site, how, op FROM strrefs WHERE addr = ? AND {lis_op_filter(args)} '
                           'ORDER BY func, site', (s['addr'],)).fetchall()
        hidden += n_hidden_unsure(con, 'strrefs', 'addr = ?', (s['addr'],), args)
        dps = con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'str' ORDER BY at", (s['addr'],)).fetchall()
        out.append({'addr': h(s['addr']), 'enc': s['enc'], 'text': s['text'],
                    'refs': [{'func': fname(con, r['func']), 'addr': h(r['func']), 'site': h(r['site']),
                              'how': r['how'], 'op': r['op']} for r in refs],
                    'data_pointers': [h(d['at']) for d in dps]})
    return {'pattern': pattern, 'n_strings': len(out), 'strings': out,
            **({'uncertain_hidden': hidden} if hidden else {})}


def print_str(r, args):
    shown = r['strings'] if args.unref else [s for s in r['strings'] if s['refs'] or s['data_pointers']]
    hidden = len(r['strings']) - len(shown)
    print(f'{r["n_strings"]} strings match /{r["pattern"]}/' +
          (f' ({hidden} with no code or data reference hidden: --unref)' if hidden else ''))
    for s in shown[:args.limit]:
        enc = '' if s['enc'] == 'ascii' else ' [utf16be]'
        print(f'  {s["addr"]}{enc} {json.dumps(s["text"])[:110]}')
        funcs = {}
        for ref in s['refs']:
            funcs.setdefault(ref['func'], []).append(ref['site'] + ('' if ref['how'] == 'direct' else f' {ref["how"]}')
                                                     + (' ?' if ref['op'].endswith('?') else ''))
        for f, sites in funcs.items():
            print(f'      {f:<28} {", ".join(sites[:6])}{" ..." if len(sites) > 6 else ""}')
        if s['data_pointers']:
            dp = s['data_pointers']
            print(f'      data words pointing at it: {", ".join(dp[:6])}{" ..." if len(dp) > 6 else ""}')
    if len(shown) > args.limit:
        print(f'  ... {len(shown) - args.limit} more (--limit)')
    print_hidden(r)


def parse_value(s, kind='imm'):
    """VALUE/ADDR text -> u32. 0x... = hex; anything with a hex letter = hex; a bare 8-digit token = hex (an
    address, as FUNC reads it). For `addr` every bare token is hex. For `imm` a bare 1-7 digit token is decimal
    (-1, 100, 4096) and one with a leading zero (010) is refused as ambiguous. Returns (value, how it was read)."""
    t = s.strip()
    neg = t.startswith('-')
    body = t[1:] if neg else t
    if re.fullmatch(r'0[xX][0-9A-Fa-f]+', body):
        v, how = int(body, 16), 'hex'
    elif re.fullmatch(r'[0-9]+', body):
        if kind == 'addr' or len(body) == 8:
            v, how = int(body, 16), 'hex (bare 8-digit/address token)'
        elif len(body) > 1 and body[0] == '0':
            raise QError(f'{s}: ambiguous (leading zero): write 0x{body} for hex or {body.lstrip("0") or "0"} for decimal')
        else:
            v, how = int(body, 10), 'decimal'
    elif re.fullmatch(r'[0-9A-Fa-f]+', body):
        v, how = int(body, 16), 'hex'
    else:
        raise QError(f'{s}: not a number (hex 0x82130000 / 82130000, decimal 100, negative -1)')
    if neg:
        v = -v
    if v < -0x80000000 or v > 0xFFFFFFFF:
        raise QError(f'{s}: out of 32-bit range')
    return v & 0xFFFFFFFF, how


def lis_op_filter(args):
    """SQL condition on an `op` column: uncertain lis pairs (op ending in '?') only with --maybe."""
    return '1' if getattr(args, 'maybe', False) else "op NOT LIKE '%?'"


def n_hidden_unsure(con, table, where, params, args):
    if getattr(args, 'maybe', False):
        return 0
    return con.execute(f"SELECT COUNT(*) FROM {table} WHERE {where} AND op LIKE '%?'", params).fetchone()[0]


def cmd_imm(con, s, args):
    v, how = parse_value(s, 'imm')
    rows = con.execute(f'SELECT func, site, op FROM imms WHERE value = ? AND {lis_op_filter(args)} ORDER BY func, site',
                       (v,)).fetchall()
    hidden = n_hidden_unsure(con, 'imms', 'value = ?', (v,), args)
    by = {}
    for r in rows:
        by.setdefault(r['func'], []).append({'site': h(r['site']), 'op': r['op']})
    extra = {'read_as': how}
    if hidden:
        extra['uncertain_hidden'] = hidden
    if IMAGE_BASE <= v < IMAGE_BASE + 0x1A60000:
        st = con.execute('SELECT enc, text FROM strings WHERE addr = ?', (v,)).fetchone()
        if st:
            extra['string'] = st['text']
        c = containing(con, v)
        if c:
            extra['inside_function'] = f'{c["name"]}+0x{v - c["addr"]:X}'
    return {'value': h(v), 'signed': v - (1 << 32) if v & 0x80000000 else v, 'n_sites': len(rows),
            'n_funcs': len(by), 'funcs': [{'func': fname(con, f), 'addr': h(f), 'sites': s} for f, s in by.items()],
            **extra}


def print_imm(r, args):
    print(f'{r["value"]} ({r["signed"]}, read as {r["read_as"]}): {r["n_sites"]} uses in {r["n_funcs"]} functions'
          + (f'; is string {json.dumps(r["string"])[:80]}' if 'string' in r else '')
          + (f'; is code inside {r["inside_function"]}' if 'inside_function' in r else ''))
    for f in r['funcs'][:args.limit]:
        ops = ', '.join(f'{s["site"]} {s["op"]}' for s in f['sites'][:5])
        print(f'  {f["func"]:<28} {ops}{" ..." if len(f["sites"]) > 5 else ""}')
    if r['n_funcs'] > args.limit:
        print(f'  ... {r["n_funcs"] - args.limit} more (--limit)')
    print_hidden(r)


def print_hidden(r):
    if r.get('uncertain_hidden'):
        print(f'  ({r["uncertain_hidden"]} uncertain lis-pair uses hidden: the lis value may not reach them on every '
              f'path; --maybe shows them marked "?")')


def cmd_rtti(con, name, args):
    rows = con.execute('SELECT td, mangled, name FROM rtti_class WHERE name LIKE ? OR mangled LIKE ? ORDER BY name',
                       (f'%{name}%', f'%{name}%')).fetchall()
    out = []
    show_slots = args.slots or len(rows) <= 3
    for c in rows:
        vts = con.execute('SELECT vt, col, nslots FROM rtti_vtable WHERE td = ? ORDER BY vt', (c['td'],)).fetchall()
        vlist = []
        for v in vts:
            off = con.execute('SELECT offset FROM rtti_col WHERE col = ?', (v['col'],)).fetchone()
            item = {'vtable': h(v['vt']), 'col': h(v['col']), 'this_offset': off['offset'] if off else None,
                    'nslots': v['nslots']}
            if show_slots:
                item['slots'] = [{'idx': s['idx'], 'func': fname(con, s['func']), 'addr': h(s['func'])} for s in
                                 con.execute('SELECT idx, func FROM rtti_slot WHERE vt = ? ORDER BY idx', (v['vt'],))]
            vlist.append(item)
        bases = [b['base'] for b in con.execute('SELECT base FROM rtti_base WHERE cls = ? AND idx > 0 ORDER BY idx',
                                                (c['name'],))]
        out.append({'class': c['name'], 'mangled': c['mangled'], 'type_descriptor': h(c['td']),
                    'bases': bases, 'vtables': vlist})
    return {'query': name, 'n_classes': len(out), 'classes': out}


def print_rtti(r, args):
    print(f'{r["n_classes"]} RTTI classes match "{r["query"]}"')
    for c in r['classes'][:args.limit]:
        b = f'  bases: {", ".join(c["bases"])}' if c['bases'] else ''
        print(f'  {c["class"]}  (TD {c["type_descriptor"]}, {len(c["vtables"])} vtables){b}')
        for v in c['vtables']:
            print(f'    vtable {v["vtable"]} (this offset {v["this_offset"]}): {v["nslots"]} slots')
            for s in v.get('slots', [])[:args.limit]:
                print(f'      [{s["idx"]:>2}] {s["func"]}')
    if r['n_classes'] > args.limit:
        print(f'  ... {r["n_classes"] - args.limit} more (--limit)')


def parse_host(s, sections):
    """wwe2k14.exe+0xOFFSET, an RVA, or an absolute address at the preferred base 0x140000000. An address from an
    ASLR-relocated process (0x7FF6...) is refused: subtract the module base and pass wwe2k14.exe+0x...
    `sections`: [[rva_start, rva_end, name]] of the map (empty for an old index: then no range check)."""
    m = re.fullmatch(r'(wwe2k14\.exe\+)?(0x[0-9A-Fa-f]+|[0-9A-Fa-f]+)', s.strip())
    if not m:
        raise QError(f'{s}: expected wwe2k14.exe+0xOFFSET or an RVA')
    v = int(m.group(2), 16)
    image_end = max((x[1] for x in sections), default=None)
    if not m.group(1) and v >= 0x140000000:
        if image_end is not None and v - 0x140000000 >= image_end:
            raise QError(f'{s}: not inside the image at its preferred base 0x140000000 (image size 0x{image_end:X}). '
                         'An address from a running process is relocated (ASLR): pass wwe2k14.exe+0xOFFSET, i.e. the '
                         'address minus the module base shown in the crash log / debugger')
        v -= 0x140000000
    if sections and not any(lo <= v < hi for lo, hi, _ in sections):
        raise QError(f'{s}: RVA 0x{v:X} is not inside any section of the map (image size 0x{image_end:X}); '
                     'for an absolute address subtract the module base: wwe2k14.exe+0xOFFSET')
    return v


def cmd_addr(con, s, args, m):
    if args.host:
        if args.map:
            # another build's map (e.g. the one next to a crashed exe): parsed live, ~2 s
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from build_index import parse_map, map_timestamp
            sections = []
            try:
                syms = sorted(parse_map(args.map, sections))
                map_ts = map_timestamp(args.map)
            except OSError as e:
                raise QError(f'cannot read map {args.map}: {e.strerror or e}')
            if not syms:
                raise QError(f'no symbols parsed from {args.map}')
            rva = parse_host(s, sections)
            rvas = [x[0] for x in syms]
            k = bisect.bisect_right(rvas, rva) - 1
            if k < 0:
                raise QError('no host symbol at or below that RVA')
            r = {'rva': syms[k][0], 'name': syms[k][1], 'obj': syms[k][2]}
            nxt = next(({'rva': x[0]} for x in syms[k + 1:] if x[0] > r['rva']), None)
            map_path = args.map
        else:
            sections = m.get('map_sections') or []
            rva = parse_host(s, sections)
            r = con.execute('SELECT rva, name, obj FROM hostsyms WHERE rva <= ? ORDER BY rva DESC LIMIT 1', (rva,)).fetchone()
            if not r:
                raise QError('no host symbol at or below that RVA (is the map indexed?)')
            nxt = con.execute('SELECT rva FROM hostsyms WHERE rva > ? ORDER BY rva LIMIT 1', (r['rva'],)).fetchone()
            map_path, map_ts = (m.get('map') or {}).get('path'), (m.get('map') or {}).get('timestamp')
        sec = next((x for x in sections if x[0] <= rva < x[1]), None)
        if nxt is None and sec is None:
            raise QError(f'RVA 0x{rva:X} is past the last map symbol ({r["name"]} at 0x{r["rva"]:X}) and the map gives '
                         'no section extents to check it against: not resolvable')
        # the symbol's extent ends at the next symbol or at the end of its section, whichever comes first
        end = min([x for x in (nxt['rva'] if nxt else None, sec[1] if sec else None) if x is not None])
        if r['rva'] < (sec[0] if sec else r['rva']):
            raise QError(f'RVA 0x{rva:X} is in section {sec[2]} before its first symbol: no symbol to attribute it to')
        out = {'host_rva': f'0x{rva:X}', 'symbol': r['name'], 'symbol_rva': f'0x{r["rva"]:X}',
               'offset': f'0x{rva - r["rva"]:X}', 'obj': r['obj'],
               'symbol_size': f'0x{end - r["rva"]:X}', 'section': sec[2] if sec else None,
               'map': map_path, 'map_timestamp': map_ts,
               'caveat': 'host offsets are only valid against the map of the exact build that produced them'}
        mm = re.fullmatch(r'(?:__imp__)?(sub_[0-9A-F]{8})', r['name'])
        if mm:
            out['guest_function'] = mm.group(1)
        else:
            row = con.execute('SELECT addr FROM funcs WHERE name = ?', (r['name'].replace('__imp__', '', 1),)).fetchone()
            if row:
                out['guest_function'] = fname(con, row['addr'])
        return out
    a, how = parse_value(s, 'addr')
    out = {'addr': h(a)}
    c = containing(con, a)
    if c:
        out['function'] = c['name']
        out['offset'] = f'0x{a - c["addr"]:X}'
        out['func_size'] = c['size']
        out['where'] = f'{c["file"]}:{c["line"]}'
    else:
        f = con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
        if f:
            out['function'] = f['name']
            out['offset'] = '0x0'
    st = con.execute('SELECT addr, enc, text FROM strings WHERE addr <= ? ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
    if st and a < st['addr'] + len(st['text']) * (2 if st['enc'] == 'utf16be' else 1) + 1:
        out['string'] = {'addr': h(st['addr']), 'text': st['text'], 'offset': a - st['addr']}
    vt = con.execute('''SELECT v.vt, v.nslots, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td
                        WHERE v.vt <= ? ORDER BY v.vt DESC LIMIT 1''', (a,)).fetchone()
    if vt and a < vt['vt'] + 4 * max(1, vt['nslots']):
        out['vtable'] = {'class': vt['name'], 'vtable': h(vt['vt']), 'slot': (a - vt['vt']) // 4}
    p = con.execute('SELECT value, kind FROM ptrs WHERE at = ?', (a,)).fetchone()
    if p:
        out['word_points_to'] = fname(con, p['value']) if p['kind'] == 'func' else h(p['value'])
    refs = con.execute(f'SELECT func, site, op FROM imms WHERE value = ? AND op LIKE ? AND {lis_op_filter(args)} '
                       'ORDER BY func LIMIT 200', (a, 'lis+%')).fetchall()
    if refs:
        out['code_refs'] = [{'func': fname(con, r['func']), 'site': h(r['site']), 'op': r['op']} for r in refs]
    hidden = n_hidden_unsure(con, 'imms', "value = ? AND op LIKE 'lis+%'", (a,), args)
    if hidden:
        out['uncertain_hidden'] = hidden
    host = con.execute('SELECT rva FROM hostsyms WHERE name = ?', (out.get('function', ''),)).fetchone()
    if host:
        out['host_rva_of_function'] = f'0x{host["rva"]:X}'
    if len(out) == 1:
        out['note'] = 'not in any function, string, vtable or indexed data word'
    if how != 'hex':
        out['read_as'] = how
    return out


def print_kv(r, args):
    for k, v in r.items():
        if isinstance(v, list):
            print(f'{k}: {len(v)}')
            for x in v[:args.limit]:
                print(f'  {x}')
        else:
            print(f'{k}: {v}')


def docs_mentions(a):
    pats = [f'sub_{a:08X}', f'0x{a:08X}']
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, 'docs', 'systems', '*.md')) +
                    glob.glob(os.path.join(REPO, 'docs', 'modules', '*.md'))):
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = [i + 1 for i, line in enumerate(f) if any(x.lower() in line.lower() for x in pats)]
        except OSError:
            continue
        if lines:
            out.append({'doc': os.path.relpath(p, REPO), 'lines': lines})
    return out


def cmd_db(con, a, args):
    entries = [{'file': fn, **e} for x, fn, e in load_db_entries() if x == a]
    return {'func': fname(con, a), 'addr': h(a), 'entries': entries, 'docs': docs_mentions(a)}


def print_db(r, args):
    if not r['entries']:
        print(f'{r["func"]}: no db entry')
    for e in r['entries']:
        print(f'{r["func"]} = {e.get("name")}  [{e.get("system")}, confidence {e.get("confidence")}]  ({e["file"]})')
        print(f'  {e.get("summary")}')
        for ev in e.get('evidence', [])[:args.limit]:
            print(f'  - {ev}')
        if e.get('source'):
            print(f'  source: {e["source"]}')
    for d in r['docs']:
        print(f'  mentioned in {d["doc"]} lines {", ".join(map(str, d["lines"][:10]))}')


_COV = []


def load_coverage():
    """out/harness/coverage/coverage.json (task 6, tools/coverage/cov.py) or None; cached per process."""
    if not _COV:
        if not os.path.exists(COVERAGE):
            _COV.append(None)
        else:
            try:
                with open(COVERAGE, encoding='utf-8') as f:
                    _COV.append(json.load(f))
            except (OSError, ValueError) as e:
                _COV.append({'error': str(e)})
    return _COV[0]


def coverage_for(a):
    """Scenarios in which this function ran (coverage build). Format wwe2k14-coverage/1: functions{"0xADDR": [scenario]};
    anything else: generic search for the address anywhere in the JSON."""
    cov = load_coverage()
    if cov is None or 'error' in cov:
        return cov
    if str(cov.get('format', '')).startswith('wwe2k14-coverage/'):
        scen = cov.get('functions', {}).get(f'0x{a:08X}', [])
        return {'file': COVERAGE, 'format': cov['format'], 'built': cov.get('built'),
                'scenarios': scen, 'all_scenarios': list(cov.get('scenarios', {})),
                'hits': [{'path': s} for s in scen]}
    keys = {f'sub_{a:08X}', f'0x{a:08X}', f'{a:08X}', f'0x{a:08x}', f'{a:08x}', f'sub_{a:08x}'}
    hits = []

    def match(x):
        return (isinstance(x, str) and x in keys) or (isinstance(x, int) and not isinstance(x, bool) and x == a)

    def walk(node, path):
        if len(hits) >= 50:
            return
        if isinstance(node, dict):
            for k, v in node.items():
                if match(k):
                    hits.append({'path': '/'.join(path + [str(k)]), 'value': v if not isinstance(v, (dict, list)) else None})
                else:
                    walk(v, path + [str(k)])
        elif isinstance(node, list):
            for i, v in enumerate(node):
                if match(v):
                    hits.append({'path': '/'.join(path) or '/', 'value': None})
                else:
                    walk(v, path + [str(i)] if isinstance(v, (dict, list)) else path)

    walk(cov, [])
    return {'file': COVERAGE, 'hits': hits}


def cmd_fn(con, a, args):
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
    if f is None:
        raise QError(f'{h(a)} is not a function start (for data: q addr {h(a)} / q db {h(a)})')
    callers = cmd_callers(con, a, args)
    callees = callees_of(con, a, False)
    flt = lis_op_filter(args)
    strs = con.execute(f'''SELECT r.site, r.how, r.op, s.addr, s.text FROM strrefs r JOIN strings s ON s.addr = r.addr
                           WHERE r.func = ? AND {flt.replace('op', 'r.op')} ORDER BY r.site''', (a,)).fetchall()
    consts = con.execute(f'SELECT value, op, site FROM imms WHERE func = ? AND {flt} ORDER BY site', (a,)).fetchall()
    hidden = n_hidden_unsure(con, 'imms', "func = ? AND op LIKE 'lis+%'", (a,), args)
    cvals = {}
    for c in consts:
        cvals.setdefault((c['value'], c['op'].startswith('lis+')), []).append(c['op'])
    host = con.execute('SELECT rva FROM hostsyms WHERE name = ?', (f['name'],)).fetchone()
    dbe = cmd_db(con, a, args)
    return {
        'func': f['name'], 'addr': h(a), 'size': f['size'], 'insns': f['insns'],
        'where': f'generated/default/{f["file"]}:{f["line"]}' if f['file'] else None,
        'host_rva': f'0x{host["rva"]:X}' if host else None,
        'callers': {'n_sites': callers['n_sites'], 'n_funcs': callers['n_callers'],
                    'list': [c['func'] for c in callers['callers']]},
        'data_pointers': callers['data_pointers'], 'vtable_slots': callers['vtable_slots'],
        'callees': [{'func': fname(con, d), 'n': len(s), 'kinds': sorted({x['kind'] for x in s})} for d, s in callees.items()],
        'indirect_calls': f['n_bctrl'], 'indirect_jumps': f['n_bctr'],
        'strings': [{'site': h(s['site']), 'addr': h(s['addr']), 'text': s['text'], 'how': s['how'],
                     **({'uncertain': True} if s['op'].endswith('?') else {})} for s in strs],
        'addresses': [{'value': h(v), 'ops': sorted(set(o))} for (v, isaddr), o in cvals.items() if isaddr],
        'constants': [{'value': h(v), 'ops': sorted(set(o))} for (v, isaddr), o in cvals.items() if not isaddr],
        'db': dbe['entries'], 'docs': dbe['docs'], 'coverage': coverage_for(a),
        **({'uncertain_hidden': hidden} if hidden else {}),
    }


def print_fn(r, args):
    L = args.limit
    print(f'{r["func"]}  {r["addr"]}  size {r["size"]} ({r["insns"]} insns)  {r["where"] or "(no body: import/helper)"}'
          + (f'  host RVA {r["host_rva"]}' if r['host_rva'] else ''))
    for e in r['db']:
        print(f'  db: {e.get("name")} [{e.get("system")}, {e.get("confidence")}] ({e["file"]}): {e.get("summary", "")[:160]}')
    for d in r['docs']:
        print(f'  doc: {d["doc"]} lines {", ".join(map(str, d["lines"][:8]))}')
    c = r['callers']
    print(f'  callers: {c["n_sites"]} sites in {c["n_funcs"]} funcs: ' + ', '.join(c['list'][:L]) + (' ...' if len(c['list']) > L else ''))
    if r['data_pointers']:
        print(f'  data words pointing at it: {len(r["data_pointers"])}: ' + ', '.join(r['data_pointers'][:8]))
    for s in r['vtable_slots'][:L]:
        print(f'  vtable slot: {s["class"]} {s["vtable"]} [{s["slot"]}]')
    ce = r['callees']
    print(f'  callees ({len(ce)}; indirect calls {r["indirect_calls"]}, indirect jumps {r["indirect_jumps"]}): '
          + ', '.join(x['func'] + ('' if x['kinds'] == ['call'] else f'({"/".join(x["kinds"])})') for x in ce[:L])
          + (' ...' if len(ce) > L else ''))
    for s in r['strings'][:L]:
        print(f'  string @{s["site"]}: {json.dumps(s["text"])[:100]}' + ('' if s['how'] == 'direct' else f'  ({s["how"]})')
              + ('  ? uncertain' if s.get('uncertain') else ''))
    if len(r['strings']) > L:
        print(f'  ... {len(r["strings"]) - L} more strings')
    if r['addresses']:
        print('  lis pairs (addresses or 32-bit constants): ' + ', '.join(f'{x["value"]} {"/".join(x["ops"])}' for x in r['addresses'][:L])
              + (' ...' if len(r['addresses']) > L else ''))
    if r['constants']:
        print('  constants: ' + ', '.join(f'{x["value"]}' for x in r['constants'][:L * 2])
              + (' ...' if len(r['constants']) > L * 2 else ''))
    cov = r['coverage']
    if cov is None:
        print('  coverage: (no out/harness/coverage/coverage.json yet)')
    elif cov.get('error'):
        print(f'  coverage: unreadable ({cov["error"]})')
    elif 'scenarios' in cov:
        n = len(cov['all_scenarios'])
        print(f'  coverage: ran in {len(cov["scenarios"])} of {n} scenarios' +
              (': ' + ', '.join(cov['scenarios']) if cov['scenarios'] else ' (never seen running)') +
              f'  (coverage.json built {cov.get("built")})')
    else:
        print(f'  coverage: {len(cov["hits"])} hits: ' + ', '.join(x['path'] for x in cov['hits'][:L]))
    print_hidden(r)


def cmd_info(con, args, m):
    return {**m, 'db_path': DB, 'stale': staleness(m)}


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(prog='q', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true', help='machine-readable output')
    common.add_argument('--limit', type=int, default=25)
    common.add_argument('--index', default=DB)
    # the same options before the command (q.py --json fn X) as after it (q.py fn X --json)
    ap.add_argument('--json', dest='top_json', action='store_true', help='machine-readable output')
    ap.add_argument('--limit', dest='top_limit', type=int, default=None)
    ap.add_argument('--index', dest='top_index', default=None)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('callers', parents=[common]); p.add_argument('func')
    p = sub.add_parser('callees', parents=[common]); p.add_argument('func')
    p.add_argument('--depth', type=int, default=1); p.add_argument('--all', action='store_true')
    maybe = argparse.ArgumentParser(add_help=False)
    maybe.add_argument('--maybe', action='store_true',
                       help='also show uncertain lis pairs (op ending in "?": the lis value may not reach the use on '
                            'every path)')
    p = sub.add_parser('str', parents=[common, maybe]); p.add_argument('regex')
    p.add_argument('-i', '--ignore-case', action='store_true')
    p.add_argument('--unref', action='store_true', help='also list strings with no code/data reference')
    p = sub.add_parser('imm', parents=[common, maybe]); p.add_argument('value')
    p = sub.add_parser('rtti', parents=[common]); p.add_argument('name'); p.add_argument('--slots', action='store_true')
    p = sub.add_parser('addr', parents=[common, maybe]); p.add_argument('addr'); p.add_argument('--host', action='store_true')
    p.add_argument('--map', help='with --host: resolve against this linker map instead of the indexed one')
    p = sub.add_parser('db', parents=[common]); p.add_argument('func')
    p = sub.add_parser('fn', parents=[common, maybe]); p.add_argument('func')
    sub.add_parser('info', parents=[common])
    args = ap.parse_args(argv)
    args.json = args.json or args.top_json
    if args.top_limit is not None:
        args.limit = args.top_limit
    if args.top_index is not None:
        args.index = args.top_index
    try:
        con = connect(args.index)
        m = meta(con)
        # the map changes with every relink: only warn about it where host addresses are used
        keys = ('init_cpp', 'map', 'image') if args.cmd in ('addr', 'fn', 'info') else ('init_cpp', 'image')
        for w in staleness(m, keys):
            print(f'q: WARNING {w}', file=sys.stderr)
        if m.get('default_gen') is False:
            print(f'q: NOTE this index was built from {m.get("gen_dir")} (not generated/default)', file=sys.stderr)
        if args.cmd == 'callers':
            a, note = resolve(con, args.func); r = cmd_callers(con, a, args); pr = print_callers
        elif args.cmd == 'callees':
            a, note = resolve(con, args.func); r = cmd_callees(con, a, args); pr = print_callees
        elif args.cmd == 'str':
            note = None; r = cmd_str(con, args.regex, args); pr = print_str
        elif args.cmd == 'imm':
            note = None; r = cmd_imm(con, args.value, args); pr = print_imm
        elif args.cmd == 'rtti':
            note = None; r = cmd_rtti(con, args.name, args); pr = print_rtti
        elif args.cmd == 'addr':
            note = None; r = cmd_addr(con, args.addr, args, m); pr = print_kv
        elif args.cmd == 'db':
            a, note = resolve(con, args.func, allow_data=True); r = cmd_db(con, a, args); pr = print_db
        elif args.cmd == 'fn':
            a, note = resolve(con, args.func); r = cmd_fn(con, a, args); pr = print_fn
        else:
            note = None; r = cmd_info(con, args, m); pr = print_kv
        if note:
            r['note'] = note
        if args.json:
            print(json.dumps(r, indent=1))
        else:
            if note:
                print(f'({note})')
            pr(r, args)
        return 0
    except QError as e:
        if getattr(args, 'json', False):
            print(json.dumps({'error': str(e)}))
        else:
            print(f'q: {e}', file=sys.stderr)
        return 2
    except BrokenPipeError:   # `q ... | head`: the reader went away, not an error
        try:
            sys.stdout = open(os.devnull, 'w')
        except OSError:
            pass
        return 0
    except Exception as e:   # a bug in q itself: still one JSON object for agents (exit 3), traceback on stderr
        import traceback
        traceback.print_exc()
        if getattr(args, 'json', False):
            print(json.dumps({'error': f'internal error: {e!r}'}))
        else:
            print(f'q: internal error: {e!r}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(errors='replace')   # a db summary with a non-cp1252 character must not crash text output
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
