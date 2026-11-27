#!/usr/bin/env python3
r"""S111 INDEX_PLAN lane B (D4): lodging as a lint. A committed probe note whose ```claims block differs from its claims file fails.

  py -I tools\handback\lodge_lint.py [--root DIR]

Every note under docs\research\ that git tracks (git ls-files, at --root) and that carries a ```claims block must have its claims file
db\claims\<sNNN>_<NOTE>.json (tools\handback\lodge_note.py: claims_rel) and the file's rows must say what the block says: address, mark, claim,
evidence, seam / ours / verdict / system and the TYPE column (kind, key, name; a site row's `site`). The comparison reads no code index (the
function / offset columns are what lodge_note.py resolves; a block that changed shows in the fields below). A note with no block, or an empty one,
needs no file. A block that does not parse fails too (it could never be lodged).

THE NOTE'S BLOCK IS THE ONLY SOURCE (main session decision, S111 lane B review; rule 6): db\claims and the claims-driven db\seams rows are built
from it by lodging and never edited by hand. A claims file that differs from its committed note is therefore a hand edit (or a note edited and
not lodged): the line says so, prints both sides' last commit and which is newer, and the fix is to put the change in the note's block
(tools\handback\pull_claims.py NOTE copies the claims file's rows in, when the file holds the correction) and lodge.

The `rev` field of each row must equal what tools\handback\rev_mark.py computes from git + the note (a reviewed [C] row: see there); a stale one
is fixed by `py -I tools\handback\rev_mark.py --apply` or by lodging the note again.

Per note it prints the problem and the exact command that fixes it:
  FAIL docs\research\s111\X.md: not lodged (no db\claims\s111_X.json)
       py -I tools\handback\lodge_note.py docs\research\s111\X.md
  FAIL docs\research\s111\Y.md: db\claims edited by hand: db\claims\s111_Y.json differs from the block (row 3 ...): put the change in the note's block and lodge
       note:        <hash> <date> <subject>
       claims file: <hash> <date> <subject>   (newer: the claims file)
       py -I tools\handback\pull_claims.py docs\research\s111\Y.md   (when the claims file holds the correction: copies its rows into the block)
       py -I tools\handback\lodge_note.py docs\research\s111\Y.md
Exit 0 = every note lodged and equal; 1 = a note differs (the lines above); 2 = could not run (no git / no root). Stdlib only; 0 game runs."""
import argparse
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIELDS = ("mark", "claim", "evidence", "seam", "ours", "verdict", "system", "kind", "key", "name", "scope")


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def expected(r, rev=None):
    """a parsed row -> the comparable dict of what the lodged object must hold (rev: what tools\\handback\\rev_mark.py computes for it)."""
    d = {"address": "0x%08X" % r["addr"]}
    for k in FIELDS:
        d[k] = r.get(k) or None
    d["site"] = ("0x%08X" % r["site"]) if r.get("kind") == "site" else None
    d["rev"] = rev or None
    return d


def lodged(o):
    d = {"address": o.get("address")}
    for k in FIELDS + ("site", "rev"):
        d[k] = o.get(k) or None
    return d


def differences(rows, objs, revs=None):
    """-> [str]: why the file does not hold the block's rows (empty = equal)."""
    if len(rows) != len(objs):
        return ["the block has %d rows, the claims file %d" % (len(rows), len(objs))]
    out = []
    for i, (r, o) in enumerate(zip(rows, objs), 1):
        e, g = expected(r, revs[i - 1] if revs else None), lodged(o)
        bad = [k for k in e if e[k] != g[k]]
        if bad:
            out.append("row %d (%s): %s differ%s" % (i, e["address"], ", ".join(bad), "s" if len(bad) == 1 else ""))
    return out


def tracked_notes(root):
    r = subprocess.run(["git", "-C", root, "ls-files", "--", "docs/research"], capture_output=True)
    if r.returncode:
        raise RuntimeError("git ls-files failed: " + r.stderr.decode("utf-8", "replace").strip()[:200])
    return [x for x in r.stdout.decode("utf-8", "replace").splitlines() if x.endswith(".md")]


