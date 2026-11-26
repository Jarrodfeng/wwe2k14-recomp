#!/usr/bin/env python3
r"""S111 INDEX_PLAN lane B (T2 TYPED LODGE): the key grammar of the TYPE column of a claims row, and the embed normalisation.

A claims row is `ADDRESS | MARK | CLAIM | evidence: ... [| KIND KEY [NAME]] [| seam=.. ours=..]`. The optional TYPE column says what the
row is ABOUT, so a tool can print it where an agent looks (a site, a field, a constant), not only on the function:

  kind   key                                   value (the claim text, plus the optional NAME)
  site   sub_X+OFF                             what happens at that instruction of that function (OFF multiple of 4, inside it)
  role   sub_X                                 the function's role
  field  vt:VT+OFF | g:ADDR[+OFF] | ctor:sub_X+OFF    name / meaning of that field (OFF decimal or 0xHEX, may be negative)
  embed  vt:A+OFF => vt:B                      the object of type A holds a sub-object of type B at +OFF
  enum   arg:sub_X.rN=V                        the meaning of literal V given in register rN to the consumer sub_X
  when   sub_X                                 task group / phase / walk of the function (a computed answer; reserved for a later lane)

EMBED NORMALISATION. A sub-object's own methods see `this` = the sub-object: sub_825A3908 reads `[in_r3-976]`, which is the character's +224
because the constructor sub_825A20A0 stores the vptr 0x8201E6D8 at character+1200 (the opaque draw node: vtable 0x8201E6D8 slot 1 is
sub_825A3908). An embed row is [C] ONLY when a constructor stores the vptr at this+K: derive_embeds() reads that from the field index
(`rows`: a 4-byte store at a0+K of a constructor / destructor of type A, the field index's `ctors`) and the listing (vptr_source(): the `addi`
of a vtable address, the imms table's flow-sure `lis+addi`, that set the stored register with nothing between that can change it: no other
writer, no call for a volatile register, no join label). `normalise()` and the CLI print the raw access beside the normalised key.

  py -I tools\q\qfactkey.py embed sub_825A3908 -976          [in_r3-976] of that slot function -> vt:0x8201E6E8+224
  py -I tools\q\qfactkey.py embeds [--outer VT] [--inner VT] the derived embed rows
  py -I tools\q\qfactkey.py field OFF --type VT [--claims DIR]   the field's accesses (q field) with the lodged claims on the field key
                                                                 and on each access site (what `q field` prints inline for Lane A)

PROVIDER API (for qfield / qdecomp hooks): load_typed(claims_dir) -> rows with a `kind`; facts_for_field(rows, vt, off, ix=None) ->
the field-kind claims for that field (through embeds both ways); facts_at_site(rows, addr) -> the claims whose cited site or own address is
that instruction. Stdlib only, reads the indexes read-only; 0 game runs."""
import argparse
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
KINDS = ("site", "role", "field", "embed", "enum", "when")
MAX_NAME = 80
_OFF = r"[+-](?:0x[0-9A-Fa-f]+|\d+)"
_FN = r"sub_([0-9A-Fa-f]{8})"
_ADDR = r"(?:0x)?([0-9A-Fa-f]{8})"


class FactKeyError(Exception):
    pass


def _num(s):
    s = s.strip()
    neg = s.startswith("-")
    t = s.lstrip("+-")
    try:   # 0xHEX or decimal; a leading-zero decimal (`+0040`) is refused, not guessed (decomp prints hex without 0x)
        v = int(t, 16) if t[:2].lower() == "0x" else int(t, 10) if (t == "0" or not t.startswith("0")) else None
    except ValueError:
        v = None
    if v is None:
        raise FactKeyError("offset %r is not decimal or 0xHEX (a leading zero is ambiguous: write 0x40 or 64)" % s)
    return -v if neg else v


def fmt_off(n):
    return "%+d" % n


def _vt(m_addr, m_off=None):
    a = int(m_addr, 16)
    return a, ("vt:0x%08X" % a) + (fmt_off(_num(m_off)) if m_off else "")


