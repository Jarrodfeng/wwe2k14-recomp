r"""q gstate: per game function, the render-state SETTER calls it makes, with their constant arguments, in listing order (S111 CDSTATE,
piece 2 of the code-diff tool: the game-side render-state extractor; docs/research/s111/CDSTATE.md).

    py tools/q/q.py gstate sub_X [sub_Y ...] [--json]         the function's setter calls: +OFF, setter, value(s), save / restore pairs
    py tools/q/q.py gstate --setter KEY [--value V] [--all] [--json]   every function that calls that setter and with what
    py tools/q/q.py gstate --list [--json]                     the setter table: wrapper, recorder slot, cache word, record row, arguments
    py tools/q/q.py gstate sub_X --direct                      the one function read now, without the index (no forwarders)
    py tools/q/q.py gstate --facts [sub_X ...]                 the code-diff fact form (JSON): {side, fn, field, value, cond, at, kind}
    py tools/q/q.py gstate --inherit sub_X [--field F] [--depth N]   the state IN FORCE when it runs, up its callers (tools/q/qgstate_inherit.py)

The setters are the recorder wrappers (sub_82BA7238 + a vcall on the per-thread state recorder, vtable 0x8206C6D0: SETTERS below; the 11
of db/facts/enums.json `calls` with `state` are read from there), the texture record sub_82BB1750 (r4 texture, r5 sampler slot) and the
four-word save / apply pair sub_82BD5460 / sub_82BD54D0. A recorder GETTER (GETTERS) read in the same function is a SAVE; a setter call
whose value is that getter's result (directly, or through one stack slot) is a RESTORE of it, printed as `restore (+OFF)`.
Values: a constant is traced over the listing's paths (qrstdiff.flow: li / lis / addi / ori / mr; several paths = every literal, `0 | 1`),
else the decomp's fold of the register at the call (qdecomp `sites` regs) prints as `reg ROOT` (`reg ret#170`, `reg [in_r3+4]`, `reg in_r4`).
A function that passes its OWN argument register to a setter (`reg in_rK`) is a FORWARDER: its callers' value of rK is listed as that
setter `via` it (one level). Index: every function with a call / tail edge to a setter, cached in out/harness/index/gstate.<key>.json
(key = the codegen fingerprint + this file + qrstdiff.py + qdecomp's code + enums.json); built on first use.
Marks: the table rows carry their own; a row of the index is read from the listing [C tool], a forwarder row is [L tool] (one level).
"""
import collections
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402

INDEX_DIR = os.path.dirname(qdecomp.CACHE_DIR)            # out/harness/index (the main checkout's, like the decomp cache)
ENUMS = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'db', 'facts', 'enums.json')
VERSION = 1
CAP = 60

