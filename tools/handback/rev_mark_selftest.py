#!/usr/bin/env python3
r"""S111 PART 2 REV selftest: the `rev` mark of tools\handback\rev_mark.py, lodge_note.py (writes it) and lodge_lint.py (checks it), in scratch git repos.

  py -I tools\handback\rev_mark_selftest.py            all cases (exit 0 PASS)
  py -I tools\handback\rev_mark_selftest.py --mutate   the planted twins: rev_mark.compute is replaced by "every [C] row gets rev"; the cases that
                                                       name a row that must NOT get rev (review-changed, no Review, ...) then FAIL (exit 1)
Cases: a probe row the review left alone gets rev; the row the review changed, the row it added, an [L] row get none; a Review without a MERGE verdict,
a MERGE verdict without a Review section, a review not yet committed (same result as committed), review and probe in ONE commit, a row another
claims file supersedes; end to end: lodge_note writes the field, lodge_lint passes, drops a hand-removed / hand-added rev, validate refuses rev on an [L] row."""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LODGE = os.path.join(HERE, "lodge_note.py")
LINT = os.path.join(HERE, "lodge_lint.py")
ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
fails = []
TABLE = {"funcs": {"0x82331BC0": 256}, "data": []}


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL: " + msg)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def git(root, *a):
    r = subprocess.run(["git", "-C", root] + list(a), env=ENV, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout


def run(args):
    r = subprocess.run([sys.executable, "-I"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout + r.stderr


R1 = "0x82331BE0 | C | the fixture card waits for the zebra loader | evidence: decomp sub_82331BC0 +0x20"
R2 = "0x82331BE4 | C | the fixture card clears the zebra word | evidence: decomp sub_82331BC0 +0x24"
R2X = "0x82331BE4 | C | the fixture card clears the zebra word only on the second pass | evidence: decomp sub_82331BC0 +0x24"
R3 = "0x82331BE8 | C | the fixture card posts the giraffe signal | evidence: decomp sub_82331BC0 +0x28"
RL = "0x82331BEC | L | the fixture card probably reads the giraffe | evidence: decomp sub_82331BC0 +0x2C"


def note(rows, review=None):
    t = "# S999 FIX: fixture note\n\n## Answer\n\nFixture.\n\n"
    if review is not None:
        t += "## Review (adversary)\n\n" + review + "\n\n"
    return t + "```claims\n" + "\n".join(rows) + "\n```\n"


def put(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(text.encode("utf-8"))
    return p


def commit(root, msg):
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", msg)


def computed(rm, cl, root, rel, text):
    return rm.compute(root, rel, text, cl.parse_claims(text), cl)


def cases(rm, cl, tmp):
    root = os.path.join(tmp, "repo")
    os.makedirs(root)
    git(root, "init", "-q")
    ok_review = "Held: row 1. Changed: row 2 reworded, row 3 added.\n\nVerdict: MERGE AFTER MY FIXES\n"
    v1 = note([R1, R2, RL])
    v2 = note([R1, R2X, RL, R3], ok_review)

    a = "docs/research/s999/A.md"
    put(root, a, v1)
    commit(root, "A probe")
    put(root, a, v2)
    check(computed(rm, cl, root, a, v2) == [rm.rev_value(a), None, None, None],
          "uncommitted review: row 1 gets rev; the changed row 2, the [L] row and the added row 3 get none: %r" % computed(rm, cl, root, a, v2))
    commit(root, "A review")
    check(computed(rm, cl, root, a, v2) == [rm.rev_value(a), None, None, None], "committed review: the same marks")
    v3 = v2.replace(R1, R1 + " | site sub_82331BC0+0x20")     # lane B types the row later: its claim text is unchanged
    check(computed(rm, cl, root, a, v3)[0] == rm.rev_value(a), "a row retyped after the review keeps rev (same address / claim / evidence)")

    b = "docs/research/s999/B.md"           # a Review with no MERGE verdict
    put(root, b, v1)
    commit(root, "B probe")
    vb = note([R1, R2, RL], "Held: all. Verdict REWORK.\n")
    check(computed(rm, cl, root, b, vb) == [None, None, None], "a review without a MERGE verdict: no rev")

    c = "docs/research/s999/C.md"           # a verdict line but no Review section
    vc = note([R1, R2, RL]) + "\nVerdict: MERGE\n"
    put(root, c, note([R1, R2, RL]))
    commit(root, "C probe")
    check(computed(rm, cl, root, c, vc) == [None, None, None], "a note with no Review section: no rev")

    d = "docs/research/s999/D.md"           # probe and review in one commit
    put(root, d, v2)
    commit(root, "D probe and review")
    check(computed(rm, cl, root, d, v2) == [None, None, None, None], "review landed with the probe (no hand-back version): no rev")

    e = "docs/research/s999/E.md"           # another claims file supersedes a row of E
    put(root, e, v1)
    commit(root, "E probe")
    put(root, e, v2)
    commit(root, "E review")
    check(computed(rm, cl, root, e, v2)[0] == rm.rev_value(a.replace("A.md", "E.md")), "E before the superseding row: row 1 has rev")
    put(root, "db/claims/s999_Z.json", json.dumps([{"note": "docs/research/s999/Z.md", "supersedes": "docs/research/s999/E.md +0x20"}]))
    check(computed(rm, cl, root, e, v2) == [None, None, None, None], "a row elsewhere supersedes the note: no rev")
    os.remove(os.path.join(root, "db", "claims", "s999_Z.json"))

    # REVIEW TWINS (S111 INDEX-REV review). The verdict is the exact line the reviewer definition asks for, the last one deciding.
    g = "docs/research/s999/G.md"
    put(root, g, v1)
    commit(root, "G probe")
    for why, review in (("a prose verdict (no exact `Verdict: MERGE` line)", "Held: row 1.\n\nVerdict MERGE after comment fixes.\n"),
                        ("a quoted verdict inside a sentence", "The rule wants a `Verdict: MERGE` line; this review gives none.\n"),
                        ("a later BACK TO DESIGN after an earlier MERGE", "Verdict: MERGE\n\nRe-review:\n\nVerdict: BACK TO DESIGN\n")):
        got = computed(rm, cl, root, g, note([R1, R2X, RL, R3], review))
        check(got == [None, None, None, None], "%s: no rev: %r" % (why, got))
    check(computed(rm, cl, root, g, note([R1, R2X, RL, R3], "Verdict: BACK TO DESIGN\n\nVerdict: MERGE\n"))[0] == rm.rev_value(g),
          "a BACK TO DESIGN then a MERGE re-review: the last verdict decides (row 1 gets rev)")

    h = "docs/research/s999/H.md"           # the reviewer commits its claims fix BEFORE the commit that writes the Review section
    put(root, h, v1)
    commit(root, "H probe")
    put(root, h, note([R1, R2X, RL]))
    commit(root, "H review fix: row 2 corrected in the claims block (no Review section yet)")
    put(root, h, v2)
    commit(root, "H review section")
    got = computed(rm, cl, root, h, v2)
    check(got == [rm.rev_value(h), None, None, None], "a review fix committed before the Review section: the row it changed gets no rev: %r" % (got,))

    k = "docs/research/s999/K.md"           # the probe's version committed on the main line, the review on a branch cut before it, merged as the branch's
    base = git(root, "rev-parse", "HEAD").strip()
    main = git(root, "rev-parse", "--abbrev-ref", "HEAD").strip()
    put(root, k, v1)
    commit(root, "K probe (main line)")
    git(root, "checkout", "-q", "-b", "kside", base)
    put(root, k, note([R1, R2X, RL]))
    commit(root, "K review fix on the branch (the note copied in, row 2 corrected)")
    put(root, k, v2)
    commit(root, "K review section on the branch")
    git(root, "checkout", "-q", main)
    subprocess.run(["git", "-C", root, "merge", "-q", "--no-edit", "kside"], env=ENV, capture_output=True)
    put(root, k, v2)
    commit(root, "K merge: the branch's version")
    got = computed(rm, cl, root, k, v2)
    check(got == [rm.rev_value(k), None, None, None], "the probe's version on the merged side line still counts: the branch-corrected row gets no rev: %r" % (got,))


def end_to_end(tmp):
    root = os.path.join(tmp, "e2e")
    os.makedirs(root)
    git(root, "init", "-q")
    table = os.path.join(tmp, "table.json")
    with open(table, "w") as f:
        json.dump(TABLE, f)
    rel = "docs/research/s999/A.md"
    put(root, rel, note([R1, R2, RL]))
    commit(root, "A probe")
    np = put(root, rel, note([R1, R2X, RL, R3], "Held: row 1.\n\nVerdict: MERGE\n"))
    rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
    check(rc == 0, "lodge_note exits 0 on the reviewed note: " + out[-300:])
    cpath = os.path.join(root, "db", "claims", "s999_A.json")
    objs = json.load(open(cpath, encoding="utf-8"))
    check([o.get("rev") for o in objs] == ["docs/research/s999/A.md #Review", None, None, None], "the lodged file marks row 1 only: %r" % [o.get("rev") for o in objs])
    commit(root, "A review and lodge")
    rc, out = run([LINT, "--root", root])
    check(rc == 0, "lodge_lint passes on the lodged note: " + out[-300:])
    objs[0].pop("rev")
    open(cpath, "w", encoding="utf-8", newline="\n").write(json.dumps(objs, indent=2, ensure_ascii=False) + "\n")
    rc, out = run([LINT, "--root", root])
    check(rc == 1 and "rev differs" in out, "lodge_lint fails when the rev of row 1 was removed by hand: " + out[-300:])
    objs[0]["rev"] = "docs/research/s999/A.md #Review"
    objs[1]["rev"] = "docs/research/s999/A.md #Review"
    open(cpath, "w", encoding="utf-8", newline="\n").write(json.dumps(objs, indent=2, ensure_ascii=False) + "\n")
    rc, out = run([LINT, "--root", root])
    check(rc == 1 and "rev differs" in out, "lodge_lint fails on a hand-added rev (row 2, changed by the review): " + out[-300:])
    objs[1].pop("rev")
    open(cpath, "w", encoding="utf-8", newline="\n").write(json.dumps(objs, indent=2, ensure_ascii=False) + "\n")
    brel = "docs/research/s999/B.md"     # a note with no verdict whose claims file carries a hand-added rev: the --apply pass removes it
    bp = put(root, brel, note([R1, R2, RL]))
    rc, out = run([LODGE, bp, "--root", root, "--resolver-table", table])
    bpath = os.path.join(root, "db", "claims", "s999_B.json")
    bobjs = json.load(open(bpath, encoding="utf-8"))
    bobjs[0]["rev"] = brel + " #Review"
    open(bpath, "w", encoding="utf-8", newline="\n").write(json.dumps(bobjs, indent=2, ensure_ascii=False) + "\n")
    commit(root, "B probe, rev added by hand")
    rc, out = run([os.path.join(HERE, "rev_mark.py"), "--root", root, "--apply"])
    got = [o.get("rev") for o in json.load(open(bpath, encoding="utf-8"))]
    check(rc == 0 and got == [None, None, None], "rev_mark --apply removes a rev from a note with no verdict: %r %s" % (got, out[-200:]))
    got = [o.get("rev") for o in json.load(open(cpath, encoding="utf-8"))]
    check(got == ["docs/research/s999/A.md #Review", None, None, None], "rev_mark --apply keeps the reviewed note's marks: %r" % (got,))
    val = load("claims").load_validate()
    rep = val.Report()
    bad = [dict(objs[2], rev="x")]
    val.check_claim_rows(rep, "x.json", bad)
    check(any("rev" in m for _, _, m in rep.errors), "validate refuses rev on an [L] row")


def main():
    rm, cl = load("rev_mark"), load("claims")
    if "--mutate" in sys.argv:
        real = rm.compute
        rm.compute = lambda root, rel, text, rows, cl=None: [rm.rev_value(rel) if r["mark"] == "C" else None for r in rows]
        print("mutated: every [C] row of every note gets rev (the cases that name a row without rev must FAIL)")
    with tempfile.TemporaryDirectory() as tmp:
        cases(rm, cl, tmp)
        if "--mutate" not in sys.argv:
            end_to_end(tmp)
    print("rev_mark_selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