def parse_key(kind, key):
    """-> dict {kind, key (canonical), plus parts}. Raises FactKeyError with the grammar line that was broken."""
    if kind not in KINDS:
        raise FactKeyError("kind %r is not one of %s" % (kind, "|".join(KINDS)))
    key = " ".join(key.split())
    if kind == "site":
        m = re.fullmatch(_FN + r"\+(0x[0-9A-Fa-f]+|\d+)", key)
        if not m:
            raise FactKeyError("site key %r is not sub_XXXXXXXX+OFF (OFF decimal or 0xHEX)" % key)
        f, off = int(m.group(1), 16), _num(m.group(2))
        if off % 4:
            raise FactKeyError("site key %r: the offset 0x%X is not a multiple of 4 (an instruction)" % (key, off))
        return {"kind": kind, "key": "sub_%08X+0x%X" % (f, off), "func": f, "off": off, "addr": f + off}
    if kind in ("role", "when"):
        m = re.fullmatch(_FN, key)
        if not m:
            raise FactKeyError("%s key %r is not sub_XXXXXXXX" % (kind, key))
        f = int(m.group(1), 16)
        return {"kind": kind, "key": "sub_%08X" % f, "func": f}
    if kind == "field":
        return _field_key(key)
    if kind == "embed":
        m = re.fullmatch(r"(vt:\S+?)\s*=>\s*(vt:\S+)", key)
        if not m:
            raise FactKeyError("embed key %r is not vt:A+OFF => vt:B" % key)
        outer, inner = _field_key(m.group(1)), _field_key(m.group(2), need_off=False)
        if outer["root"] != "vt" or inner["root"] != "vt":
            raise FactKeyError("embed key %r: both sides are vt:ADDR (the outer with +OFF)" % key)
        if "off" not in outer:
            raise FactKeyError("embed key %r: the outer side needs +OFF" % key)
        if inner.get("off"):
            raise FactKeyError("embed key %r: the inner side is a type, no offset" % key)
        return {"kind": kind, "key": "vt:0x%08X%s => vt:0x%08X" % (outer["vt"], fmt_off(outer["off"]), inner["vt"]),
                "outer": outer["vt"], "off": outer["off"], "inner": inner["vt"]}
    m = re.fullmatch(r"arg:" + _FN + r"\.r(\d+)=(-?(?:0x[0-9A-Fa-f]+|\d+))", key)
    if not m or int(m.group(2)) > 31:
        raise FactKeyError("enum key %r is not arg:sub_XXXXXXXX.rN=V (N 0..31, V decimal or 0xHEX)" % key)
    f, n, v = int(m.group(1), 16), int(m.group(2)), _num(m.group(3))
    return {"kind": kind, "key": "arg:sub_%08X.r%d=%d" % (f, n, v), "func": f, "reg": n, "value": v}


def _field_key(key, need_off=True):
    m = re.fullmatch(r"vt:" + _ADDR + "(" + _OFF + ")?", key)
    if m:
        if need_off and not m.group(2):
            raise FactKeyError("field key %r needs +OFF" % key)
        a, canon = _vt(m.group(1), m.group(2))
        d = {"kind": "field", "key": canon, "root": "vt", "vt": a}
        if m.group(2):
            d["off"] = _num(m.group(2))
        return d
    m = re.fullmatch(r"g:" + _ADDR + "(" + _OFF + ")?", key)
    if m:
        a = int(m.group(1), 16)
        off = _num(m.group(2)) if m.group(2) else 0
        return {"kind": "field", "key": "g:0x%08X" % a + (fmt_off(off) if m.group(2) else ""), "root": "g", "global": a, "off": off}
    m = re.fullmatch(r"ctor:" + _FN + "(" + _OFF + ")", key)
    if m:
        f, off = int(m.group(1), 16), _num(m.group(2))
        return {"kind": "field", "key": "ctor:sub_%08X%s" % (f, fmt_off(off)), "root": "ctor", "func": f, "off": off}
    raise FactKeyError("field key %r is not vt:VT+OFF, g:ADDR[+OFF] or ctor:sub_X+OFF" % key)


def parse_type(text):
    """The TYPE column text `kind key [name]` -> dict {kind, key (canonical), name|None, ...parts}. Raises FactKeyError."""
    text = text.strip()
    parts = text.split(None, 1)
    if len(parts) < 2:
        raise FactKeyError("the TYPE column is `kind key [name]` (found %r)" % text)
    kind, rest = parts
    if kind == "embed":
        m = re.fullmatch(r"(\S+)\s*=>\s*(\S+)(?:\s+(.*))?", rest)
        if not m:
            raise FactKeyError("embed: `embed vt:A+OFF => vt:B [name]`")
        key, name = m.group(1) + " => " + m.group(2), m.group(3)
    else:
        kp = rest.split(None, 1)
        key, name = kp[0], (kp[1] if len(kp) > 1 else None)
    d = parse_key(kind, key)
    name = name.strip() if name else None
    if name and (len(name) > MAX_NAME or "|" in name):
        raise FactKeyError("the name is at most %d characters, no '|'" % MAX_NAME)
    d["name"] = name or None
    return d


