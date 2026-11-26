#!/usr/bin/env python3
r"""S111 lane B review (main session decision, rule 6: one copy of a fact, one writer): the note's ```claims block is the ONLY source; db\claims
and the claims-driven db\seams rows are built from it by lodging and are never edited by hand. This carries a claims file's rows INTO the note's
block where a review edited the file by hand, so the next lodge is the identity.

  py -I tools\handback\pull_claims.py [--root DIR] [--dry] NOTE ...

Per note: the block's rows and the claims file's objects (db\claims\<sNNN>_<NOTE>.json, lodge_note.py claims_rel) are matched by address, in
order (difflib). A matched row whose mark, claim, evidence or seam fields (seam / ours / verdict / system) differ is rewritten from the file:
the note's address token and its TYPE column (`| site sub_X+OFF`, tools\q\qfactkey.py) are kept, the rest is
`ADDRESS | MARK | CLAIM | evidence: EVIDENCE [| TYPE] [| seam=.. ours=.. verdict=..] [| system=..]`. A row only in the file is inserted at its
place (untyped; tools\handback\type_sites.py can type it). A row only in the note is kept and listed (it is newer than the file, or the file
dropped it: the line says which row, a person decides). A file row the lodge would refuse (claims.py MAX_CLAIM / MAX_EVIDENCE, game code text,
a ' | ' inside a field) is listed and its note row left as it is. Line endings are kept. Then: `py -I tools\handback\lodge_note.py NOTE`.
Exit 0 = ran (per note: rewritten / inserted / kept / refused), 2 = could not run (no note, no claims file, a block that does not parse).
Stdlib only; 0 game runs."""
import argparse
import difflib
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CONTENT = ("mark", "claim", "evidence", "seam", "ours", "verdict", "system")


def load(name, path=None):
    spec = importlib.util.spec_from_file_location(name, path or os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def content(d):
    return tuple((d.get(k) or None) for k in CONTENT)


def render(addr_tok, o, type_text):
    """the claims-file object -> the block line (the TYPE column text kept as the note had it)"""
    line = "%s | %s | %s | evidence: %s" % (addr_tok, o["mark"], o["claim"], o["evidence"])
    if type_text:
        line += " | " + type_text
    seam = " ".join("%s=%s" % (k, o[k]) for k in ("seam", "ours", "verdict") if o.get(k))
    if seam:
        line += " | " + seam
    if o.get("system"):
        line += " | system=" + o["system"]
    return line


def refusal(o, cl, val):
    for k, cap in (("claim", cl.MAX_CLAIM), ("evidence", cl.MAX_EVIDENCE)):
        t = o.get(k) or ""
        if " | " in t or "\n" in t:
            return "the file's %s holds ' | ' or a line break" % k
        if len(t) > cap:
            return "the file's %s is %d characters (the lodge takes at most %d)" % (k, len(t), cap)
        if cl.code_text(t, val):
            return "the file's %s carries game code text (%s)" % (k, cl.code_text(t, val))
    return None


def pull(text, objs, cl, fk, val):
    """-> (new text, report {rewritten: [..], inserted: [..], kept: [..], refused: [..]})"""
    rows = cl.parse_claims(text)
    lines = text.split("\n")
    rep = {"rewritten": [], "inserted": [], "kept": [], "refused": []}
    edits = []   # (line index, "replace" | "insert-before" | "insert-after", [new lines])
    sm = difflib.SequenceMatcher(None, ["0x%08X" % r["addr"] for r in rows], [str(o.get("address")).upper().replace("0X", "0x") for o in objs], autojunk=False)
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        pairs = list(zip(range(i1, i2), range(j1, j2))) if op in ("equal", "replace") else []
        for i, j in pairs:
            r, o = rows[i], objs[j]
            if content(r) == content(o):
                continue
            why = refusal(o, cl, val)
            if why:
                rep["refused"].append("row %d (0x%08X): %s" % (i + 1, r["addr"], why))
                continue
            f = r["raw"].split(" | ")
            type_text = next((x.strip() for x in f[4:] if fk.is_type_field(x)), None)
            edits.append((r["line"] - 1, "replace", [render(f[0].strip(), o, type_text)]))
            rep["rewritten"].append("row %d (0x%08X): %s" % (i + 1, r["addr"], ", ".join(k for k in CONTENT if (r.get(k) or None) != (o.get(k) or None))))
        extra_note = range(i1 + len(pairs), i2)
        extra_file = range(j1 + len(pairs), j2)
        for i in extra_note:
            rep["kept"].append("row %d (0x%08X) is in the note only: %s" % (i + 1, rows[i]["addr"], rows[i]["claim"][:80]))
        new = []
        for j in extra_file:
            why = refusal(objs[j], cl, val)
            if why:
                rep["refused"].append("file row %d (%s): %s" % (j + 1, objs[j].get("address"), why))
                continue
            new.append(render(objs[j]["address"], objs[j], None))
            rep["inserted"].append("file row %d (%s): %s" % (j + 1, objs[j].get("address"), objs[j]["claim"][:80]))
        if new:
            if i1 + len(pairs) < len(rows):
                edits.append((rows[i1 + len(pairs)]["line"] - 1, "insert-before", new))
            else:
                edits.append((rows[-1]["line"] - 1 if rows else None, "insert-after", new))
    order = {"insert-after": 2, "replace": 1, "insert-before": 0}   # bottom to top; at one line: after it, the line, before it
    for k, how, new in sorted(edits, key=lambda e: (-1 if e[0] is None else e[0], order[e[1]]), reverse=True):
        if k is None:
            raise ValueError("the block has no row to insert after")
        cr = "\r" if lines[k].endswith("\r") else ""
        new = [x + cr for x in new]
        if how == "replace":
            lines[k:k + 1] = new
        elif how == "insert-before":
            lines[k:k] = new
        else:
            lines[k + 1:k + 1] = new
    return "\n".join(lines), rep


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("notes", nargs="+")
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    cl, ln = load("claims"), load("lodge_note")
    fk = load("qfactkey_pull", os.path.join(REPO, "tools", "q", "qfactkey.py"))
    val = cl.load_validate()
    rc = 0
    for n in a.notes:
        path = os.path.abspath(n) if os.path.isabs(n) or os.path.exists(n) else os.path.join(root, n)
        rel = os.path.relpath(path, root).replace("\\", "/")
        crel = ln.claims_rel(rel)
        try:
            with open(path, "rb") as f:
                text = f.read().decode("utf-8")
            with open(os.path.join(root, *crel.split("/")), encoding="utf-8") as f:
                objs = json.load(f)
            new, rep = pull(text, objs, cl, fk, val)
        except (OSError, ValueError, cl.ClaimsError) as e:
            print("pull_claims: %s: could not run: %s" % (rel, e))
            rc = 2
            continue
        print("pull_claims %s <- %s: %d rewritten, %d inserted, %d kept (note only), %d refused%s" % (
            rel, crel, len(rep["rewritten"]), len(rep["inserted"]), len(rep["kept"]), len(rep["refused"]), " (--dry: nothing written)" if a.dry else ""))
        for k in ("rewritten", "inserted", "kept", "refused"):
            for x in rep[k]:
                print("  %-9s %s" % (k, x))
        if new != text and not a.dry:
            with open(path, "wb") as f:
                f.write(new.encode("utf-8"))
            print("changed: " + rel)
    return rc


if __name__ == "__main__":
    sys.exit(main())
