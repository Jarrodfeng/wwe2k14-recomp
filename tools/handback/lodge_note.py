#!/usr/bin/env python3
r"""S111 LODGE: lodge a probe note's ```claims rows as the note's OWN claims file, so the next probe's `q db` / `q fact` starts from them.

  py -I tools\handback\lodge_note.py NOTE [--dry] [--root DIR] [--note-as REL]

NOTE holds a ```claims block (format: tools\handback\claims.py, docs\agents\note_sites.md; `note_sites.py --template` prints it). The rows
become ONE file, db\claims\<sNNN>_<NOTE>.json (docs\research\s110\ENTRBLACK.md -> db\claims\s110_ENTRBLACK.json; a note outside a session
folder -> db\claims\lodged_<NOTE>.json), a JSON list with one object per row, in the note's order:

    {"address": "0x82331BE0", "function": "0x82331BC0", "offset": "0x20", "mark": "C", "claim": "...", "evidence": "...",
     "note": "docs/research/s999/FIX.md" [, "seam": ..., "ours": ..., "verdict": ..., "system": ...]
     [, "kind": "site", "key": "sub_82331BC0+0x20", "name": ..., "site": "0x82331BE0"] [, "rev": "<note> #Review"]}
                                                                           (the TYPE column: tools\q\qfactkey.py; rev: tools\handback\rev_mark.py)

  - "function" / "offset": the function holding the address (the code index, `q addr`) and the address's offset in it; both null for a
    data address
  - the file is REWRITTEN WHOLE on every lodge of the note: lodging the same note twice changes nothing, an edited note replaces its rows,
    and deleting the file removes the note's claims as one unit. No existing db\*.json is ever read for writing or edited (S111 design:
    parallel lanes never meet in a shared db file; the claims are not function entries, so no placeholder entry is made)
  - a note with no ```claims block (or an empty one) lodges nothing and leaves an existing claims file as it is (a later round's note that
    dropped its block does not erase what an earlier round lodged; delete the file to remove them)
  - the rows are checked twice before anything is written: claims.py check_rows (every address resolves, no game code, short) and
    tools\db\validate.py check_claim_rows (the claims file schema; validate.py applies the same check to every db\claims\*.json)
  - `q db FUNC` / `q fn FUNC` list the claims on that function or address (tools\q\q.py claims_for); `q fact WORDS` searches them (kind claim)
Prints `changed: db/claims/FILE` when the file changed and `lodge_note: ...` summary lines. --dry: report only.
Exit: 0 ok (also: no block, unchanged), 1 refused (a row not OK: nothing written), 2 usage / unreadable. Stdlib only; 0 game runs."""
import argparse
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def load_claims():
    spec = importlib.util.spec_from_file_location("claims", os.path.join(HERE, "claims.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def default_note_rel(note_abs, root):
    rel = os.path.relpath(os.path.realpath(note_abs), os.path.realpath(root))
    if rel.startswith(".."):
        return "docs/research/landed_notes/" + os.path.basename(note_abs)
    return rel.replace("\\", "/")


def target_file(note_rel):
    """docs/research/s110/ENTRBLACK.md -> s110_ENTRBLACK.json; any other note -> lodged_<stem>.json."""
    parts = note_rel.replace("\\", "/").split("/")
    stem = re.sub(r"[^A-Za-z0-9]+", "_", os.path.splitext(parts[-1])[0]).strip("_") or "note"
    if len(parts) >= 2 and re.fullmatch(r"s\d+", parts[-2]):
        return "%s_%s.json" % (parts[-2], stem)
    return "lodged_%s.json" % stem


def claims_rel(note_rel):
    """The repo-relative path of the note's claims file (land_note.py watches it in the main checkout)."""
    return "db/claims/" + target_file(note_rel)


def build_rows(rows, resolver, note_rel):
    """parsed + checked rows -> the claims file's objects (no I/O)."""
    out = []
    for r in rows:
        kind, start = resolver.resolve(r["addr"])
        o = {"address": "0x%08X" % r["addr"],
             "function": "0x%08X" % start if kind == "func" else None,
             "offset": "0x%X" % (r["addr"] - start) if kind == "func" else None,
             "mark": r["mark"], "claim": r["claim"], "evidence": r["evidence"], "note": note_rel.replace("\\", "/")}
        for k in ("seam", "ours", "verdict", "system", "scope"):
            if r.get(k):
                o[k] = r[k]
        if r.get("kind"):   # the TYPE column (INDEX_PLAN T2): kind / key / name, and for a site row the instruction address
            o["kind"], o["key"] = r["kind"], r["key"]
            if r.get("name"):
                o["name"] = r["name"]
            if r["kind"] == "site":
                o["site"] = "0x%08X" % r["site"]
        out.append(o)
    return out


def render(objs):
    return json.dumps(objs, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="lodge a note's ```claims rows as db\\claims\\<sNNN>_<NOTE>.json")
    ap.add_argument("note")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--root", default=REPO, help="the checkout whose db\\claims\\ is written (default: this one)")
    ap.add_argument("--note-as", default=None, help="the note's repo-relative path (default: its place under --root)")
    ap.add_argument("--resolver-table", default=None, help=argparse.SUPPRESS)   # selftest: {"funcs": {"0x..": size}, "data": ["0x.."]}
    a = ap.parse_args(argv)
    cl = load_claims()
    try:
        with open(a.note, "rb") as f:
            text = f.read().decode("utf-8", "replace")
        rows = cl.parse_claims(text)
    except (OSError, cl.ClaimsError) as e:
        print("lodge_note: cannot read the claims: %s" % e)
        return 2
    root = os.path.abspath(a.root)
    note_rel = (a.note_as or default_note_rel(os.path.abspath(a.note), root)).replace("\\", "/")
    rel = claims_rel(note_rel)
    path = os.path.join(root, *rel.split("/"))
    if not rows:
        print("lodge_note: no claims block: nothing lodged%s (a ```claims block lists the [C] / [L] claims with an address: note_sites.py --template)"
              % ("; %s stays as it is" % rel if os.path.exists(path) else ""))
        return 0
    try:
        if a.resolver_table:
            with open(a.resolver_table, encoding="utf-8") as f:
                t = json.load(f)
            resolver = cl.TableResolver(t.get("funcs", {}), t.get("data", []), t.get("facts"))
        else:
            resolver = cl.QResolver(root)
    except Exception as e:   # noqa: BLE001 (no index: the same refusal as --check)
        print("lodge_note: no code index to resolve the addresses (%s: %s). Next: py tools\\q\\build_index.py" % (type(e).__name__, e))
        return 1
    errs = cl.check_rows(rows, resolver)
    objs = build_rows(rows, resolver, note_rel) if not errs else []
    if objs:   # S111 REV: the reviewed-fact mark, computed from git + the note (tools\handback\rev_mark.py), never written by hand
        spec = importlib.util.spec_from_file_location("rev_mark", os.path.join(HERE, "rev_mark.py"))
        rm = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rm)
        for o, v in zip(objs, rm.compute(root, note_rel, text, rows, cl)):
            if v:
                o["rev"] = v
    if not errs:   # the schema validate.py holds every db\claims file to (one definition)
        val = cl.load_validate()
        rep = val.Report()
        val.check_claim_rows(rep, rel, objs)
        errs = ["%s: %s" % (w, m) for _, w, m in rep.errors]
    if errs:
        for x in errs:
            print("lodge_note: REFUSED: " + x)
        print("lodge_note: %d claim rows, %d problems: nothing lodged" % (len(rows), len(errs)))
        return 1
    new = render(objs)
    try:
        with open(path, encoding="utf-8", newline="") as f:
            old = f.read()
    except OSError:
        old = None
    n_seam = sum(1 for r in rows if r["seam"] or r["ours"] or r["verdict"])
    print("lodge_note: %d claim rows -> %s (%s)%s" % (len(rows), rel, "unchanged" if old == new else ("new file" if old is None else "rows replaced"),
                                                     "; %d rows carry seam fields (wired into db\\seams by seam_wire.py)" % n_seam if n_seam else ""))
    for o in objs:
        where = "%s +%s" % (o["function"], o["offset"]) if o["function"] else "data"
        print("  %s  [%s]  %s  %s" % (o["address"], o["mark"], where, o["claim"]))
    if old != new:
        if a.dry:
            print("lodge_note: --dry: claims file not written")
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
            print("changed: " + rel)
    if n_seam:   # rows with seam=TOPIC update db\seams\<TOPIC>.json (seam_wire.py; idempotent, never touches another note's rows)
        spec = importlib.util.spec_from_file_location("seam_wire", os.path.join(HERE, "seam_wire.py"))
        sw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sw)
        sw.wire(objs, root, a.dry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