def last_commit(root, rel):
    """-> (commit hash or None, 'hash date subject' + ' + uncommitted changes' when the tree differs, dirty) for one path"""
    r = subprocess.run(["git", "-C", root, "log", "-1", "--format=%H %h %cs %s", "--", rel], capture_output=True)
    t = r.stdout.decode("utf-8", "replace").strip()
    dirty = bool(subprocess.run(["git", "-C", root, "status", "--porcelain", "--", rel], capture_output=True).stdout.strip())
    h, text = (t.split()[0], t.split(None, 1)[1][:120]) if t else (None, "no commit")
    return h, text + (" + uncommitted changes" if dirty else ""), dirty


def newer_side(root, note, claims):
    """which of the two paths was changed last: uncommitted beats committed, then git ancestry (not the clock: one second holds many commits)"""
    (hn, _, dn), (hc, _, dc) = note, claims
    if dn != dc:
        return "the note" if dn else "the claims file"
    if hn == hc or not hn or not hc:
        return "neither (the same commit)" if hn == hc else ("the note" if hn else "the claims file")
    anc = lambda a, b: subprocess.run(["git", "-C", root, "merge-base", "--is-ancestor", a, b], capture_output=True).returncode == 0
    return "the claims file" if anc(hn, hc) else "the note" if anc(hc, hn) else "neither (parallel branches)"


def lint(root, out=print):
    """-> (notes with a block, [(note rel, reason, [lines to print under it: the commits, then the commands])])."""
    cl, ln, rv = load("claims"), load("lodge_note"), load("rev_mark")
    nblock, bad = 0, []
    for rel in tracked_notes(root):
        full = os.path.join(root, *rel.split("/"))
        try:
            with open(full, "rb") as f:
                text = f.read().decode("utf-8", "replace")
        except OSError:
            continue    # tracked but deleted in the tree: git status shows it; not a lodging fault
        if "```claims" not in text:
            continue
        cmd = "py -I tools\\handback\\lodge_note.py " + rel.replace("/", "\\")
        try:
            rows = cl.parse_claims(text)
        except cl.ClaimsError as e:
            nblock += 1
            bad.append((rel, "the claims block does not parse: %s" % e, ["py -I tools\\handback\\note_sites.py --check " + rel.replace("/", "\\")]))
            continue
        if not rows:
            continue
        nblock += 1
        cpath = os.path.join(root, *ln.claims_rel(rel).split("/"))
        if not os.path.isfile(cpath):
            bad.append((rel, "not lodged (no %s)" % ln.claims_rel(rel).replace("/", "\\"), [cmd]))
            continue
        try:
            with open(cpath, encoding="utf-8") as f:
                objs = json.load(f)
        except (OSError, ValueError) as e:
            bad.append((rel, "%s does not parse: %s" % (ln.claims_rel(rel), e), [cmd]))
            continue
        why = differences(rows, objs if isinstance(objs, list) else [], rv.compute(root, rel, text, rows, cl))
        if why:
            crel = ln.claims_rel(rel)
            n_, c_ = last_commit(root, rel), last_commit(root, crel)
            sn, sc, newer = n_[1], c_[1], newer_side(root, n_, c_)
            bad.append((rel, "db\\claims edited by hand: %s differs from the block (%s): put the change in the note's block and lodge"
                        % (crel.replace("/", "\\"), "; ".join(why[:3]) + (" ..." if len(why) > 3 else "")),
                        ["note:        " + sn, "claims file: " + sc + "   (newer: %s)" % newer,
                         "py -I tools\\handback\\pull_claims.py %s   (when the claims file holds the correction: copies its rows into the block)" % rel.replace("/", "\\"),
                         cmd]))
    return nblock, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=".")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    try:
        nblock, bad = lint(root)
    except (RuntimeError, OSError) as e:
        print("lodge_lint: could not run: %s" % e)
        return 2
    for rel, why, lines in bad:
        print("FAIL %s: %s" % (rel.replace("/", "\\"), why))
        for x in lines:
            print("     " + x)
    ab_fails, ab_warn = load("abslint").lint(root)   # ABSLINT: absolute words in lodged claims ([L] / [U] = FAIL)
    for x in ab_fails:
        print(x)
    print("lodge_lint: abslint %d FAIL, %d [C] WARN (py -I tools\\handback\\abslint.py --sweep lists them)" % (len(ab_fails), ab_warn))
    print("lodge_lint: %d notes with a claims block, %d not lodged or different" % (nblock, len(bad)))
    return 1 if bad or ab_fails else 0


if __name__ == "__main__":
    sys.exit(main())