def is_type_field(text):
    """True when a row field is the TYPE column (its first word is a kind and has no '='), else it is an optional seam= / ours= field."""
    w = text.strip().split(None, 1)
    return bool(w) and w[0] in KINDS and "=" not in w[0]


# ----------------------------------------------------------------------------------------------------- checks against an index

def check_typed(t, row_addr, mark, res):
    """-> error strings for a parsed TYPE dict `t` of a row at `row_addr`. `res` = the claims resolver (resolve(a); optional .ix with
    known_vt / ctor_vt / embed_ok, absent for a table resolver without them)."""
    errs = []
    ix = getattr(res, "ix", None)
    kind = t["kind"]

    def func_ok(f):
        r = res.resolve(f)
        return r is not None and r[0] == "func" and r[1] == f

    if kind == "site":
        r = res.resolve(t["addr"])
        if r is None or r[0] != "func" or r[1] != t["func"]:
            errs.append("site %s is not an instruction of sub_%08X (the offset is outside the function or the function is not in the index)" % (t["key"], t["func"]))
        ra = res.resolve(row_addr)
        if ra is not None and ra[0] == "func" and ra[1] != t["func"]:
            errs.append("site %s is in another function than the row's address 0x%08X (sub_%08X)" % (t["key"], row_addr, ra[1]))
    elif kind in ("role", "when", "enum"):
        if not func_ok(t["func"]):
            errs.append("%s key %s: sub_%08X is not a function start in the index" % (kind, t["key"], t["func"]))
    elif kind == "field":
        if t["root"] == "vt" and ix is not None and not ix.known_vt(t["vt"]):
            errs.append("field key %s: 0x%08X is not a vtable a constructor stores (q field info; q rtti NAME)" % (t["key"], t["vt"]))
        elif t["root"] == "g" and res.resolve(t["global"]) is None:
            errs.append("field key %s: 0x%08X is not a data address the index knows" % (t["key"], t["global"]))
        elif t["root"] == "ctor" and ix is not None and ix.ctor_vt(t["func"]) is None:
            errs.append("field key %s: sub_%08X is not a constructor in the field index" % (t["key"], t["func"]))
    elif kind == "embed":
        if ix is not None:
            for k in ("outer", "inner"):
                if not ix.known_vt(t[k]):
                    errs.append("embed key %s: 0x%08X is not a vtable a constructor stores" % (t["key"], t[k]))
            if mark == "C" and not errs and not ix.embed_ok(t["outer"], t["off"], t["inner"]):
                errs.append("embed %s is marked C but no constructor of 0x%08X stores the vptr 0x%08X at +%d (the field index shows none): "
                            "an embed row is [C] only when a constructor stores it (use mark L)" % (t["key"], t["outer"], t["inner"], t["off"]))
    return errs


# ----------------------------------------------------------------------------------------------------- the field / code index

class TableFacts:
    """A fixed fixture for the selftest: {"vts": [addr], "ctors": {func: vt}, "embeds": [[outer, off, inner]], "slots": {func: [vt]}}."""
    def __init__(self, t):
        h = lambda x: int(x, 16) if isinstance(x, str) else x
        self.vts = {h(v) for v in t.get("vts", [])}
        self.ctors = {h(k): h(v) for k, v in t.get("ctors", {}).items()}
        self.emb = [(h(o), k, h(i)) for o, k, i in t.get("embeds", [])]
        self.slots = {h(k): [h(x) for x in v] for k, v in t.get("slots", {}).items()}

    def known_vt(self, v):
        return v in self.vts

    def ctor_vt(self, f):
        return self.ctors.get(f)

    def embeds(self, outer=None, inner=None):
        return [{"outer": o, "off": k, "inner": i, "ctor": None, "store": None} for o, k, i in self.emb
                if (outer is None or o == outer) and (inner is None or i == inner)]

    def embed_ok(self, o, k, i):
        return (o, k, i) in self.emb

    def slot_types(self, f):
        return self.slots.get(f, [])


