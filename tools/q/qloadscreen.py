r"""q loadscreen: a loading-screen request id -> its picture, joined from what the tools already know (S111 QFIX4; the re-find of docs\research\s111\M100_ART.md Q14.1).

    py tools/q/q.py loadscreen ID [ID ...]     request id -> class -> slot-14 load function -> the /LOAD/ PACH name -> the PACH(es) of LoadHD.pac -> layout scene + textures
    py tools/q/q.py loadscreen --all [--json]  every id the builder switch names, one block per class (ids listed), and the default (> the bound)

The chain, each step read from the index / the player's files, never typed in:
  1. the builder `sub_827E5938` (a jump-table switch on the request id: `q switch`): each constructor it calls is assigned to the case whose body holds the call
     site (the case bodies are laid out in address order; the default body is the one for ids above the bound);
  2. the constructor's class vtable (the field index `ctors`), its slot 14 (+56) = the load function (`q vtable`; slot 27 when slot 14 names no /LOAD/ string: the
     Streak classes);
  3. the /LOAD/ name pattern: the strings the load function and its callees (3 levels, library routines with > 64 callers not entered) reference (`strrefs`),
     e.g. /LOAD/GN%02d;
  4. the PACHs of pac/menu/LoadHD.pac whose virtual path matches the pattern (`q asset pac LoadHD`); none = "not in the player's files";
  5. per PACH: the layout leaves' scene records (the YXMLBIN decoder tools\menus\yxmlbin.py: record kind 11 = a scene, kind 5 = an element set) and the texture
     names of its texture directory. PACHes with the same scene and textures are grouped (GN00-GN31).
Names only: no picture data, no bytes of the files are printed. Marks: [C] = read from the index / files; [L] = the id -> class assignment (by call-site position
in the switch body: confirm a surprising one in `q switch sub_827E5938` / `q asm`).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

BUILDER = 0x827E5938
PAC = 'LoadHD.pac'
SLOTS = (14, 27)
DEPTH, LIB_CALLERS = 3, 64
GROUP_CAP = 4
COMMON_TEX =('wwelogo_move', 'wwe_circle')      # in every loading PACH (M100_ART Q14.1)


class LoadError(Exception):
    pass


def fn(a):
    return f'sub_{a:08X}'


# ------------------------------------------------------------------ 1: id -> constructor
def case_map(con, addr=BUILDER):
    """-> {'bound', 'default', 'bias', 'bodies': [(target, [ids])]} of the builder's switch"""
    import qgx
    r = qgx.cmd_switch(con, addr, 80)
    if not r['switches']:
        raise LoadError(f'{fn(addr)}: no jump-table switch in its listing')
    sw = r['switches'][0]
    bias = sw.get('bias') or 0
    return {'bound': sw['range'], 'default': sw['default'], 'bias': bias,
            'bodies': sorted((g['target'], [n + bias for n in g['cases']]) for g in sw['groups'])}


def ctor_calls(con, fdb, addr=BUILDER):
    """-> [(site, ctor, vtable)] the direct calls of the builder to a function the field index records storing a vptr"""
    out = []
    for r in con.execute("SELECT DISTINCT site, dst FROM edges WHERE src = ? AND kind = 'call' ORDER BY site", (addr,)):
        site, dst = r[0], r[1]
        vt = None
        if fdb:
            row = fdb.execute('SELECT vt FROM ctors WHERE func = ?', (dst,)).fetchone()
            vt = row[0] if row else None
        if vt is not None:
            out.append((site, dst, vt))
    return out


def body_start(starts, site):
    """the case body holding a call site: the greatest body start at or below it (the bodies are laid out in address order)"""
    c = [s for s in starts if s <= site]
    return c[-1] if c else None


