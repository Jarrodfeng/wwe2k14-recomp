#!/usr/bin/env python3
r"""S111 INDEX_PLAN lane B, the one-off migration: type the claims rows that cite their OWN site in the evidence text.

  py -I tools\handback\type_sites.py [--root DIR] [--dry] [--resolver-table FILE] [NOTE ...]

A row whose address is inside a function sub_X and whose evidence says `... sub_X +0x1D4 ...` (or `sub_X+0x1D4`, or a range `+0x134..+0x14C`: its
first offset) gets the TYPE column `site sub_X+0x1D4`, inserted right after the evidence field. Only what the text states is parsed: a row that
cites another function's offset, a bare address range, or no offset is left alone; no field, role or enum is ever guessed. A cited offset that is
not a multiple of 4, or lies outside the function, is skipped and listed (the evidence is then not a site). A row that already has a TYPE column
is left alone, so a second run changes nothing. NOTES default to every note under docs\research\ that git tracks and that has a ```claims block.
The notes are rewritten in place (line endings kept); then run `py -I tools\handback\lodge_note.py NOTE` for each changed note (the printed
lines name them) or the lodge lint (tools\handback\lodge_lint.py) tells which are behind.
Exit 0 always when it ran (the summary says what changed); 2 = could not run. Stdlib only; 0 game runs."""
import argparse
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FENCE_OPEN = re.compile(r"^```claims\s*$")
FENCE_ANY = re.compile(r"^`{3,}\s*[A-Za-z]*\s*$")


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def own_site(evidence, func):
    """-> the first offset the evidence cites for sub_<func> (`sub_X +0x1D4`, `sub_X+0x1D4`, `sub_X +0x134..+0x14C`), else None. Only the 0x
    form, and a bare `+A8` / `+3A4` (a hex letter in it: only hex reads so). A bare all-digit `+10` / `+390` is ambiguous (hex or decimal) and
    is not guessed (the row stays untyped)."""
    m = re.search(r"sub_%08X\s*\+\s*(0x[0-9A-Fa-f]+|[0-9A-Fa-f]*[A-Fa-f][0-9A-Fa-f]*)(?![\w])" % func, evidence, re.I)
    return int(m.group(1), 16) if m else None


def migrate_text(text, cl, resolver):
    """-> (new text, typed [(line, key)], skipped [(line, why)])."""
    lines = text.split("\n")
    typed, skipped, in_block = [], [], False
    for i, raw in enumerate(lines):
        body = raw.rstrip("\r")
        if not in_block:
            in_block = bool(FENCE_OPEN.match(body))
            continue
        if FENCE_ANY.match(body):
            break
        ln = body.strip()
        if not ln or ln.startswith("#"):
            continue
        try:
            row = cl._row(ln, i + 1)
        except cl.ClaimsError:
            continue   # a malformed row is note_sites.py --check's to report
        if row["kind"]:
            continue
        r = resolver.resolve(row["addr"])
        if r is None or r[0] != "func":
            continue
        off = own_site(row["evidence"], r[1])
        if off is None:
            continue
        if off % 4 or resolver.resolve(r[1] + off) != ("func", r[1]):
            skipped.append((i + 1, "sub_%08X +0x%X is not an instruction of that function" % (r[1], off)))
            continue
        key = "sub_%08X+0x%X" % (r[1], off)
        f = ln.split(" | ")
        new = " | ".join(f[:4] + ["site " + key] + f[4:])
        lines[i] = new + ("\r" if raw.endswith("\r") else "")
        typed.append((i + 1, key))
    return "\n".join(lines), typed, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("notes", nargs="*")
    ap.add_argument("--root", default=".")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--resolver-table", default=None, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    cl, ll = load("claims"), load("lodge_lint")
    try:
        if a.resolver_table:
            with open(a.resolver_table, encoding="utf-8") as f:
                t = json.load(f)
            resolver = cl.TableResolver(t.get("funcs", {}), t.get("data", []), t.get("facts"))
        else:
            resolver = cl.QResolver(root)
        notes = [os.path.relpath(os.path.abspath(n), root).replace("\\", "/") for n in a.notes] or ll.tracked_notes(root)
    except Exception as e:   # noqa: BLE001
        print("type_sites: could not run: %s: %s" % (type(e).__name__, e))
        return 2
    total, changed, skipped_all = 0, [], 0
    for rel in notes:
        path = os.path.join(root, *rel.split("/"))
        try:
            with open(path, "rb") as f:
                text = f.read().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "```claims" not in text:
            continue
        new, typed, skipped = migrate_text(text, cl, resolver)
        for ln, why in skipped:
            print("  skipped %s:%d: %s" % (rel, ln, why))
        skipped_all += len(skipped)
        if typed:
            total += len(typed)
            changed.append(rel)
            print("%s: %d rows typed as site" % (rel, len(typed)))
            if not a.dry:
                with open(path, "wb") as f:
                    f.write(new.encode("utf-8"))
    print("type_sites: %d rows typed in %d notes, %d skipped%s" % (total, len(changed), skipped_all, " (--dry: nothing written)" if a.dry else ""))
    for rel in changed:
        print("changed: " + rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