VOLATILE = {"r0"} | {"r%d" % i for i in range(3, 13)}   # not kept across a call (r3..r12, r0)
_NO_DEST = ("st", "cmp", "tw", "td", "mt", "dcb", "icb", "b", "sync", "lwsync", "eieio", "isync", "nop")


def writes_reg(ins, reg):
    """True when the instruction can change register `reg`: its destination (the first operand of anything but a store, compare, trap, mt*,
    cache op or branch), the base of a load / store update form, lmw's range."""
    mn = ins.mn.rstrip(".+-")
    ops = ins.ops.replace(" ", "").split(",") if ins.ops else []
    if mn == "lmw":
        return bool(ops) and ops[0][1:].isdigit() and reg[1:].isdigit() and int(reg[1:]) >= int(ops[0][1:])
    if mn.startswith(("l", "st")) and mn.endswith(("u", "ux")) and len(ops) > 1 and (ops[1] == reg or ops[1].endswith("(%s)" % reg)):
        return True
    return bool(ops) and ops[0] == reg and not mn.startswith(_NO_DEST)


def vptr_source(insns, labels, k):
    """-> the index of the `addi` that set the register the store insns[k] writes, when nothing between them can have changed it (another writer,
    a call while the register is volatile, a join label: another path reaches the store), else None. The S111 review of lane B: a walk that
    skipped writers outside a mnemonic list derived 50+ false embeds (sub_82398EA8 0x82398FC4 stores a byte load, not the vptr loaded before)."""
    m = re.match(r"^(r\d+),", insns[k].ops.replace(" ", ""))
    if not m:
        return None
    reg = m.group(1)
    for j in range(k - 1, max(-1, k - 64), -1):
        if insns[j + 1].addr in labels:
            return None
        p = insns[j]
        mn = p.mn.rstrip(".+-")
        if reg in VOLATILE and mn.startswith("b") and mn.endswith("l"):
            return None
        if writes_reg(p, reg):
            return j if mn == "addi" else None
    return None