def classes_by_id(con, fdb):
    """-> {'ids': {id: (ctor, vt) or None}, 'default': (ctor, vt) or None, 'bound', 'cases': [(ids, ctor, vt)]}"""
    cm = case_map(con)
    starts = [t for t, _ids in cm['bodies']]
    default = cm['default']
    if default is not None and default not in starts:
        starts = sorted(starts + [default])
    calls = ctor_calls(con, fdb)

    by_body = {}
    for site, ctor, vt in calls:
        b = body_start(starts, site)
        if b is not None:
            by_body.setdefault(b, []).append((site, ctor, vt))
    ids = {}
    cases = []
    for target, cids in cm['bodies']:
        cl = by_body.get(target, [])
        pick = (cl[-1][1], cl[-1][2]) if cl else None       # the last constructor called in the body: the one whose object the case keeps
        for i in cids:
            ids[i] = pick
        cases.append((cids, pick))
    dflt = None
    if default is not None:
        cl = by_body.get(default, [])
        dflt = (cl[-1][1], cl[-1][2]) if cl else None
    return {'ids': ids, 'default': dflt, 'bound': cm['bound'], 'cases': cases}


# ------------------------------------------------------------------ 2 / 3: class -> load function -> /LOAD/ pattern
def slot_funcs(con, vt):
    import qvtable
    import q as qm
    img = qvtable.Img(os.path.join(os.path.dirname(qm.DB), 'image.bin'))
    return [t for _s, _a, t in qvtable.read_table(con, img, vt)]


PATH_RX = re.compile(r'^/[A-Z]{2,6}/')      # a pac virtual path: /LOAD/GN%02d, /SNLD/LG%d


