r"""qvtarget: the known targets of an indirect call, printed AT the call site (S111 Lane E VCALL, docs/research/s111/INDEX_PLAN.md T5).

A provider of qfactcard (`qfactcard.register('vcall', ...)`): `q decomp FUNC` / `q asm FUNC --at` print, before the line of each indirect call:

    ; [vcall] slot +24 [C] sub_82A1BDD8 (icall ae)                                      (0x82A63E68)
    ; [vcall] slot +116 [C] sub_8259F0E0 (own vptr store before the site; icall b narrow 2)   (0x825A26B0, F1 below)
    ; [vcall] icall A [L] narrow 2: sub_82456BD8, sub_82456C00 (icall arg)                (0x8242A3E0)
    ; [vcall] slot +16 [L] seen 1: sub_82696F60 x10 | cand 2: sub_82696F60, sub_82A2CB80 (icall bfL)   (0x8250E7CC)
    ; [vcall] slot +12 [L] seen 1 of 1189 fns: sub_824BA470 x10                           (0x8249BD9C)

ONE source for the static answer (S111 review, main session: rule 6, the objtype fork deleted): vcall.s<N>.db `icall` / `icall_target`, the reviewed
S110 ICALL + CLASSFLOW table (`qx7.resolve_sites`, `qclassflow.py`; `how` = the rule: this-class, b, ae, const ...):
  exact   -> [C] its one target                      (S111 review: 0 of 515 exact via_vt sites with coverage contradicted by it)
  narrow  -> [L] narrow N: the set (a sound superset: the target is one of them; the rule b gives the UNION of the ctor / dtor classes on one object)
  cand    -> [L] cand N: candidates only (escape / field reads not checked)
  offset / open -> nothing of its own (everything at that slot is not an answer)
plus `q seen` (seen.s1.db): the targets coverage runs OBSERVED at the very site, printed first on an [L] line: seen in runs, never all the targets
(`seen K of N fns` = K seen, N the functions the slot offset holds; no "of N" when the table has no count). Nothing known prints NOTHING.
F1 (kept: icall_target lacks it, rule b is order-free): a NARROW vtable site whose receiver this function itself gave a vptr (fields.s<N>.db objtype
`vptr` of the slot-load row's root, a store at root+0 at an address before the site) is [C] when exactly ONE of icall's functions is that class's
slot word (S111 review: a constructor calls its base constructor, then stores its own vptr: rule b lists the base's slot beside the right one,
e.g. 0x8231AC98, 0x825A26B0). It only narrows icall's set, never adds; "before" is by address, not by path.
NOT KNOWN, said plainly: a model / component held in a pointer field (e.g. sub_825A3908 +0x430 / +0x4D0, slot +124 of the model at char+48): icall
offset (109 functions), no coverage of the site, so NO line; neither table types a pointer field's stored class there.
The lists are capped (CAP entries); the count of the rest is always printed. Selftest: py -I tools\q\qvtarget_selftest.py
"""
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qfactcard  # noqa: E402

CAP = 4                  # entries listed per site (the count of the rest is printed)
LOOK = 0x40              # bytes before the bctrl searched for the slot-load row (F1)
_ROOT = re.compile(r'^p\((.*),0\)$')
_SRC = []                # selftest: [Sources]
_REAL = []               # the opened real stores, once per process