class IndexFacts:
    """The real indexes (read-only): fields.s<N>.db (vtables, ctors, slots, rows) and the code index (funcs, imms) + the generated listing."""
    def __init__(self, fields_db=None, index_db=None, gen=None):
        sys.path.insert(0, HERE)
        import build_fields as bf
        import q
        g, _image, fdb = bf.default_paths(None, gen)
        self.gen = g
        self.fcon = sqlite3.connect("file:%s?mode=ro" % (fields_db or fdb), uri=True)
        self.icon = q.connect(index_db or q.DB)
        self.image = _image
        self._img = None
        self._emb = {}
        self._vts = None

    def known_vt(self, v):
        """a vtable a constructor stores (field index), or a table the code index catalogues / RTTI names (an embedded sub-object's vtable is
        only ever stored at a non-zero offset, so the field index alone does not know it)."""
        if self._vts is None:
            self._vts = {r[0] for r in self.fcon.execute("select vt from vtables")}
        if v in self._vts:
            return True
        c = self.icon
        if c.execute("select 1 from rtti_vtable where vt=?", (v,)).fetchone():
            return True
        try:
            return c.execute("select 1 from tables where start=?", (v,)).fetchone() is not None
        except sqlite3.Error:
            return False

    def ctor_vt(self, f):
        r = self.fcon.execute("select vt from ctors where func=?", (f,)).fetchone()
        return r[0] if r else None

    def slot_types(self, f):
        """the vtables that have f as a slot (the code index: q vtable --of), at most 3 (a function shared by more says nothing about its object)."""
        import qvtable
        if self._img is None:
            self._img = qvtable.Img(self.image)
        pts = qvtable.of_func(self.icon, self._img, f)["points"]
        vts = sorted({int(p["vtable"], 16) for p in pts if not p["table_by"].startswith("run-start")})
        return vts if len(vts) <= 3 else []

    def _listing(self, f):
        fr = self.icon.execute("select file, line, name from funcs where addr=?", (f,)).fetchone()
        if not fr or not fr[0]:
            return None
        import qdecomp
        path = os.path.join(self.gen, fr[0])
        if not os.path.exists(path):
            return None
        return qdecomp.read_listing(path, fr[1], fr[2], f)[:2]   # (insns, labels)

    def derive_ctor(self, f):
        """-> [{outer, off, inner, ctor, store}] for the constructor (or destructor) f: each 4-byte store at a0+K (K != 0) whose register holds,
        by vptr_source(), a flow-sure address-pair load of a vtable (the vptr of a sub-object at this+K)."""
        outer = self.ctor_vt(f)
        if outer is None:
            return []
        stores = self.fcon.execute("select r.addr, r.off from rows r join roots ro on ro.id=r.root where r.func=? and r.rw='W' and r.width=4 "
                                   "and ro.text='a0' and r.off<>0", (f,)).fetchall()
        if not stores:
            return []
        got = self._listing(f)
        if not got or not got[0]:
            return []
        insns, labels = got
        at = {ins.addr: k for k, ins in enumerate(insns)}
        out = []
        for addr, off in stores:
            k = at.get(addr)
            if k is None or insns[k].mn != "stw":
                continue
            m = re.match(r"^(r\d+),(-?\d+)\((r\d+)\)$", insns[k].ops.replace(" ", ""))
            if not m or int(m.group(2)) != off:
                continue
            j = vptr_source(insns, labels, k)
            if j is None:
                continue
            v = self.icon.execute("select value from imms where func=? and site=? and op='lis+addi'", (f, insns[j].addr)).fetchone()
            if v and self.known_vt(v[0]):
                out.append({"outer": outer, "off": off, "inner": v[0], "ctor": f, "store": addr})
        return out

    def embeds(self, outer=None, inner=None):
        """the derived embed rows; outer given = that type's constructors only; otherwise every constructor (slow: used for `inner` queries)."""
        if outer is not None:
            fs = [r[0] for r in self.fcon.execute("select func from ctors where vt=?", (outer,))]
        elif inner is not None:   # the functions that load that vtable address, which are constructors
            cands = {r[0] for r in self.icon.execute("select distinct func from imms where value=? and op like 'lis+%'", (inner,))}
            fs = [r[0] for r in self.fcon.execute("select func from ctors") if r[0] in cands]
        else:
            fs = [r[0] for r in self.fcon.execute("select c.func from ctors c where exists (select 1 from rows r join roots ro on ro.id=r.root "
                                                  "where r.func=c.func and r.rw='W' and r.width=4 and ro.text='a0' and r.off<>0)")]
        res = []
        for f in fs:
            if f not in self._emb:
                self._emb[f] = self.derive_ctor(f)
            res += [e for e in self._emb[f] if inner is None or e["inner"] == inner]
        return res

    def embed_ok(self, o, k, i):
        return any(e["off"] == k and e["inner"] == i for e in self.embeds(outer=o))


def normalise(ix, func, off):
    """The raw access `[in_r3+off]` inside `func`, a vtable slot of an embedded sub-object -> [{vt, off, via}] in the OUTER object's terms
    (one per embed row: type A holds the sub-object's type at +K, so the field is A+(K+off))."""
    res = {}
    for inner in ix.slot_types(func):
        for e in ix.embeds(inner=inner):
            g = res.setdefault((e["outer"], e["off"], inner), {"vt": e["outer"], "off": e["off"] + off, "inner": inner, "embed_off": e["off"], "stores": []})
            g["stores"].append((e["ctor"], e["store"]))
    return list(res.values())


# ----------------------------------------------------------------------------------------------------- the typed claims

def load_typed(claims_dir=None):
    """Every lodged claim row that has a `kind` (db\\claims\\*.json of this checkout)."""
    d = claims_dir or os.path.join(REPO, "db", "claims")
    out = []
    for p in sorted(glob.glob(os.path.join(d, "*.json"))):
        try:
            with open(p, encoding="utf-8") as f:
                rows = json.load(f)
        except (OSError, ValueError):
            continue
        for r in rows if isinstance(rows, list) else []:
            if isinstance(r, dict) and r.get("kind"):
                out.append(dict(r, file=os.path.basename(p)))
    return out


def facts_at_site(rows, addr):
    """The typed rows about instruction `addr`: kind site with that address, or any typed row whose own address is it."""
    out = []
    for r in rows:
        try:
            own = int(str(r.get("address")), 16)
            site = int(str(r.get("site")), 16) if r.get("site") else None
        except ValueError:
            continue
        if own == addr or site == addr:
            out.append(r)
    return out


