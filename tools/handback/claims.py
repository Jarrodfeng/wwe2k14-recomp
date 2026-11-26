#!/usr/bin/env python3
r"""S111 LODGE: the ```claims block of a probe note: parse and check (used by note_sites.py --check and by lodge_note.py).

FORMAT. A fenced block opened by a line "```claims" at column 0 (anywhere in the note, once), one row per line, fields split by " | ":

  ADDRESS | MARK | CLAIM | evidence: WHAT YOU READ [| KIND KEY [NAME]] [| seam=TOPIC ours=PATH:LINE verdict=match|replace|repeat] [| system=render]

  TYPE column (S111 INDEX_PLAN lane B, optional, old 4-column rows still parse): `KIND KEY [NAME]`, what the row is ABOUT, so q prints it where an
  agent looks. Kinds and key grammar: tools\q\qfactkey.py (site sub_X+OFF | role sub_X | field vt:VT+OFF, g:ADDR[+OFF], ctor:sub_X+OFF |
  embed vt:A+OFF => vt:B | enum arg:sub_X.rN=V | when sub_X). A site key must be an instruction of that function; an embed row is [C] only when a
  constructor stores the vptr at this+OFF; a field / embed vtable is one the indexes know. lodge_note.py writes kind / key / name (and `site`, the
  instruction address of a site row) into db\claims\*.json.
  ADDRESS  0x82331BC0 or sub_82331BC0: a function (any address inside it) or a data address, 0x82..... / 0x83.....
  MARK     C (confirmed in the code or a log), L (likely), U (unknown / open)
  CLAIM    one sentence in OUR words: what the code at that address does or holds (never a listing, never an instruction)
  evidence the game-side read that supports it (`decomp sub_X +0x40`, `field 1176 --writes`, `run 328 log`), no code text
  optional fields (lodge_note.py writes them into db\seams\TOPIC.json via seam_wire.py): seam=TOPIC ours=PATH:LINE
  verdict=match|replace|repeat; system=camera|render|match|assets|audio|io|ui|other (the db `system` of a NEW entry)
Blank lines and lines starting with # are ignored. check_rows: every address must resolve (function, or data the index knows), the mark is
C / L / U, the claim and the evidence are non-empty and short, and no game code appears (tools\db\validate.py claim_code: any mnemonic with
a register / condition field / displacement / branch target, a bare blr / bctrl, a generated-code fragment: the db holds summaries only).
The code index is the MAIN checkout's (tools\q\q.py REPO; game addresses are the same on every branch); the db entries that make a data
address known are read from the checkout being landed (--root), so a data entry the lane itself adds counts. Stdlib only; 0 game runs."""
import importlib.util
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def load_factkey():
    """tools\\q\\qfactkey.py (the TYPE column's key grammar), loaded by path from this checkout."""
    spec = importlib.util.spec_from_file_location("qfactkey", os.path.join(REPO, "tools", "q", "qfactkey.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m
FENCE_RE = re.compile(r"^(`{3,})\s*([A-Za-z]*)\s*$")
ADDR_RE = re.compile(r"^(?:sub_|0x)([0-9A-Fa-f]{8})$")
SYSTEMS = ("camera", "render", "match", "assets", "audio", "io", "ui", "other")
VERDICTS = ("match", "replace", "repeat")
MAX_CLAIM = 300
MAX_EVIDENCE = 200


class ClaimsError(Exception):
    pass


def parse_claims(text):
    """-> list of rows {line, addr (int), mark, claim, evidence, seam, ours, verdict, system, raw} or [] when the note has no ```claims
    block. Raises ClaimsError for a malformed row / block. Only the format is checked here; resolve and code text: check_rows."""
    lines = text.lstrip("﻿").replace("\r\n", "\n").split("\n")
    rows, blocks, i, n = [], 0, 0, len(lines)
    while i < n:
        fm = FENCE_RE.match(lines[i])
        if not fm:
            i += 1
            continue
        ticks, tag = fm.groups()
        j = i + 1
        while j < n:
            cm = FENCE_RE.match(lines[j])
            if cm and cm.group(2) == "" and len(cm.group(1)) >= len(ticks):
                break
            j += 1
        if j >= n:
            raise ClaimsError("line %d: fence is never closed" % (i + 1))
        if tag.lower() == "claims":
            blocks += 1
            if blocks > 1:
                raise ClaimsError("line %d: two ```claims blocks (one block, one row per claim)" % (i + 1))
            for k in range(i + 1, j):
                ln = lines[k].strip()
                if ln and not ln.startswith("#"):
                    rows.append(_row(ln, k + 1))
        i = j + 1
    return rows


def _row(ln, lineno):
    f = [x.strip() for x in ln.split(" | ")]
    if len(f) < 4:
        raise ClaimsError("line %d: a row is `ADDRESS | MARK | CLAIM | evidence: ...` (found %d fields)" % (lineno, len(f)))
    m = ADDR_RE.match(f[0])
    if not m:
        raise ClaimsError("line %d: address %r is not 0x82331BC0 / sub_82331BC0 (8 hex digits)" % (lineno, f[0]))
    mark = f[1].upper()
    if mark not in ("C", "L", "U"):
        raise ClaimsError("line %d: mark %r is not C, L or U" % (lineno, f[1]))
    if not f[2]:
        raise ClaimsError("line %d: the claim is empty" % lineno)
    if not re.match(r"(?i)evidence\s*:\s*\S", f[3]):
        raise ClaimsError("line %d: the fourth field must be `evidence: <what you read>` (not empty)" % lineno)
    row = {"line": lineno, "addr": int(m.group(1), 16), "mark": mark, "claim": f[2],
           "evidence": re.sub(r"(?i)^evidence\s*:\s*", "", f[3]), "seam": None, "ours": None, "verdict": None,
           "system": None, "kind": None, "key": None, "name": None, "site": None, "typed": None, "raw": ln}
    for extra in f[4:]:
        fk = load_factkey()
        if fk.is_type_field(extra):
            if row["kind"]:
                raise ClaimsError("line %d: two TYPE columns (one `kind key [name]` per row)" % lineno)
            try:
                t = fk.parse_type(extra)
            except fk.FactKeyError as e:
                raise ClaimsError("line %d: TYPE column: %s" % (lineno, e))
            row.update(kind=t["kind"], key=t["key"], name=t["name"], typed=t, site=t.get("addr"))
            continue
        for tok in extra.split():
            if "=" not in tok:
                raise ClaimsError("line %d: optional field %r is not key=value (seam= ours= verdict= system=)" % (lineno, tok))
            k, v = tok.split("=", 1)
            if k not in ("seam", "ours", "verdict", "system") or not v:
                raise ClaimsError("line %d: unknown or empty optional field %r (seam= ours= verdict= system=)" % (lineno, k))
            if k == "verdict" and v not in VERDICTS:
                raise ClaimsError("line %d: verdict %r is not match|replace|repeat" % (lineno, v))
            if k == "system" and v not in SYSTEMS:
                raise ClaimsError("line %d: system %r is not one of %s" % (lineno, v, "|".join(SYSTEMS)))
            if k == "ours" and not re.search(r":\d+$", v):
                raise ClaimsError("line %d: ours=%r needs PATH:LINE" % (lineno, v))
            row[k] = v
    return row


def load_validate():
    """tools\\db\\validate.py as a module (its code-text patterns are the one definition of 'looks like game code')."""
    spec = importlib.util.spec_from_file_location("db_validate", os.path.join(REPO, "tools", "db", "validate.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def code_text(text, val):
    """-> a reason string when `text` carries game code (validate.py claim_code: the one definition, also applied to db\\claims\\*.json), else None."""
    frag = val.claim_code(text)
    return "instruction / generated-code text %r" % frag if frag else None


class QResolver:
    """Addresses through the code index (tools\\q\\q.py, `q addr`): -> ('func', start) for any address inside a function, ('data', a) for
    an address the index knows as data (a code reference, a pointer word, a string, a vtable slot, or an existing db entry), else None."""
    def __init__(self, root=None):
        import glob
        import json
        import sys
        d = os.path.join(REPO, "tools", "q")
        if d not in sys.path:
            sys.path.insert(0, d)
        import q
        self.q = q
        self.con = q.connect(q.DB)
        self._ix = None
        self.db_addrs = set()
        for p in glob.glob(os.path.join(root or REPO, "db", "*.json")):   # the landed checkout's db, not q's (q.REPO is the main checkout)
            try:
                with open(p, encoding="utf-8-sig") as f:
                    d = json.load(f)
            except (OSError, ValueError):
                continue
            for e in d if isinstance(d, list) else []:
                try:
                    self.db_addrs.add(int(str(e.get("address")), 16))
                except (AttributeError, ValueError):
                    pass

    @property
    def ix(self):
        """the field index + listing for TYPE rows (qfactkey.IndexFacts); None when the field index is not built (field / embed keys are then
        checked for their form only)."""
        if self._ix is None:
            try:
                self._ix = load_factkey().IndexFacts()
            except Exception:   # noqa: BLE001
                self._ix = False
        return self._ix or None

    def resolve(self, a):
        c = self.q.containing(self.con, a)
        if c:
            return ("func", c["addr"])
        if self.con.execute("SELECT 1 FROM funcs WHERE addr = ?", (a,)).fetchone():
            return ("func", a)
        con = self.con
        if (a in self.db_addrs
                or con.execute("SELECT 1 FROM imms WHERE value = ? AND op LIKE 'lis+%' LIMIT 1", (a,)).fetchone()
                or con.execute("SELECT 1 FROM ptrs WHERE at = ?", (a,)).fetchone()):
            return ("data", a)
        st = con.execute("SELECT addr, enc, text FROM strings WHERE addr <= ? ORDER BY addr DESC LIMIT 1", (a,)).fetchone()
        if st and a < st["addr"] + len(st["text"]) * (2 if st["enc"] == "utf16be" else 1) + 1:
            return ("data", a)
        vt = con.execute("SELECT v.vt, v.nslots FROM rtti_vtable v WHERE v.vt <= ? ORDER BY v.vt DESC LIMIT 1", (a,)).fetchone()
        if vt and a < vt["vt"] + 4 * max(1, vt["nslots"]):
            return ("data", a)
        return None


class TableResolver:
    """A fixed table (selftest, --resolver-table): funcs = {start: size}, data = [addr, ...]."""
    def __init__(self, funcs, data, facts=None):
        self.funcs = {int(k, 16) if isinstance(k, str) else k: v for k, v in funcs.items()}
        self.data = {int(x, 16) if isinstance(x, str) else x for x in data}
        self.ix = load_factkey().TableFacts(facts) if facts else None   # vtables / constructors / embeds for TYPE rows

    def resolve(self, a):
        for s, size in self.funcs.items():
            if s <= a < s + size:
                return ("func", s)
        return ("data", a) if a in self.data else None


def check_rows(rows, resolver):
    """-> list of error strings (empty = OK). Every address resolves; no game code; text short."""
    val = load_validate()
    errs = []
    for r in rows:
        where = "line %d (0x%08X)" % (r["line"], r["addr"])
        if resolver is None:
            errs.append("%s: no code index to resolve the address (build it: py tools\\q\\build_index.py)" % where)
        elif resolver.resolve(r["addr"]) is None:
            errs.append("%s: the address does not resolve (not inside a function and not a data address the index knows: `q addr`)" % where)
        if r.get("typed") and resolver is not None:
            for e in load_factkey().check_typed(r["typed"], r["addr"], r["mark"], resolver):
                errs.append("%s: TYPE column: %s" % (where, e))
        for what, text, cap in (("claim", r["claim"], MAX_CLAIM), ("evidence", r["evidence"], MAX_EVIDENCE)):
            why = code_text(text, val)
            if why:
                errs.append("%s: the %s carries game code (%s): say it in our own words, name where you read it" % (where, what, why))
            if len(text) > cap:
                errs.append("%s: the %s is %d characters (at most %d): one sentence" % (where, what, len(text), cap))
    return errs
