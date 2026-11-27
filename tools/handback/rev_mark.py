#!/usr/bin/env python3
r"""S111 PART 2 REV: the `rev` mark on lodged [C] rows (DECISIONS 2026-11-26 20:3x: a REVIEWED background fact printed as `[C rev]` may be taken
as read by a reviewer; tools\q\qfactcard.py reads `rev`). The mark is COMPUTED from git + the note, never written by hand; the note's ```claims
block stays the one source (lodge_note.py writes the field, lodge_lint.py checks it against this module).

  py -I tools\handback\rev_mark.py [--apply] [--root DIR]     the one-time / re-do pass over every tracked note: lists the rows that get `rev`;
                                                              --apply rewrites the field in their db\claims files (nothing else changes)

THE RULE (INDEX-A review, decision (b)). A row of a note's claims file gets  "rev": "<note path> #Review"  ONLY when
  (1) the note has a `## Review` section (any heading level) and, at or after it, a line that is exactly `Verdict: MERGE` or
      `Verdict: MERGE AFTER MY FIXES` (the reviewer definition's line; the LAST `Verdict: ...` line decides, so a later `Verdict: BACK TO DESIGN`
      takes the mark away; a verdict in prose or quoted inside a sentence marks nothing),
  (2) the row is mark [C] and its raw claims line is byte-identical in EVERY committed version of the note that has no Review heading and in
      the version that holds the review (a row the review added or changed gets NO rev, nor does one whose seam / ours / verdict / system /
      TYPE text the review touched; a review fix committed before the Review section is a version without the heading, so it counts too),
  (3) no row of another db\claims file `supersedes` the note (qfactcard's `supersedes: NOTE +0xOFF` column; conservative: then none of the note's
      rows gets rev). The review's own rows are in the same block, so (2) already holds them.
The versions come from git (`git log --full-history -- NOTE` at --root: every line of history, so a probe version on the side of a merge that
took the branch's text still counts): the review version = the oldest committed one that holds the `Review` heading (else the note's text being
lodged, not yet committed); the pre-review versions = every committed one without the heading. No git, no pre-review version (note and review
landed in one commit), or no Review section: no rev anywhere (where the two cannot be told apart, no rev). A row later retyped
(TYPE column added by lane B) still counts when its address / mark / claim / evidence equal an eligible row's. Stdlib only; 0 game runs."""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REVIEW_RE = re.compile(r"(?m)^#{1,6}[ \t]+Review\b")
VERDICT_RE = re.compile(r"(?m)^Verdict: (MERGE AFTER MY FIXES|MERGE|BACK TO DESIGN)[ \t]*\r?$")   # the reviewer definition's exact line


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def rev_value(note_rel):
    return note_rel.replace("\\", "/") + " #Review"


def verdict_ok(text):
    """the note has a Review section and, at or after its heading, `Verdict: ...` lines whose LAST one is MERGE or MERGE AFTER MY FIXES."""
    m = REVIEW_RE.search(text)
    said = VERDICT_RE.findall(text[m.start():]) if m else []
    return bool(said) and said[-1] != "BACK TO DESIGN"


def versions(root, rel):
    """-> [(commit, text)] of the note, oldest first (every line of git history at root: --full-history, so a merge that took one side's
    text does not hide the other side's versions); None when git cannot say. One `git cat-file --batch` reads every version."""
    r = subprocess.run(["git", "-C", root, "log", "--full-history", "--topo-order", "--reverse", "--format=%H", "--", rel], capture_output=True)
    if r.returncode:
        return None
    hashes = r.stdout.decode("utf-8", "replace").split()
    if not hashes:
        return []
    s = subprocess.run(["git", "-C", root, "cat-file", "--batch"], input="".join("%s:%s\n" % (h, rel) for h in hashes).encode("utf-8"),
                       capture_output=True)
    if s.returncode:
        return None
    out, buf, pos = [], s.stdout, 0
    for h in hashes:
        nl = buf.find(b"\n", pos)
        if nl < 0:
            return None
        head = buf[pos:nl].split()
        pos = nl + 1
        if len(head) != 3 or head[1] != b"blob":   # `<rev> missing`: a commit that deleted the note shows nothing: skipped
            continue
        size = int(head[2])
        out.append((h, buf[pos:pos + size].decode("utf-8", "replace")))
        pos += size + 1
    return out