def facts_for_field(rows, vt, off, ix=None):
    """The field-kind rows for vt:VT+OFF; with `ix`, also the rows keyed through an embed (the same word seen from the outer or the inner type)."""
    want = {("vt:0x%08X" % vt) + fmt_off(off)}
    if ix is not None:
        for e in ix.embeds(outer=vt):
            want.add(("vt:0x%08X" % e["inner"]) + fmt_off(off - e["off"]))
        for e in ix.embeds(inner=vt):
            want.add(("vt:0x%08X" % e["outer"]) + fmt_off(off + e["off"]))
    return [r for r in rows if r.get("kind") == "field" and r.get("key") in want]


def fact_line(r):
    where = ("  site %s" % r["key"]) if r.get("kind") == "site" else ""
    nm = (" " + r["name"] + ":") if r.get("name") else ""
    return "[%s]%s %s (note %s; %s %s)%s" % (r.get("mark"), nm, r.get("claim"), os.path.splitext(os.path.basename(r.get("note", "?")))[0], r.get("kind"), r.get("key"), where)


# ----------------------------------------------------------------------------------------------------- CLI

def cmd_embed(a):
    ix = IndexFacts()
    f = int(re.sub(r"^(?:sub_|0x)", "", a.func), 16)
    off = _num(a.off)
    got = normalise(ix, f, off)
    print("sub_%08X [in_r3%s]  (vtable slot of: %s)" % (f, fmt_off(off), ", ".join("0x%08X" % v for v in ix.slot_types(f)) or "none"))
    if not got:
        print("  no embed row: the function is not a slot of a type a constructor embeds at a non-zero offset (the raw access stays as printed)")
        return 1
    for g in got:
        print("  = vt:0x%08X%s   via embed vt:0x%08X%s => vt:0x%08X  [C: a constructor / destructor of the outer type stores the vptr: %s]"
              % (g["vt"], fmt_off(g["off"]), g["vt"], fmt_off(g["embed_off"]), g["inner"],
                 ", ".join("sub_%08X at 0x%08X" % s for s in g["stores"])))
    return 0


def cmd_embeds(a):
    ix = IndexFacts()
    n = 0
    for e in ix.embeds(outer=int(a.outer, 16) if a.outer else None, inner=int(a.inner, 16) if a.inner else None):
        print("vt:0x%08X%s => vt:0x%08X   ctor sub_%08X store 0x%08X" % (e["outer"], fmt_off(e["off"]), e["inner"], e["ctor"], e["store"]))
        n += 1
    print("-- %d embed rows" % n)
    return 0


def cmd_field(a):
    off, vt = _num(a.off), int(a.type, 16)
    qpy = os.path.join(HERE, "q.py")
    r = subprocess.run([sys.executable, "-I", qpy, "field", str(off), "--type", a.type, "--json"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        acc = json.loads(r.stdout).get("rows", [])
    except ValueError:
        print("qfactkey: q field failed: " + (r.stdout + r.stderr).strip()[:300])
        return 2
    rows = load_typed(a.claims)
    try:
        ix = IndexFacts()
    except Exception:   # noqa: BLE001 (no field index: direct keys only)
        ix = None
    print("field vt:0x%08X%s" % (vt, fmt_off(off)))
    for t in facts_for_field(rows, vt, off, ix):
        print("  " + fact_line(t))
    for x in acc:
        print("  %s  %s w%s %s" % (x["addr"], x["rw"], x["width"], x["func"]))
        for t in facts_at_site(rows, int(x["addr"], 16)):
            print("      " + fact_line(t))
    print("-- %d accesses" % len(acc))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("embed"); p.add_argument("func"); p.add_argument("off")
    p = sub.add_parser("embeds"); p.add_argument("--outer"); p.add_argument("--inner")
    p = sub.add_parser("field"); p.add_argument("off"); p.add_argument("--type", required=True); p.add_argument("--claims", default=None)
    p = sub.add_parser("parse"); p.add_argument("text")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "parse":
            print(json.dumps(parse_type(a.text), sort_keys=True))
            return 0
        return {"embed": cmd_embed, "embeds": cmd_embeds, "field": cmd_field}[a.cmd](a)
    except FactKeyError as e:
        print("qfactkey: " + str(e))
        return 1


if __name__ == "__main__":
    sys.exit(main())
