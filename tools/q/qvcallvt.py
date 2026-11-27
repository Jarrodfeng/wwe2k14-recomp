"""q vcall OFF --vt 0xVT (S111 QVCALLVT): the virtual call sites of slot OFF whose receiver is an object of a class given by its RAW vtable address
(no RTTI needed: the menu pages and frame classes have none). Used by qx7.vcall_main; the sites come from the vcall table, the class from q struct's
machinery:

  class    the family of the vtable: qstruct.class_target(vt) (the class root: every object whose constructor stores the vptr; a subobject vptr maps
           to its outer class) plus the classes built on it (a derived class's constructor calls the base constructor on its this: q class's "classes
           built on it", followed to depth 3): a frame object of a derived class is an object whose vptr store writes the base vtable too
  a site   is kept when ONE of these holds (the `how` text says which):
             vptr       the vptr was stored in the SAME function before the call (the vcall table's own `cls`)
             slice      qslice.canon of the call's r3 (the object the vtable word was read from, `rotlwi rN,rM,0` copies looked through) is the family's
                        root: the receiver traces to a constructor's result, an argument every caller types, a global the family is kept in
             slot       the call is on `this` of a function that is a slot of a family vtable
             field      r3 is loaded from a field ([page+1056]): the writers of that field qslice finds on the receiver's class (the class's functions and
                        their callees) store an object of the family there [C]
             field [L]  nothing proves the field's base is the same object: a store of displacement OFF in ANY function (the field index's W rows of
                        that offset, whatever their root) puts an object of the family there. The page forwarders: the derived page classes store the
                        frame at +1056 through roots the slicer does not tie to the base page class. It can name a site whose field is another class's
                        field at the same offset; the text says so
  a site whose receiver the slicer cannot type is COUNTED (unresolved), a site traced to another class is COUNTED (other), neither is listed.

Limits: the tracer is qslice's: a receiver loaded from a field of an untyped object or through a loop is unresolved. A function that is a slot of several
vtables is `slot` for each of them (a base method shared with other classes).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qslice  # noqa: E402
import qstruct  # noqa: E402

MAX_WSTORES = 12      # writer stores of a pointer field read (all counted by qslice; these are the ones canon'd)
MAX_FAMILY = 60       # classes followed
FAMILY_DEPTH = 3


class VcvError(Exception):
    pass


def unrot(T, v):
    """`rotlwi r3,r11,0` is the compiler's register copy: the interpreter keeps it as ('op', 'rotl32', x, ('c', 0)); the value is x"""
    for _ in range(4):
        v = qstruct.resolve_def(T, v)
        if v is not None and v[0] == 'op' and len(v) == 4 and v[1] == 'rotl32' and v[3] == ('c', 0):
            v = v[2]
        else:
            break
    return v


def family(src, sl, tgt):
    """the vtables of the class and of the classes built on it (their constructors call ours on this), depth <= FAMILY_DEPTH; sets tgt.fam, tgt.root,
    tgt.globals to cover all of them"""
    fam, todo = {tgt.vt}, [(tgt.vt, 0)]
    while todo and len(fam) < MAX_FAMILY:
        v, d = todo.pop(0)
        if d >= FAMILY_DEPTH:
            continue
        try:
            _t, out = qstruct.class_of(src, 'vt:0x%08X' % v, names={})
        except qstruct.StructError:
            continue
        for ch in out['children']:
            c = int(ch['vtable'], 16)
            if c not in fam:
                fam.add(c)
                todo.append((c, d + 1))
    tgt.fam = fam
    tgt.root = ('type', tuple(sorted(fam)))
    for v in fam:
        tgt.globals |= set(sl.globals_of(v))
    return fam


def in_family(sl, tgt, vt):
    return sl.norm_vt(vt)[0] in tgt.fam or vt in tgt.fam


def off_stores(src, off):
    """[(func, store offset)] every store of displacement `off` and width 4 in the game's code, whatever its base: the field index's W rows (a planted source
    answers off_stores itself)"""
    f = getattr(src, 'off_stores', None)
    if f is not None:
        return list(f(off))
    db = src.fdb()
    if not db:
        return []
    return [(fn, a - fn) for fn, a in db.execute("SELECT DISTINCT func, addr FROM rows WHERE rw = 'W' AND off = ? AND width = 4", (off,))]


def store_value_hit(sl, tgt, fa, rel):
    """True / False / None: the value of the store at function fa + rel is an object of the family / of another class / not typed"""
    W = sl.facts(fa)
    if W is None:
        return None
    for s in W['stores']:
        if s['o'] == rel and s['stk'] is None:
            if s['val'][0] == 'c':
                return None
            cv = sl.canon(W, s['val'], None)
            if cv is None or cv[0][0] != 'type':
                return None
            return qslice.same_root(tgt.root, cv[0])
    return None