# The setters db/facts/enums.json does not carry (its `calls` rows with `state` are the other 11). Evidence: the recorder methods'
# decomps (S111 CDSTATE): cache word written, record row (the op-3 row offset the method passes sub_82C28798 / sub_82C26E00).
# args: (register, meaning); the arg named 'value' is the one a restore / forwarder is read from.
SETTERS = [
    {'fn': 0x82BB7C70, 'key': 'winding', 'what': 'cull winding', 'slot': 24, 'word': 116, 'row': 56, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C241A8: caches +116, records row 56 only while the cull enable +112 is set'},
    {'fn': 0x82BB7708, 'key': 'slot6', 'what': 'recorder slot 6', 'slot': 6, 'word': 72, 'row': 360, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C22CD0: caches +72, records row 360 (the device row\'s name not read: [U])'},
    {'fn': 0x82BB7760, 'key': 'stencil_twoside', 'what': 'stencil slot 9', 'slot': 9, 'word': 32, 'row': 112, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C23510: caches +32, records row 112 (two-sided stencil: the row name [L])'},
    {'fn': 0x82BB77A8, 'key': 'stencil_func', 'what': 'stencil function', 'slot': 10, 'word': None, 'row': None,
     'args': (('r4', 'face'), ('r5', 'value')), 'mark': 'C', 'source': 'sub_82C23580: caches +(8 + r4) * 4 = r5, records a row from r5 <= 7 by a table'},
    {'fn': 0x82BB7800, 'key': 'stencil_ops', 'what': 'stencil operations', 'slot': 11, 'word': None, 'row': None,
     'args': (('r4', 'face'), ('r5', 'value'), ('r6', 'op2'), ('r7', 'op3')), 'mark': 'C',
     'source': 'sub_82C23648: three cached words per face r4 (r5 / r6 / r7), each recorded through the table at 0x8206C784'},
    {'fn': 0x82BB7850, 'key': 'stencil_ref', 'what': 'stencil reference', 'slot': 12, 'word': 44, 'row': 132, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C22C80: caches +44, records rows 132 and 160'},
    {'fn': 0x82BB79B8, 'key': 'colour_mask_14', 'what': 'colour write mask group 14', 'slot': 14, 'word': 96, 'row': 212, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C23980: caches +96, records row 212 (the colour write mask) while +92 is set'},
    {'fn': 0x82BB7A00, 'key': 'colour_mask_15', 'what': 'colour write mask group 15', 'slot': 15, 'word': 100, 'row': 212, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C23A20: caches +100, records row 212 (vtable 0x8206C6D0 slot 15)'},
    {'fn': 0x82BB7A48, 'key': 'colour_mask_16', 'what': 'colour write mask group 16', 'slot': 16, 'word': 104, 'row': 212, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C23AC0: caches +104, records row 212 (vtable 0x8206C6D0 slot 16)'},
    {'fn': 0x82BB7A90, 'key': 'colour_mask_17', 'what': 'colour write mask group 17', 'slot': 17, 'word': 108, 'row': 212, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C23B60: caches +108, records row 212 (vtable 0x8206C6D0 slot 17)'},
    {'fn': 0x82BB7CB8, 'key': 'row304', 'what': 'recorder slot 25', 'slot': 25, 'word': 120, 'row': 304, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C22D70: caches +120, records row 304 (T2_render_states.md calls it the VP transform: [L])'},
    {'fn': 0x82BB7D00, 'key': 'row200', 'what': 'recorder slot 26', 'slot': 26, 'word': 124, 'row': 200, 'args': (('r4', 'value'),),
     'mark': 'C', 'source': 'sub_82C22E08: caches +124, records row 200'},
    {'fn': 0x82BB7B20, 'key': 'sampler_address', 'what': 'pixel sampler address mode', 'slot': 28, 'word': None, 'row': None,
     'args': (('r4', 'sampler'), ('r5', 'value')), 'mark': 'C',
     'source': 'sub_82C23D78: r5 = 7 reads as 2; caches +(37 + r4) * 4, records sampler r4 state offsets 0 / 4 (address U / V)'},
    {'fn': 0x82BB7B78, 'key': 'vertex_sampler_filter', 'what': 'vertex sampler filter', 'slot': 29, 'word': None, 'row': None,
     'args': (('r4', 'sampler'), ('r5', 'value')), 'mark': 'C',
     'source': 'sub_82C23E98: r5 = 6 reads as 2; caches +(53 + r4) * 4, records sampler r4 + 16 state offsets 16 / 20 / 24'},
    {'fn': 0x82BB7BD0, 'key': 'sampler_filter', 'what': 'pixel sampler filter', 'slot': 30, 'word': None, 'row': None,
     'args': (('r4', 'sampler'), ('r5', 'value')), 'mark': 'C',
     'source': 'sub_82C23FE0: r5 = 6 reads as 2; caches +(57 + r4) * 4, records sampler r4 state offsets 16 / 20 / 24 (min / mag / mip)'},
    {'fn': 0x82BB1750, 'key': 'texture', 'what': 'texture at a sampler slot', 'slot': None, 'word': None, 'row': None,
     'args': (('r4', 'texture'), ('r5', 'value')), 'mark': 'C',
     'source': 'sub_82BB1750: appends record 44 with r4 at +16 (the texture) and r5 at +20 (the slot): GEARLIQUID sub_8259F1E8 r5 = 14'},
    {'fn': 0x82BD5460, 'key': 'save4', 'what': 'save blend code, blend enable, alpha test, cull', 'slot': None, 'word': None, 'row': None,
     'args': (('r3', 'value'),), 'mark': 'C', 'kind': 'save',
     'source': 'sub_82BD5460: stores the getters of +16, +8, +12, +112 at [r3], [r3+4], [r3+8], [r3+12]'},
    {'fn': 0x82BD54D0, 'key': 'apply4', 'what': 'apply a saved blend code, blend enable, alpha test, cull', 'slot': None, 'word': None, 'row': None,
     'args': (('r3', 'value'),), 'mark': 'C',
     'source': 'sub_82BD54D0: calls the blend code, blend enable, alpha test and cull setters with [r3], [r3+4], [r3+8], [r3+12]'},
]
# The getters enums.json does not carry (its `getters` are the other 8): the recorder word each returns -> the setter key.
GETTERS = [
    {'fn': 0x82BB8278, 'key': 'z_func', 'word': 88, 'source': 'sub_82BB8278 returns +88 (slot 20 sub_82C237D8 caches it)'},
    {'fn': 0x82BB8228, 'key': 'stencil_enable', 'word': 28, 'source': 'sub_82BB8228 returns +28 (slot 5 sub_82C23490 caches it)'},
    {'fn': 0x82BB83C0, 'key': 'winding', 'word': 116, 'source': 'sub_82BB83C0 returns +116 (slot 24 sub_82C241A8 caches it)'},
    {'fn': 0x82BB82C8, 'key': 'colour_write_mask', 'word': 92, 'source': 'sub_82BB82C8 returns +92 (slot 13 sub_82C23910 caches it)'},
    {'fn': 0x82BB81B0, 'key': 'colour_mask_17', 'word': 108, 'source': 'sub_82BB81B0 returns +108 (slot 17 sub_82C23B60 caches it)'},
    {'fn': 0x82BB83E8, 'key': 'row304', 'word': 120, 'source': 'sub_82BB83E8 returns +120 (slot 25 sub_82C22D70 caches it)'},
    {'fn': 0x82BB8318, 'key': 'sampler_address', 'word': None, 'source': 'sub_82BB8318 returns +(37 + r4) * 4 (slot 28 caches it)'},
    {'fn': 0x82BB8358, 'key': 'sampler_filter', 'word': None, 'source': 'sub_82BB8358 returns +(57 + r4) * 4 (slot 30 caches it)'},
]
NUM_RE = re.compile(r'-?(?:0x[0-9A-Fa-f]+|\d+)')


class GstateError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def key_of(what):
    return re.sub(r'[^a-z0-9]+', '_', what.lower()).strip('_')


def table():
    """-> (setters {fn: row}, getters {fn: row}): enums.json's state calls / getters + SETTERS / GETTERS; every row has key / what / args / mark"""
    import qenum
    d = qenum.load()
    st, gt = {}, {}
    for c in d['calls']:
        if not c.get('state'):
            continue
        fn = int(c['fn'], 16)
        st[fn] = {'fn': fn, 'key': key_of(c['what']), 'what': c['what'], 'slot': c['slot'], 'word': c['word'], 'row': None,
                  'args': ((c['arg'], 'value'),), 'enum': c['enum'], 'mark': c['mark'], 'source': 'db/facts/enums.json: ' + c['source']}
    for r in SETTERS:
        st[r['fn']] = dict(r, enum=r.get('enum'))
    for g in d['getters']:
        gt[int(g['fn'], 16)] = {'fn': int(g['fn'], 16), 'key': key_of(g['what']), 'word': g['word'], 'source': 'db/facts/enums.json: ' + g['source']}
    for g in GETTERS:
        gt[g['fn']] = dict(g)
    return st, gt


def value_text(setter, imm):
    """[ints] -> '1 on' / '0 alpha blend (...)' / '0 | 1': the enum's name when the setter has one"""
    import qenum
    if setter.get('enum'):
        return qenum.value_text(setter['enum'], imm)
    return ' | '.join(str(v) for v in imm)


# ------------------------------------------------------------------ one function
def _decomp(con, fn, cache):
    if fn not in cache:
        try:
            cache[fn] = qdecomp.decompile(con, fn)
        except qdecomp.DecompError as e:
            cache[fn] = str(e)
    return cache[fn]


def _site_regs(d, tgt, site):
    if not isinstance(d, dict):
        return None
    for s in d['summary']['sites'].get(f'{tgt:08X}', []):
        if s['site'] == site:
            return s.get('regs') or {}
    return None


def _local_ret(d, root):
    """`L88@+378` (a stack slot read back) -> the one `ret#X` the decomp stores there, else None"""
    m = re.match(r'L(\d+)@', root or '')
    if not m or not isinstance(d, dict):
        return None
    got = set()
    for l in d['lines']:
        mm = re.search(r'local\[sp\+%s\] = (ret#[0-9A-F]+)\s' % m.group(1), l)
        if mm:
            got.add(mm.group(1))
    return got.pop() if len(got) == 1 else None


def value_reg(s):
    """the register of a setter's value (the argument a restore / forwarder is read from)"""
    return next((reg for reg, m in s['args'] if m == 'value'), s['args'][0][0])


def function_rows(con, fn, setters, getters, trace=True, pair=True, cache=None, insns=None):
    """-> [row] in listing order for one function: a setter call {off, kind set|restore|save, key, fn, via, tail, args {reg: {imm, root}},
    restore, value_reg} or a getter read {off, kind 'read', key, fn}. trace=False: no literal tracer (the planted twin: every value is
    `reg` with no root); pair=False: no restore pairing (the second twin)."""
    import qconstdiff
    import qrstdiff as R
    cache = {} if cache is None else cache
    insns = insns if insns is not None else qconstdiff._insns(con, fn)
    state, _idx = R.flow(insns, fn, {})
    own = {x.addr for x in insns}
    rows = []
    for i, x in enumerate(insns):
        mn = x.mn.rstrip('+-')
        if mn not in ('bl', 'b'):
            continue
        try:
            tgt = int((x.ops or '').split(',')[0].strip(), 16)
        except ValueError:
            continue
        if mn == 'b' and tgt in own:
            continue
        off = x.addr - fn
        if tgt in getters:
            rows.append({'off': off, 'kind': 'read', 'key': getters[tgt]['key'], 'fn': tgt, 'tail': mn == 'b'})
            continue
        s0 = setters.get(tgt)
        if s0 is None:
            continue
        st = state[i] or {}
        for s in [s0] + list(s0.get('also') or ()):
            args = {}
            for reg, _m in s['args']:
                args[reg] = _arg(con, fn, x.addr, tgt, reg, st, trace, cache)
            rows.append({'off': off, 'kind': s.get('kind', 'set'), 'key': s['key'], 'fn': tgt, 'via': s.get('via'), 'tail': mn == 'b',
                         'args': args, 'restore': None, 'value_reg': value_reg(s)})
    if pair:
        _pair(con, fn, rows, cache)
    return rows


def _arg(con, fn, site, tgt, reg, st, trace, cache):
    """one argument register at a setter call: {imm: [ints] or None, root: the decomp's text or None}"""
    if not trace:
        return {'imm': None, 'root': None}
    lits = st.get(reg)
    if lits:
        return {'imm': sorted(v - (1 << 32) if v >= 0xFFFF8000 else v for v in lits), 'root': None}
    regs = _site_regs(_decomp(con, fn, cache), tgt, site)
    root = (regs or {}).get(reg)
    if root is not None and NUM_RE.fullmatch(root):
        return {'imm': [int(root, 0)], 'root': None}
    return {'imm': None, 'root': root}


def _pair(con, fn, rows, cache):
    """a setter whose value is a getter's result of the same key read earlier in this function -> restore of that read; apply4 of the
    record a save4 of this function filled (the same r3 root) -> restore of that save"""
    reads = {r['off']: r for r in rows if r['kind'] == 'read'}
    saves = [r for r in rows if r['kind'] == 'save']
    for r in rows:
        if r['kind'] != 'set':
            continue
        root = (r['args'].get(r['value_reg']) or {}).get('root')
        if not root:
            continue
        if r['key'] == 'apply4':
            prev = [s for s in saves if s['off'] < r['off'] and (s['args'].get('r3') or {}).get('root') == root]
            if prev:
                r['kind'], r['restore'] = 'restore', prev[-1]['off']
            continue
        if root.startswith('L'):
            root = _local_ret(_decomp(con, fn, cache), root) or root
        m = re.fullmatch(r'ret#([0-9A-F]+)', root)
        if m:
            g = reads.get(int(m.group(1), 16))
            if g is not None and g['key'] == r['key']:
                r['kind'], r['restore'] = 'restore', g['off']


# ------------------------------------------------------------------ the index
def _index_key():
    h = hashlib.sha1()
    try:
        with open(os.path.join(qdecomp.GEN, 'codegen.stamp'), encoding='utf-8') as f:
            h.update(json.load(f).get('fingerprint', '').encode())
    except (OSError, ValueError):
        h.update(b'no-stamp')
    for p in (os.path.abspath(__file__), os.path.join(HERE, 'qrstdiff.py'), ENUMS):
        with open(p, 'rb') as f:
            h.update(f.read())
    h.update(qdecomp._code_sha().encode())
    h.update(str(VERSION).encode())
    return h.hexdigest()[:12]


def _callers(con, fns):
    out = set()
    for f in fns:
        for (src,) in con.execute("SELECT DISTINCT src FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (f,)):
            out.add(src)
    return out


def forwarders(done):
    """{fn: [{key, reg}]}: the functions that pass their own argument register (`in_rK`, unchanged since entry) as a setter's value"""
    fwd = collections.defaultdict(list)
    for f, rows in done.items():
        for r in rows:
            if r['kind'] != 'set' or r['key'] in ('apply4', 'save4') or r.get('via'):
                continue
            m = re.fullmatch(r'in_r(\d+)', (r['args'].get(r['value_reg']) or {}).get('root') or '')
            e = {'key': r['key'], 'reg': f'r{m.group(1)}'} if m else None
            if e and e not in fwd[f]:
                fwd[f].append(e)
    return {f: v for f, v in fwd.items() if v}


def derived_setters(setters, fwd):
    """the table plus one row per forwarder (its first forwarded key; the others ride `also`), each reading the forwarded register"""
    by_key = {s['key']: s for s in setters.values()}
    out = dict(setters)
    for f, lst in fwd.items():
        if f in setters:
            continue
        rows = [dict(by_key[e['key']], fn=f, via=f, args=((e['reg'], 'value'),), mark='L', also=()) for e in lst]
        out[f] = dict(rows[0], also=tuple(rows[1:]))
    return out


def build(con, setters, getters, log=None):
    """-> the index dict {version, key, functions {hex: {name, rows}}, forwarders {hex: [{key, reg}]}, failed {hex: why}}"""
    cache = {}
    done, failed = {}, {}

    def read(funcs, table_, what):
        for n, f in enumerate(funcs):
            if log and n % 100 == 0:
                log(f'q gstate: indexing {what} {n}/{len(funcs)}')
            try:
                done[f] = function_rows(con, f, table_, getters, cache=cache)
            except Exception as e:  # noqa: BLE001 - a function the listing reader refuses is named in the index, never hidden
                failed[f] = f'{type(e).__name__}: {e}'
            cache.pop(f, None)

    read(sorted(_callers(con, setters)), setters, 'setter callers')
    fwd = forwarders(done)
    if fwd:
        read(sorted(_callers(con, fwd)), derived_setters(setters, fwd), 'forwarder callers')
    names = {}
    for f in done:
        r = con.execute('SELECT name FROM funcs WHERE addr = ?', (f,)).fetchone()
        names[f] = r[0] if r else f'sub_{f:08X}'
    return {'version': VERSION, 'key': _index_key(), 'functions': {hx(f): {'name': names[f], 'rows': rows} for f, rows in sorted(done.items())},
            'forwarders': {hx(f): v for f, v in sorted(fwd.items())}, 'failed': {hx(f): w for f, w in sorted(failed.items())}}


def load_index(con, rebuild=False, log=None):
    """the cached index (out/harness/index/gstate.<key>.json), built when its key changed"""
    key = _index_key()
    path = os.path.join(INDEX_DIR, f'gstate.{key}.json')
    if not rebuild and os.path.exists(path):
        try:
            with open(path, encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            pass
    st, gt = table()
    idx = build(con, st, gt, log)
    try:
        os.makedirs(INDEX_DIR, exist_ok=True)
        for old in os.listdir(INDEX_DIR):
            if old.startswith('gstate.') and old.endswith('.json') and old != os.path.basename(path):
                try:
                    os.remove(os.path.join(INDEX_DIR, old))
                except OSError:
                    pass
        with open(path + '.tmp', 'w', encoding='utf-8') as f:
            json.dump(idx, f)
        os.replace(path + '.tmp', path)
    except OSError:
        pass
    return idx


# ------------------------------------------------------------------ text
def arg_text(setter, a):
    if a is None:
        return '?'
    if a.get('imm') is not None:
        return value_text(setter, a['imm'])
    return 'reg ' + (a.get('root') or '?')


def _setter_of(r, setters):
    base = next((x for x in setters.values() if x['key'] == r['key'] and not x.get('via')), None)
    return base or {'args': ((r['value_reg'], 'value'),), 'key': r['key']}


def value_part(r, setters):
    """the arguments of one setter call as text: `1 on`, `sampler 0, 2`, `restore of +12C`, `reg ret#170`"""
    s = _setter_of(r, setters)
    parts = []
    for reg, meaning in (s['args'] if not r.get('via') else ((r['value_reg'], 'value'),)):
        if reg not in r['args']:
            continue
        if meaning == 'value' and r['kind'] == 'restore':
            parts.append(f'restore of +{r["restore"]:X}')
            continue
        t = arg_text(s, r['args'][reg])
        parts.append(t if meaning == 'value' else f'{meaning} {t}')
    via = f' (via sub_{r["via"]:08X} {r["value_reg"]})' if r.get('via') else ''
    return ', '.join(parts) + via + (' (tail)' if r.get('tail') else '')


def row_text(r, setters):
    if r['kind'] == 'read':
        return f'+{r["off"]:X}  save     {r["key"]}  (getter sub_{r["fn"]:08X})'
    head = {'set': 'set     ', 'restore': 'restore ', 'save': 'save4   '}[r['kind']]
    return f'+{r["off"]:X}  {head}{r["key"]} = {value_part(r, setters)}'


def pairs_text(rows):
    ps = [f'{r["key"]} +{r["restore"]:X} -> +{r["off"]:X}' for r in rows if r['kind'] == 'restore']
    return 'pairs (save -> restore): ' + ('; '.join(ps) if ps else 'none')


def _resolve(con, s):
    import q as qmod
    try:
        return qmod.resolve(con, s)[0]
    except Exception as e:  # noqa: BLE001 - q.py run as __main__ owns another QError class: said as ours
        if type(e).__name__ == 'QError':
            raise GstateError(str(e))
        raise


def facts(idx, funcs=None):
    """the code-diff fact form (docs/research/s111/WHOLEDIFF_PLAN.md A.2): {side, fn, field, value, cond, at} + kind / via.
    value = the constant (a list when the paths differ), {'restore': save address} or {'reg': root}; cond = None: not computed (the
    listing order and the per-path literals are what the index holds)"""
    out = []
    for fh, ent in idx['functions'].items():
        f = int(fh, 16)
        if funcs is not None and f not in funcs:
            continue
        for r in ent['rows']:
            if r['kind'] == 'read':
                continue
            v = r['args'].get(r['value_reg']) or {}
            if r['kind'] == 'restore':
                val = {'restore': hx(f + r['restore'])}
            elif v.get('imm') is not None:
                val = v['imm'][0] if len(v['imm']) == 1 else v['imm']
            else:
                val = {'reg': v.get('root')}
            fact = {'side': 'game', 'fn': f'sub_{f:08X}', 'field': r['key'], 'value': val, 'cond': None, 'at': hx(f + r['off']), 'kind': r['kind']}
            others = {reg: a for reg, a in r['args'].items() if reg != r['value_reg']}
            if others:
                fact['args'] = others
            if r.get('via'):
                fact['via'] = f'sub_{r["via"]:08X}'
            out.append(fact)
    return out


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q gstate', description=__doc__.split('\n')[0])
    ap.add_argument('funcs', nargs='*')
    ap.add_argument('--setter', default=None, metavar='KEY', help='every function that calls this setter (a key, a wrapper sub_X, or words of its name)')
    ap.add_argument('--value', default=None, metavar='V', help='with --setter: only the functions with a call passing V (a number, restore, reg)')
    ap.add_argument('--list', action='store_true', help='the setter table')
    ap.add_argument('--direct', action='store_true', help='read the named functions now, without the index (no forwarders)')
    ap.add_argument('--rebuild', action='store_true', help='rebuild the index')
    ap.add_argument('--facts', action='store_true', help='the code-diff fact form {side, fn, field, value, cond, at} as JSON (all functions, or the named ones)')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--inherit', action='store_true', help='the state in force when the named functions run, read up their callers (qgstate_inherit.py)')
    ap.add_argument('--leaks', action='store_true', help='state a callee sets and leaves, seen by a later draw of the same pass (qgstate_leaks.py); no function = sweep of --field keys')
    ap.add_argument('--field', action='append', default=None, metavar='F', help='with --inherit: only this setter key (repeatable)')
    ap.add_argument('--depth', type=int, default=None, metavar='N', help='with --inherit: caller levels (default qgstate_inherit.DEPTH)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    setters, getters = table()
    if a.list:
        return _print_list(setters, getters, a.json)
    if a.direct:
        if not a.funcs:
            raise GstateError('--direct needs a function')
        out = {f: function_rows(con, f, setters, getters) for f in (_resolve(con, s) for s in a.funcs)}
        return _print_funcs(con, out, setters, a.json, 'read now, no index: forwarders not listed')
    if a.leaks:
        import qgstate_leaks as GL
        return GL.main(con, [_resolve(con, s) for s in a.funcs], a.field, a.json)
    if a.inherit:
        import qgstate_inherit as GI
        if not a.funcs:
            raise GstateError('--inherit needs a function')
        return GI.main(con, [_resolve(con, s) for s in a.funcs], a.field, a.depth or GI.DEPTH, a.json)
    idx = load_index(con, a.rebuild, log=lambda m: print(m, file=sys.stderr))
    if a.facts:
        want = {_resolve(con, s) for s in a.funcs} if a.funcs else None
        doc = {'index': f'gstate.{idx["key"]}.json', 'facts': facts(idx, want)}
        if want:   # the named functions also get the state they inherit (S111 GINHERIT; the whole index is not walked)
            import qgstate_inherit as GI
            w = GI.Walker(GI.Graph(con, idx))
            doc['inherited'] = [f for fn in sorted(want) for f in GI.facts(w, fn)]
        print(json.dumps(doc, indent=1))
        return 0
    if a.setter:
        return _print_setter(con, idx, a, setters)
    if not a.funcs:
        print(f'q gstate: {len(idx["functions"])} functions with setter calls, {len(idx["forwarders"])} forwarders, {len(idx["failed"])} unread '
              f'(index gstate.{idx["key"]}.json). q gstate sub_X | --setter KEY | --list')
        for fh, why in idx['failed'].items():
            print(f'  NOT READ {fh}: {why}')
        return 0
    out = {}
    for s in a.funcs:
        f = _resolve(con, s)
        ent = idx['functions'].get(hx(f))
        out[f] = ent['rows'] if ent else []
        if hx(f) in idx['failed']:
            print(f'q gstate: sub_{f:08X} NOT READ: {idx["failed"][hx(f)]}', file=sys.stderr)
    return _print_funcs(con, out, setters, a.json, f'index gstate.{idx["key"]}.json', idx)


def _print_list(setters, getters, as_json):
    rows = [dict(s, fn=hx(s['fn'])) for s in sorted(setters.values(), key=lambda s: (s['slot'] is None, s['slot'] or 0, s['fn']))]
    if as_json:
        print(json.dumps({'setters': rows, 'getters': [dict(g, fn=hx(g['fn'])) for g in getters.values()]}, indent=1))
        return 0
    print(f'q gstate --list: {len(setters)} setters, {len(getters)} getters (recorder vtable 0x8206C6D0)')
    for s in rows:
        args = ', '.join(f'{r} {m}' for r, m in s['args'])
        slot = s['slot'] if s['slot'] is not None else '-'
        word = s['word'] if s['word'] is not None else '-'
        print(f'  {s["key"]:22s} {s["fn"]}  slot {slot:>2}  word {word:>3}  ({args})  [{s["mark"]}]  {s["source"]}')
    for g in getters.values():
        print(f'  getter {hx(g["fn"])} -> {g["key"]} (word {g["word"]})  {g["source"]}')
    return 0


def _print_funcs(con, out, setters, as_json, src, idx=None):
    import q as qmod
    fwd = (idx or {}).get('forwarders', {})
    if as_json:
        print(json.dumps({'source': src, 'functions': {hx(f): {'name': qmod.fname(con, f), 'rows': rows, 'forwards': fwd.get(hx(f), [])}
                                                        for f, rows in out.items()}}, indent=1))
        return 0
    for f, rows in out.items():
        n_set = sum(1 for r in rows if r['kind'] in ('set', 'save'))
        n_res = sum(1 for r in rows if r['kind'] == 'restore')
        n_read = sum(1 for r in rows if r['kind'] == 'read')
        print(f'q gstate {qmod.fname(con, f)} (0x{f:08X}): {n_set} set, {n_res} restore, {n_read} saved by a getter  ({src})')
        if not rows:
            print('  no setter call (q gstate --list names the setters)')
            continue
        for r in rows:
            print('  ' + row_text(r, setters))
        print('  ' + pairs_text(rows))
        if fwd.get(hx(f)):
            print('  forwards its own argument to: ' + ', '.join(f'{e["key"]} ({e["reg"]})' for e in fwd[hx(f)]) + '  (its callers: q gstate --setter KEY)')
    return 0


def _match_setter(want, setters):
    w = want.lower()
    if w.startswith('sub_') or w.startswith('0x'):
        try:
            a = int(w[4:] if w.startswith('sub_') else w, 16)
        except ValueError:
            a = None
        if a in setters:
            return setters[a]['key']
    keys = {s['key'] for s in setters.values()}
    if key_of(w) in keys:
        return key_of(w)
    hit = sorted({s['key'] for s in setters.values() if w in s['what'].lower() or w in s['key']})
    if len(hit) == 1:
        return hit[0]
    raise GstateError(f'--setter {want}: ' + (f'several setters match: {", ".join(hit)}' if hit else f'no setter (have: {", ".join(sorted(keys))})'))


def _call_matches(r, want):
    if want is None:
        return True
    a = r['args'].get(r['value_reg']) or {}
    w = want.strip().lower()
    if w == 'restore':
        return r['kind'] == 'restore'
    if w == 'reg':
        return a.get('imm') is None and r['kind'] != 'restore'
    try:
        return a.get('imm') is not None and int(w, 0) in a['imm']
    except ValueError:
        return False


def _hist_key(r):
    v = r['args'].get(r['value_reg']) or {}
    if r['kind'] == 'restore':
        return 'restore'
    if v.get('imm') is not None:
        return ' | '.join(str(x) for x in v['imm'])
    return 'reg'


def _print_setter(con, idx, a, setters):
    import q as qmod
    key = _match_setter(a.setter, setters)
    hits = []
    for fh, ent in idx['functions'].items():
        calls = [r for r in ent['rows'] if r['key'] == key and r['kind'] != 'read']
        if calls and any(_call_matches(r, a.value) for r in calls):
            hits.append((fh, qmod.fname(con, int(fh, 16)), calls))
    hist = collections.Counter(_hist_key(r) for _fh, _n, calls in hits for r in calls)
    if a.json:
        print(json.dumps({'setter': key, 'value': a.value, 'functions': [{'func': fh, 'name': n, 'calls': calls} for fh, n, calls in hits],
                          'histogram': dict(hist)}, indent=1))
        return 0
    n_calls = sum(len(c) for _f, _n, c in hits)
    print(f'q gstate --setter {key}: {len(hits)} functions' + (f' with a call passing {a.value}' if a.value else '') +
          f'; their {n_calls} calls of it: ' + ', '.join(f'{k} x{v}' for k, v in hist.most_common()))
    for fh, n, calls in hits[:None if a.all else CAP]:
        print(f'  {n} ({fh}): ' + '; '.join(f'+{r["off"]:X} {value_part(r, setters)}' for r in calls))
    if not a.all and len(hits) > CAP:
        print(f'  ... {len(hits) - CAP} more: q gstate --setter {key}' + (f' --value {a.value}' if a.value else '') + ' --all')
    return 0


if __name__ == '__main__':
    import q as _q
    sys.exit(main(sys.argv[1:], _q.connect(_q.DB)))