def load_strings(con, root, depth=DEPTH):
    """-> [(string, func)] the pac-path strings (/LOAD/..) referenced by `root` and its callees (`depth` levels; a callee with > LIB_CALLERS callers is a library routine)"""
    seen, frontier, out = {root}, [root], []
    for _d in range(depth + 1):
        nxt = []
        for f in frontier:
            for (t,) in con.execute('SELECT s.text FROM strrefs r JOIN strings s ON s.addr = r.addr WHERE r.func = ?', (f,)):
                if PATH_RX.match(t) and (t, f) not in out:
                    out.append((t, f))
            for (d,) in con.execute("SELECT DISTINCT dst FROM edges WHERE src = ? AND kind IN ('call', 'tail')", (f,)):
                if d in seen:
                    continue
                seen.add(d)
                n = con.execute("SELECT COUNT(DISTINCT src) FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (d,)).fetchone()[0]
                if n <= LIB_CALLERS:
                    nxt.append(d)
        frontier = nxt
    return out


def load_of(con, vt, ctor):
    """-> {'slot', 'func', 'strings': [(text, func)]} for a class vtable: slot 14, else slot 27; the constructor's own strings count too (the SC / SM classes name
    their pattern `/LOAD/SC%s` in the constructor, the number is formatted below slot 14)"""
    fs = slot_funcs(con, vt)
    own = load_strings(con, ctor, 0)
    for k in SLOTS:
        if k < len(fs):
            s = load_strings(con, fs[k])
            for t in own:
                if t not in s:
                    s.append(t)
            if s:
                return {'slot': k, 'func': fs[k], 'strings': s}
    return {'slot': SLOTS[0], 'func': fs[SLOTS[0]] if SLOTS[0] < len(fs) else None, 'strings': own}


def pattern_rx(p):
    """/LOAD/GN%02d -> a regex over the PACH virtual path"""
    out, i = '', 0
    while i < len(p):
        m = re.match(r'%(0?)(\d*)([dcsx])', p[i:])
        if m:
            w = m.group(2)
            out += {'d': r'\d' + (('{%s}' % w) if w else '+'), 'c': '.', 's': '.+', 'x': '[0-9A-Fa-f]' + (('{%s}' % w) if w else '+')}[m.group(3)]
            i += m.end()
        else:
            out += re.escape(p[i])
            i += 1
    return re.compile('^' + out + '$', re.I)


# ------------------------------------------------------------------ 4 / 5: PACH -> scene + textures
def pach_list(acon):
    """-> {idx: vpath} of LoadHD.pac (the asset index)"""
    import qasset
    p = qasset.pac_rows(acon, PAC)
    if not p:
        raise LoadError(f'no pac {PAC} in the asset index (q asset update)')
    return p[0]['id'], {r['idx']: r['vpath'] for r in acon.execute('SELECT idx, vpath FROM pach WHERE pac_id = ?', (p[0]['id'],))}


def textures_of(acon, pac_id, idx):
    return [r[0] for r in acon.execute("SELECT t.name FROM entry e JOIN pach p ON e.pach_id = p.id JOIN texture t ON t.entry_id = e.id "
                                       "WHERE p.pac_id = ? AND p.idx = ? AND e.kind = 'texdir' ORDER BY e.id, t.id", (pac_id, idx))]


_LAY = {}


def layouts():
    """-> {PACH idx: [(leaf, {'scenes': [...], 'sets': [...]})]} the YXMLBIN leaves of LoadHD.pac decoded by tools\\menus\\yxmlbin.py"""
    if not _LAY:
        sys.path.insert(0, os.path.join(HERE, '..', 'menus'))
        import yxmlbin
        for leaf, dec in yxmlbin.pac_leaves(PAC):
            m = re.fullmatch(r'p(\d+)/(\d+)', leaf)
            if not m:
                continue
            try:
                lay = yxmlbin.parse(dec)
            except yxmlbin.YxmlError:
                continue
            recs = lay['records']
            _LAY.setdefault(int(m.group(1)), []).append(
                (leaf, {'scenes': [r['name'] for r in recs if r['kind'] == 11], 'sets': [r['name'] for r in recs if r['kind'] == 5]}))
    return _LAY


def sig_of(acon, pac_id, idx, lay):
    ls = lay.get(idx, [])
    tx = [t for t in textures_of(acon, pac_id, idx) if t not in COMMON_TEX]
    return (tuple((l.split('/', 1)[1], tuple(x['scenes']), tuple(x['sets'])) for l, x in ls), tuple(tx))


def pach_groups(acon, pac_id, pachs, lay):
    """-> [{'pachs': [(idx, vpath)], 'leaves': [...], 'textures': [...]}] PACHes with the same layouts and textures grouped"""
    groups = {}
    for idx, vp in sorted(pachs):
        groups.setdefault(sig_of(acon, pac_id, idx, lay), []).append((idx, vp))
    out = []
    for (ls, tx), ps in sorted(groups.items(), key=lambda kv: kv[1][0][0]):
        out.append({'pachs': ps, 'leaves': [{'leaf': l, 'scenes': list(sc), 'sets': list(st)} for l, sc, st in ls], 'textures': list(tx)})
    return out


# ------------------------------------------------------------------ the answer
def describe_class(con, acon, fdb, ctor, vt, names=None):
    ld = load_of(con, vt, ctor)
    pac_id, pachs = pach_list(acon)
    matched = {}
    for text, f in ld['strings']:
        if not text.startswith('/LOAD/'):
            continue      # another pac's path (/SNLD/LG%d is menu.pac): not looked up in LoadHD.pac
        rx = pattern_rx(text)
        for idx, vp in pachs.items():
            if vp and rx.match(vp):
                matched.setdefault(text, []).append((idx, vp))
    lay = layouts()
    out = {'ctor': fn(ctor), 'vtable': f'0x{vt:08X}', 'load_slot': ld['slot'], 'load_func': fn(ld['func']) if ld['func'] else None,
           'patterns': [], 'other': []}
    if not ld['strings']:
        out['other'] = ['no /LOAD/ string in the load function tree (this class loads from another pac: see its slot %d %s)' % (ld['slot'], fn(ld['func']) if ld['func'] else '?')]
    for text, f in ld['strings']:
        ps = matched.get(text, [])
        out['patterns'].append({'pattern': text, 'in': fn(f), 'pachs': len(ps), 'other_pac': not text.startswith('/LOAD/'),
                                'groups': pach_groups(acon, pac_id, ps, lay) if ps else []})
    return out


def range_text(ps):
    idx = [i for i, _ in ps]
    names = [v.rsplit('/', 1)[-1] for _, v in ps]
    ctg = idx == list(range(idx[0], idx[0] + len(idx)))
    nm = names[0] if len(names) == 1 else f'{names[0]}..{names[-1]}'
    cnt = f'{len(idx)} PACHes, ' if len(idx) > 1 else ''
    return f"{nm} ({cnt}PACH {idx[0]}{'-' + str(idx[-1]) if len(idx) > 1 else ''}{'' if ctg or len(idx) == 1 else ', not contiguous'})"


def print_class(ids_text, d, out=print, full=False):
    out(f"id {ids_text}: class {d['ctor']}, vtable {d['vtable']}, slot {d['load_slot']} = {d['load_func']}")
    for x in d['other']:
        out(f'    {x}')
    for p in d['patterns']:
        if p.get('other_pac'):
            out(f"    {p['pattern']} (named in {p['in']}): not a /LOAD/ path (another pac's PACH: q asset pac menu)")
            continue
        if not p['pachs']:
            out(f"    {p['pattern']} (named in {p['in']}): no PACH of that name in the player's {PAC}")
            continue
        out(f"    {p['pattern']} (named in {p['in']}): {p['pachs']} PACH(es) in {PAC}")
        groups = p['groups'] if full else p['groups'][:GROUP_CAP]
        for g in groups:
            out(f"      {range_text(g['pachs'])}")
            for lf in g['leaves']:
                sc = ', '.join(lf['scenes'][:6]) + (f" +{len(lf['scenes']) - 6}" if len(lf['scenes']) > 6 else '')
                st = ', '.join(lf['sets'][:8]) + (f" +{len(lf['sets']) - 8}" if len(lf['sets']) > 8 else '')
                out(f"        layout key {lf['leaf']}: scene {sc or '-'}; element sets {st or '-'}")
            if g['textures']:
                out(f"        textures: {', '.join(g['textures'][:14])}" + (f" +{len(g['textures']) - 14}" if len(g['textures']) > 14 else ''))
        if len(p['groups']) > len(groups):
            out(f"      ... {len(p['groups']) - len(groups)} more PACH groups of {len(p['groups'])} (--full, or --json)")


def ids_text(ids, bound=None, dflt=False):
    ids = sorted(ids)
    return ', '.join(str(i) for i in ids) + (f', > {bound}' if dflt and bound is not None else '')


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q loadscreen', description=__doc__.split('\n')[0])
    ap.add_argument('ids', nargs='*', help='request ids')
    ap.add_argument('--all', action='store_true', help='every id of the builder switch, one block per class')
    ap.add_argument('--full', action='store_true', help='every PACH group of a pattern (default: the first %d)' % GROUP_CAP)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    if not a.ids and not a.all:
        ap.error('give ID [ID ...] or --all')
    import qasset
    import qslice
    fdb = qslice.QSource(con).fdb() or None
    if not fdb:
        raise LoadError('no field index (the constructors\' vtables): py tools\\q\\build_fields.py')
    acon = qasset.connect()
    cb = classes_by_id(con, fdb)
    want = {}                       # (ctor, vt) -> [ids]
    if a.all:
        for i, pick in sorted(cb['ids'].items()):
            if pick:
                want.setdefault(pick, []).append(i)
        if cb['default']:
            want.setdefault(cb['default'], [])
    else:
        for s in a.ids:
            try:
                i = int(s, 0)
            except ValueError:
                raise LoadError(f'{s!r}: a request id is a number')
            pick = cb['ids'].get(i) if i in cb['ids'] else cb['default']
            if not pick:
                print(f'id {i}: no class (the builder switch builds nothing for it)')
                continue
            want.setdefault(pick, []).append(i)
    rows = []
    for (ctor, vt), ids in sorted(want.items(), key=lambda kv: (min(kv[1]) if kv[1] else 10 ** 9, kv[0][0])):
        d = describe_class(con, acon, fdb, ctor, vt)
        dflt = cb['default'] == (ctor, vt) and (a.all or any(i not in cb['ids'] for i in ids))
        d['ids'] = ids
        d['default'] = bool(dflt)
        rows.append(d)
    if a.json:
        print(json.dumps({'builder': fn(BUILDER), 'bound': cb['bound'], 'classes': rows}, indent=1))
        return 0
    for d in rows:
        print_class(ids_text(d['ids'], cb['bound'], d['default']), d, full=a.full)
    print(f"(builder {fn(BUILDER)}: ids -> class by the call site's case body [L]; vtable / slot / strings / PACHes / layouts / textures read from the index and the player's files [C]; names only)")
    return 0