def field_how(src, sl, tgt, T, r3):
    """the receiver is a pointer LOADED from a field ([this+1056] of a page); -> the how text or None. First the class's own writers of that field
    (qslice.writers) [C], then, failing that, every store of that displacement anywhere [L]"""
    memo = tgt.fmemo
    for v in qstruct.expand(T, r3):
        if v[0] != 'ld':
            continue
        cn = sl.canon(T, v[2], None)
        if cn is None or cn[0][0] in ('u', 'sp'):
            continue
        key = (repr(cn[0]), cn[1], v[1])
        if key not in memo:      # one field is read by many sites: its writers are searched once
            ws = sl.writers(T, cn[0], cn[1], v[1])
            hit = tot = 0
            for fa, s, _mark, _via, _r2, wbind in ws[:MAX_WSTORES]:
                W = sl.facts(fa)
                cv = sl.canon(W, s['val'], wbind) if W is not None else None
                if cv is None or s['val'][0] == 'c':
                    continue
                tot += 1
                hit += qslice.same_root(tgt.root, cv[0])
            memo[key] = (hit, tot)
        hit, tot = memo[key]
        if hit:
            return 'field: r3 is loaded from %s +%d; %d of %d writer stores put an object of the class there' % (qslice.root_text(cn[0]), cn[1][0], hit, tot)
        if cn[0][0] in ('type', 'p') and not cn[1][1]:
            off = cn[1][0]
            if off not in tgt.omemo:
                ok = n = 0
                for fa, rel in off_stores(src, off)[:400]:
                    h = store_value_hit(sl, tgt, fa, rel)
                    n += h is not None
                    ok += bool(h)
                tgt.omemo[off] = (ok, n)
            ok, n = tgt.omemo[off]
            if ok:
                return ('field [L]: r3 is loaded from %s +%d; no store of that field on the receiver\'s own class puts the class there, but %d of %d typed '
                        'stores of +%d anywhere do (the base of those stores is not proven to be this object)' % (qslice.root_text(cn[0]), off, ok, n, off))
    return None


def classify(src, sl, tgt, row):
    """-> (how, typed): how = the text of the relation to the class or None; typed = the receiver was traced to SOME class (so a None with typed is
    another class, a None without is unresolved). row: dict with site, func, cls (vtable stored in the function, 0 = none), this_recv"""
    cls = row['cls']
    if cls:
        if in_family(sl, tgt, cls):
            return 'vptr: sub_%08X stores the vptr %s before the call' % (row['func'], qstruct.hx(cls)), True
        return None, True
    T = sl.facts(row['func'])
    call = T['calls'].get(row['site'] - row['func']) if T is not None else None
    r3 = call['regs'].get('r3') if call is not None else None
    typed = False
    if r3 is not None:
        r3 = unrot(T, r3)
        cn = sl.canon(T, r3, None)
        if cn is not None:
            m = 'C' if qslice.same_root(tgt.root, cn[0]) else ('L' if cn[0][0] == 'g' and cn[0][1] in tgt.globals else None)
            if m:
                off = cn[1][0]
                return 'slice [%s]: r3 at the call traces to the class%s' % (m, (' (+%d)' % off) if off else ''), True
            typed = cn[0][0] == 'type'
        how = field_how(src, sl, tgt, T, r3)
        if how:
            return how, True
    if row['this_recv']:
        vs = src.vslots(row['func'])
        for vt, i in vs:
            if in_family(sl, tgt, vt):
                return 'slot: `this` of sub_%08X, slot %d of %s' % (row['func'], i, qstruct.hx(vt)), True
        typed = typed or bool(vs)
    return None, typed


def pick(src, vt, rows):
    """-> (kept [(row, how)], other classes count, unresolved count, target): the rows whose receiver is an object of the class of vtable vt"""
    if not src.is_vt(vt):
        raise VcvError('%s: its first word is not a function start: not a vtable (q words %s 4)' % (qstruct.hx(vt), qstruct.hx(vt)))
    sl = qslice.Slicer(src)
    tgt = qstruct.class_target(sl, src, vt)
    tgt.fmemo, tgt.omemo = {}, {}
    family(src, sl, tgt)
    kept, other, unresolved = [], 0, 0
    for r in rows:
        how, typed = classify(src, sl, tgt, r)
        if how:
            kept.append((r, how))
        elif typed:
            other += 1
        else:
            unresolved += 1
    return kept, other, unresolved, tgt