class Sources:
    """the read-only stores; any may be None (that tier then says nothing)"""

    def __init__(self, vcon=None, fcon=None, scon=None):
        self.vcon, self.fcon, self.scon = vcon, fcon, scon

    @classmethod
    def real(cls):
        def ro(p):
            try:
                return sqlite3.connect(f'file:{p}?mode=ro', uri=True) if p and os.path.exists(p) else None
            except sqlite3.Error:
                return None
        import qx7
        import build_fields as bf
        import qseen
        return cls(ro(qx7.vdb_path()), ro(bf.default_paths()[2]), ro(qseen.SEEN_DB))

    # -- the static table
    def sites(self, func):
        """-> [(site, shape, off, status, n)] every indirect call site of the function"""
        if self.vcon is None:
            return []
        try:
            return self.vcon.execute('SELECT site, shape, off, status, n FROM icall WHERE func = ? ORDER BY site', (func,)).fetchall()
        except sqlite3.Error:
            return []

    def targets(self, site):
        """-> [(target, how)]"""
        return self.vcon.execute('SELECT target, how FROM icall_target WHERE site = ? ORDER BY target', (site,)).fetchall()

    def seen(self, site):
        if self.scon is None:
            return []
        try:
            return self.scon.execute('SELECT target, MAX(runs) FROM seen WHERE site = ? GROUP BY target ORDER BY target', (site,)).fetchall()
        except sqlite3.Error:
            return []

    # -- F1: this function's own vptr store on the receiver
    def recv_root(self, func, site, slot):
        """the root text of the object whose vtable the call reads: the slot-load row `p(X,0)` nearest before the site"""
        if self.fcon is None:
            return None
        r = self.fcon.execute('SELECT ro.text FROM rows r JOIN roots ro ON ro.id = r.root WHERE r.func = ? AND r.addr < ? AND r.addr >= ? AND r.rw = ? AND r.off = ? '
                              'ORDER BY r.addr DESC LIMIT 1', (func, site, site - LOOK, 'R', slot)).fetchone()
        m = _ROOT.match(r[0]) if r else None
        return m.group(1) if m else None

    def own_vtables(self, func, root):
        """the vtables THIS function stored at root+0"""
        if self.fcon is None or root is None:
            return []
        return [vt for (vt,) in self.fcon.execute('SELECT DISTINCT o.vt FROM objtype o JOIN roots ro ON ro.id = o.root WHERE ro.text = ? AND o.func = ? AND o.how = ?',
                                                  (root, func, 'vptr'))]

    def stored_before(self, func, site, root):
        """a store at root+0 in this function at an address before the site"""
        return self.fcon.execute('SELECT 1 FROM rows r JOIN roots ro ON ro.id = r.root WHERE r.func = ? AND r.addr < ? AND r.rw = ? AND r.off = 0 AND ro.text = ? LIMIT 1',
                                 (func, site, 'W', root)).fetchone() is not None

    def slot_func(self, vt, slot):
        r = self.fcon.execute('SELECT func FROM slots WHERE vt = ? AND idx = ?', (vt, slot // 4)).fetchone()
        return r[0] if r else None


def _cap(items):
    """the list capped, the rest counted: -> (shown, ', +N more' or '')"""
    return items[:CAP], (f', +{len(items) - CAP} more' if len(items) > CAP else '')


def _label(shape, off):
    if shape.startswith('V'):
        return f'slot +{off}'
    if shape == 'F':
        return f'field +{off}'
    return f'icall {shape}'


def _seen_text(obs, n):
    shown, more = _cap(obs)
    return f'seen {len(obs)}' + (f' of {n} fns' if n and n >= len(obs) else '') + ': ' + ', '.join(f'sub_{t:08X} x{k}' for t, k in shown) + more


def own_vptr_target(src, func, site, slot, fns):
    """F1: the one function of icall's set that is the slot word of the class this function stored at the receiver before the site, else None"""
    root = src.recv_root(func, site, slot)
    vts = src.own_vtables(func, root)
    if not vts or not src.stored_before(func, site, root):
        return None
    got = {src.slot_func(vt, slot) for vt in vts} & set(fns)
    return got.pop() if len(got) == 1 else None


def site_text(src, func, site, shape, off, status, n):
    """-> (text, weight) of one site, or None when nothing useful is known"""
    lbl = _label(shape, off)
    tg = src.targets(site) if status in ('exact', 'narrow', 'cand') else []
    fns = sorted({t for t, h in tg})
    how = '/'.join(sorted({h for t, h in tg}))
    if status == 'exact' and len(fns) == 1:
        return f'{lbl} [C] sub_{fns[0]:08X} (icall {how})', 6
    obs = src.seen(site)
    if status == 'narrow' and fns:
        one = own_vptr_target(src, func, site, off, fns) if shape.startswith('V') and src.fcon is not None else None
        if one is not None:
            return f'{lbl} [C] sub_{one:08X} (own vptr store before the site; icall {how} narrow {len(fns)})', 6
        shown, more = _cap(fns)
        txt = f'{lbl} [L] narrow {len(fns)}: ' + ', '.join(f'sub_{t:08X}' for t in shown) + more + f' (icall {how})'
        return txt + (' | ' + _seen_text(obs, None) if obs else ''), 3
    parts = [_seen_text(obs, n if status == 'offset' else None)] if obs else []
    if status == 'cand' and fns:
        shown, more = _cap(fns)
        parts.append(f'cand {len(fns)}: ' + ', '.join(f'sub_{t:08X}' for t in shown) + more + f' (icall {how})')
    if not parts:
        return None
    return f'{lbl} [L] ' + ' | '.join(parts), 2


def facts_for(src, func, size=None):
    out = []
    for site, shape, off, status, n in src.sites(func):
        if size and not 0 <= site - func < size:
            continue
        r = site_text(src, func, site, shape or '', off or 0, status, n)
        if r is not None:
            out.append(qfactcard.Fact('vcall', r[0], site - func, None, 50, r[1]))
    return out


def provider(ctx):
    """qfactcard provider: the facts of every indirect call site of ctx.addr"""
    if _SRC:
        src = _SRC[0]
    else:
        if not _REAL:
            _REAL.append(Sources.real())
        src = _REAL[0]
    return facts_for(src, ctx.addr, ctx.size)


def set_sources(src):
    """selftest: use these stores (None = the real files)"""
    _SRC[:] = [src] if src is not None else []