def key_of(r):
    return (r["addr"], r["mark"], r["claim"], r["evidence"])


def rawmap(cl, text):
    try:
        return {r["raw"]: key_of(r) for r in cl.parse_claims(text) if r["mark"] == "C"}
    except cl.ClaimsError:
        return {}


def superseders(root, rel):
    """-> [supersedes text] of rows in OTHER claims files that name this note (qfactcard's `supersedes: NOTE +0xOFF`)."""
    stem = os.path.splitext(os.path.basename(rel))[0].lower()
    out = []
    d = os.path.join(root, "db", "claims")
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return out
    for n in names:
        if not n.endswith(".json"):
            continue
        try:
            with open(os.path.join(d, n), encoding="utf-8") as f:
                objs = json.load(f)
        except (OSError, ValueError):
            continue
        for o in objs if isinstance(objs, list) else []:
            sup = str(o.get("supersedes") or "").split() if isinstance(o, dict) else []
            if sup and os.path.splitext(os.path.basename(sup[0]))[0].lower() == stem and \
                    os.path.splitext(os.path.basename(str(o.get("note") or "")))[0].lower() != stem:
                out.append(" ".join(sup))
    return out


def compute(root, rel, text, rows, cl=None):
    """-> [rev string or None] aligned with `rows` (the parsed block of `text`, the note being lodged)."""
    cl = cl or load("claims")
    none = [None] * len(rows)
    if not any(r["mark"] == "C" for r in rows) or not verdict_ok(text):
        return none
    vers = versions(root, rel)
    if vers is None:
        return none
    review = next((t for _, t in vers if REVIEW_RE.search(t)), text)
    pre = [rawmap(cl, t) for _, t in vers if not REVIEW_RE.search(t)]   # every committed version without the heading (probe drafts, review fixes)
    if not pre:
        return none
    ok = {key for raw, key in rawmap(cl, review).items() if all(raw in hb for hb in pre)}
    if superseders(root, rel):   # conservative: a row elsewhere supersedes (part of) this note: no rev on any of its rows (the offset is not known here)
        return none
    return [rev_value(rel) if r["mark"] == "C" and key_of(r) in ok else None for r in rows]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=".")
    ap.add_argument("--apply", action="store_true", help="rewrite the rev fields of the db\\claims files")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    cl, ln, ll = load("claims"), load("lodge_note"), load("lodge_lint")
    total = nfiles = 0
    for rel in ll.tracked_notes(root):
        try:
            with open(os.path.join(root, *rel.split("/")), "rb") as f:
                text = f.read().decode("utf-8", "replace")
            rows = cl.parse_claims(text)
        except (OSError, cl.ClaimsError):
            continue
        if not rows:   # a note with no verdict is still rewritten: compute gives it no rev, so a stale or hand-added one is removed
            continue
        revs = compute(root, rel, text, rows, cl)
        cpath = os.path.join(root, *ln.claims_rel(rel).split("/"))
        try:
            with open(cpath, encoding="utf-8") as f:
                objs = json.load(f)
        except (OSError, ValueError):
            continue
        if len(objs) != len(rows):
            print("rev_mark: %s: the claims file has %d rows, the block %d: lodge it first" % (rel, len(objs), len(rows)))
            continue
        n = sum(1 for v in revs if v)
        new = []
        for o, v in zip(objs, revs):
            o = {k: x for k, x in o.items() if k != "rev"}
            if v:
                o["rev"] = v
            new.append(o)
        total += n
        changed = new != objs or [o.get("rev") for o in objs] != [o.get("rev") for o in new]
        if n or changed:
            print("rev_mark: %s: %d of %d rows get rev (%d [C])%s" % (rel, n, len(rows), sum(1 for r in rows if r["mark"] == "C"),
                                                                     ": the claims file changes" if changed else ""))
        if changed:
            nfiles += 1
            if a.apply:
                with open(cpath, "w", encoding="utf-8", newline="\n") as f:
                    f.write(ln.render(new))
    print("rev_mark: %d rows marked rev; %d claims files %s" % (total, nfiles, "rewritten" if a.apply else "would change (--apply rewrites them)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
