#!/usr/bin/env python3
r"""S111 LODGE selftest: claims.py + lodge_note.py + validate.py's claims check + `q fact` / `q db`, in a temp fixture tree (no game, no
code index: a fixed address table; 0 runs).

  py -I tools\handback\lodge_note_selftest.py

Cases: a fixture note with three claim rows (one inside a function, one at a function start, one a data address) writes ONE file
db\claims\s999_FIX.json with function / offset filled; the existing db files are byte-equal afterwards (a lodge never edits them, never
makes an entry); a second lodge changes nothing; an edited note REPLACES its rows (a dropped row is gone); a note without a block leaves
the file as it is; validate.py passes the lodged file and fails planted bad claims files (code text, a bad mark, a missing field) while
ignoring them as function entries; `q fact` finds the claim (not before the lodge); q.claims_for answers for a data address, a function
start and through `function` for a claim inside it; refusals write nothing: an address that does not resolve, game code in the claim or
the evidence (nine forms, among them the ones the first check let through: `bl sub_`, `cmplwi cr6,`, `mtctr r11`, `rlwinm.`, `bctrl`), a bad
mark, missing evidence, two blocks; plain English with words like "adds" / "branches" passes; note_sites.py --check reports a bad block
and stays silent for a note without one."""
import argparse
import contextlib
import glob
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LODGE = os.path.join(HERE, "lodge_note.py")
SITES = os.path.join(HERE, "note_sites.py")
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL: " + msg)


def run(args):
    r = subprocess.run([sys.executable, "-I"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout + r.stderr


def snap(root):
    return {p: open(p, "rb").read() for p in sorted(glob.glob(os.path.join(root, "db", "**", "*.json"), recursive=True))}


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


FIX_A = """[
  {
    "address": "0x82331BC0",
    "name": "loading_card_release",
    "summary": "Releases the loading card.",
    "evidence": [
      "old evidence item"
    ],
    "confidence": "medium",
    "system": "ui",
    "source": "fixture"
  }
]
"""
FIX_B = ('[{"address": "0x82340000", "name": "fixture_compact", "summary": "A compact one-line entry.", "evidence": ["one"], '
         '"confidence": "low", "system": "other", "source": "fixture"}]\n')
TABLE = {"funcs": {"0x82331BC0": 256, "0x82340000": 128}, "data": ["0x836F38D0"]}


def note(rows, extra=""):
    return ("# S999 FIX: fixture note\n\n## Sites the build will touch\n\nNo sites: fixture\n\n" + extra +
            "```claims\n" + "\n".join(rows) + "\n```\n")


GOOD = [
    "0x82331BE0 | C | the fixture card waits for the zebra loader before it closes | evidence: decomp sub_82331BC0 +0x20",
    "sub_82340000 | L | the compact fixture function posts the zebra signal | evidence: callers listing",
    "0x836F38D0 | C | the fixture zebra table holds one word per slot | evidence: field read in inputs_contrib.h:146 | system=match",
]
# game code the claim check must refuse (the first five slipped through the first version's check, which wanted a comma after a register)
CODE = ["lwz r3,0x10(r31)", "addi r3,r31,8", "bl sub_82331BC0", "cmplwi cr6,r11,9", "mtctr r11", "rlwinm. r11,r3,0,31,31", "bctrl",
        "stw r4 -> [S+1176]", "ctx.r3.u32 = 0"]
ENGLISH = "the fixture adds one to the zebra count, then branches back to the loader and returns"


def seam_cases(tmp, table):
    """SEAMWIRE: claims rows with seam=TOPIC update db/seams/<TOPIC>.json (tools/handback/seam_wire.py), in their own fixture tree."""
    root = os.path.join(tmp, "repo2")
    os.makedirs(os.path.join(root, "docs", "research", "s999"))
    os.makedirs(os.path.join(root, "src"))
    with open(os.path.join(root, "src", "z.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// zebra\nint zebra_wait = 1;\nint zebra_post = 2;\n\nint zebra_end = 3;\n")
    sp = os.path.join(root, "db", "seams", "zebra.json")
    yp = os.path.join(root, "db", "seams", "yak.json")
    n1 = os.path.join(root, "docs", "research", "s999", "ONE.md")
    n2 = os.path.join(root, "docs", "research", "s999", "TWO.md")
    r1 = "0x82331BE0 | C | the zebra loader waits for the card | evidence: decomp +0x20 | seam=zebra ours=src/z.h:2 verdict=match"

    def lodge(path, rows):
        with open(path, "w", encoding="utf-8") as f:
            f.write(note(rows))
        return run([LODGE, path, "--root", root, "--resolver-table", table])

    def entry():
        with open(sp, encoding="utf-8") as f:
            return json.load(f)

    # a seam row lands in a topic with no entry yet (game address + ours line: the entry is created)
    rc, out = lodge(n1, [r1])
    check(rc == 0 and os.path.exists(sp), "a seam row creates the entry for a new topic (rc %d)\n%s" % (rc, out))
    e = entry() if os.path.exists(sp) else {"game": [], "ours": []}
    check([g["addr"] for g in e["game"]] == ["0x82331BE0"] and e["game"][0]["what"] == "the zebra loader waits for the card"
          and e["game"][0]["source"] == "docs/research/s999/ONE.md" and e["game"][0]["mark"] == "C", "the game row holds addr / what / source / mark: %s" % e["game"])
    check([(o["path"], o["anchor"], o["verdict"], o["why"]) for o in e["ours"]]
          == [("src/z.h", "int zebra_wait = 1;", "match", "the zebra loader waits for the card")], "the ours row holds the anchor read from the file: %s" % e["ours"])
    check(all("line" not in o and "ctx" not in o for o in e["ours"]), "S111 SEAMNOLINE: the ours row stores no line number: %s" % e["ours"])
    # lodging twice adds nothing
    b0 = open(sp, "rb").read()
    rc, out = lodge(n1, [r1])
    check(rc == 0 and open(sp, "rb").read() == b0 and "game +0 ~0 =1, ours +0 ~0 =1" in out, "lodging the same note twice adds nothing (rc %d)\n%s" % (rc, out))
    # a second row on the existing entry: added; a stale ours line is refused and reported, its game row still lands
    r2 = "0x82340000 | L | the zebra signal is posted | evidence: callers | seam=zebra ours=src/z.h:3 verdict=replace"
    r3 = "sub_82331BC0 | U | the zebra card start | evidence: listing | seam=zebra ours=src/z.h:99 verdict=match"
    rc, out = lodge(n1, [r1, r2, r3])
    e = entry()
    check(rc == 0 and len(e["ours"]) == 2 and e["ours"][1]["verdict"] == "replace" and e["ours"][1]["anchor"] == "int zebra_post = 2;",
          "a second seam row is added to the entry (rc %d)\n%s" % (rc, out))
    check("REFUSED" in out and "src/z.h:99" in out and len(e["ours"]) == 2,
          "a stale ours anchor (line past the end of the file) is refused and reported, not written\n" + out)
    check([g["addr"] for g in e["game"]] == ["0x82331BE0", "0x82340000", "0x82331BC0"], "the game row of a refused ours line still lands: %s" % e["game"])
    # a blank line is no anchor either
    rc, out = lodge(n1, [r1.replace("z.h:2", "z.h:4")])
    check("REFUSED" in out and len(entry()["ours"]) == 2, "an ours line that is blank is refused\n" + out)
    # an edited note updates its own game row; another note's row is never touched
    rc, out = lodge(n1, [r1.replace("waits for the card", "waits for the zebra card")])
    check(entry()["game"][0]["what"] == "the zebra loader waits for the zebra card" and "~1" in out, "an edited note updates its game row\n" + out)
    rc, out = lodge(n2, ["0x82331BE0 | L | another note says something else | evidence: x | seam=zebra ours=src/z.h:2 verdict=repeat"])
    e = entry()
    check(e["game"][0]["what"] == "the zebra loader waits for the zebra card" and e["game"][0]["source"] == "docs/research/s999/ONE.md"
          and e["ours"][0]["verdict"] == "match" and len(e["game"]) == 3 and len(e["ours"]) == 2,
          "a row owned by another note is not touched by a second note (rc %d)\n%s%s" % (rc, out, json.dumps(e)))
    # a topic without an entry and without an ours line is kept in claims only
    rc, out = lodge(n2, ["0x82340000 | L | the yak has no ours line | evidence: x | seam=yak"])
    check(rc == 0 and not os.path.exists(yp) and "no entry for yak: row kept in claims only" in out,
          "a topic without an entry and without an ours line makes no entry and says so (rc %d)\n%s" % (rc, out))
    check(any(r.get("seam") == "yak" for r in json.load(open(os.path.join(root, "db", "claims", "s999_TWO.json"), encoding="utf-8"))),
          "the row stays in the claims file")


def seam_anchor_cases(tmp, table):
    """SEAMWIRE review: the cited line is checked against the tree the note cited it in (a git fixture tree). A file changed since the
    note's row was committed: the line's text then is followed to the one line that holds it now, or the row is refused "anchor
    unverified"; a row re-cited since (not in any commit) is read as it is now. And two claims of one note on one ours line make one row,
    the first claim's, on every lodge (no flip-flop)."""
    root = os.path.join(tmp, "repo3")
    os.makedirs(os.path.join(root, "docs", "research", "s999"))
    os.makedirs(os.path.join(root, "src"))
    zh = os.path.join(root, "src", "z.h")
    sp = os.path.join(root, "db", "seams", "zebra.json")
    n1 = os.path.join(root, "docs", "research", "s999", "ONE.md")

    def write(path, text):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def git(*a):
        r = subprocess.run(["git", "-C", root, "-c", "user.name=selftest", "-c", "user.email=selftest@example.invalid", "-c", "core.autocrlf=false"]
                           + list(a), capture_output=True, text=True, encoding="utf-8", errors="replace")
        check(r.returncode == 0, "fixture git %s: %s" % (" ".join(a), r.stdout + r.stderr))

    def lodge(path):
        return run([LODGE, path, "--root", root, "--resolver-table", table])

    rows = ["0x82331BE0 | C | the zebra loader waits for the card | evidence: x | seam=zebra ours=src/z.h:2 verdict=match",
            "0x82340000 | L | the zebra signal is posted | evidence: x | seam=zebra ours=src/z.h:3 verdict=replace",
            "sub_82331BC0 | U | the zebra card ends | evidence: x | seam=zebra ours=src/z.h:4 verdict=match"]
    write(zh, "// zebra\nint zebra_wait = 1;\nint zebra_post = 2;\nint zebra_end = 3;\n")
    write(n1, note(rows))
    git("init", "-q")
    git("add", "src/z.h", "docs/research/s999/ONE.md")
    git("commit", "-q", "-m", "the note cites z.h")
    # the planted moved line: a line inserted above the cited ones (wait 2 -> 3, end 4 -> 5) and the post line's text changed
    write(zh, "// zebra\nint zebra_new = 0;\nint zebra_wait = 1;\nint zebra_post = 9;\nint zebra_end = 3;\n")
    git("add", "src/z.h")
    git("commit", "-q", "-m", "a later lane moves the lines")
    rc, out = lodge(n1)
    e = json.load(open(sp, encoding="utf-8")) if os.path.exists(sp) else {"ours": []}
    got = sorted(o["anchor"] for o in e["ours"])
    check(rc == 0 and got == ["int zebra_end = 3;", "int zebra_wait = 1;"],
          "a cited line that moved since the note is followed to its text, a changed one is not anchored (rc %d): %s\n%s" % (rc, got, out))
    check("src/z.h:2 -> 3" in out and "src/z.h:4 -> 5" in out, "a followed line is reported with its old and new number\n" + out)
    check("anchor unverified" in out and "src/z.h:3" in out and "int zebra_post = 2;" in out,
          "a cited line whose text changed since the note is refused 'anchor unverified', with the text it held\n" + out)
    # a row re-cited since (its ours= / verdict= text in no commit) is read in the tree as it is now
    write(n1, note(rows[:2] + [rows[1].replace("z.h:3 verdict=replace", "z.h:4 verdict=repeat")] + rows[2:]))
    rc, out = lodge(n1)
    e = json.load(open(sp, encoding="utf-8")) if os.path.exists(sp) else {"ours": []}
    check(any(o["anchor"] == "int zebra_post = 9;" and o["verdict"] == "repeat" for o in e["ours"]),
          "a re-cited row is anchored in today's tree (rc %d)\n%s" % (rc, out))
    # a row edited since whose ours= no claims commit holds (this fixture never commits db\claims): read in today's tree, no refusal for it
    # (with its claims committed, the same edit keeps the first claims commit's date: seam_reword_cases)
    write(n1, note([rows[0].replace("waits for the card", "waits for the card (re-read)"), rows[2]]))
    rc, out = lodge(n1)
    check(rc == 0 and "anchor unverified" not in out and "src/z.h:2 ->" not in out, "an edited row is read in today's tree (rc %d)\n%s" % (rc, out))
    # two claims of one note on one ours line: one row (the first claim's why), and the second lodge changes nothing
    n2 = os.path.join(root, "docs", "research", "s999", "TWO.md")
    write(n2, note(["0x82331BE0 | C | the gnu first claim | evidence: x | seam=gnu ours=src/z.h:1 verdict=match",
                    "0x82340000 | C | the gnu second claim | evidence: x | seam=gnu ours=src/z.h:1 verdict=match"]))
    lodge(n2)
    gp = os.path.join(root, "db", "seams", "gnu.json")
    b0 = open(gp, "rb").read() if os.path.exists(gp) else b""
    rc, out = lodge(n2)
    g = json.load(open(gp, encoding="utf-8")) if os.path.exists(gp) else {"ours": []}
    check(rc == 0 and open(gp, "rb").read() == b0 and "ours +0 ~0 =2" in out and [o["why"] for o in g["ours"]] == ["the gnu first claim"],
          "two claims on one ours line keep the first claim's row on every lodge (rc %d): %s\n%s" % (rc, [o["why"] for o in g["ours"]], out))


def seam_reword_cases(tmp, table):
    """S111 SEAMCITE: a reworded claims row keeps its citation date. The row's ours= file moved after the note cited it; the claim is then
    reworded (ours= unchanged, a new claims commit). The citation is dated from the EARLIEST claims commit holding that address with that
    ours= value, so the moved line is followed to its text; dated from the reword it would wire whatever line now sits at the cited number."""
    root = os.path.join(tmp, "repo4")
    os.makedirs(os.path.join(root, "docs", "research", "s999"))
    os.makedirs(os.path.join(root, "src"))
    zh = os.path.join(root, "src", "z.h")
    sp = os.path.join(root, "db", "seams", "zebra.json")
    n1 = os.path.join(root, "docs", "research", "s999", "ONE.md")

    def write(path, text):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def git(*a):
        r = subprocess.run(["git", "-C", root, "-c", "user.name=selftest", "-c", "user.email=selftest@example.invalid", "-c", "core.autocrlf=false"]
                           + list(a), capture_output=True, text=True, encoding="utf-8", errors="replace")
        check(r.returncode == 0, "fixture git %s: %s" % (" ".join(a), r.stdout + r.stderr))

    def lodge(path):
        return run([LODGE, path, "--root", root, "--resolver-table", table])

    row = "0x82331BE0 | C | the zebra loader waits for the card | evidence: x | seam=zebra ours=src/z.h:2 verdict=match"
    write(zh, "// zebra\nint zebra_wait = 1;\nint zebra_post = 2;\n")
    write(n1, note([row]))
    git("init", "-q")
    lodge(n1)
    os.remove(sp)   # the first lodge's seam file is not what is under test
    git("add", "src/z.h", "docs/research/s999/ONE.md", "db/claims")
    git("commit", "-q", "-m", "the note cites z.h:2 and its claims are lodged")
    write(zh, "// zebra\nint zebra_new = 0;\nint zebra_wait = 1;\nint zebra_post = 2;\n")   # a later lane moves the cited line to 3
    git("add", "src/z.h")
    git("commit", "-q", "-m", "z.h moves the cited line")
    write(n1, note([row.replace("waits for the card", "blocks until the card is there")]))   # the reword: same ours=
    lodge(n1)
    os.remove(sp)
    git("add", "docs/research/s999/ONE.md", "db/claims")
    git("commit", "-q", "-m", "a review rewords the claim")
    rc, out = lodge(n1)
    e = json.load(open(sp, encoding="utf-8")) if os.path.exists(sp) else {"ours": []}
    got = [o["anchor"] for o in e["ours"]]
    check(rc == 0 and got == ["int zebra_wait = 1;"],
          "a reworded row keeps the citation date of its first claims commit: the moved line is followed, not read at the cited number (rc %d): %s\n%s" % (rc, got, out))
    check("src/z.h:2 -> 3" in out, "the followed line is reported\n" + out)


def seam_merge_cases(tmp, table):
    """SEAMCITE review: a citation made in a MERGE commit (the row written while resolving a merge, ENVCUBE 9ababa37c) is dated from that
    merge. `git log -S` without -m reads no merge's diff, so the row's text was "in no commit" and the line was read in today's tree: after
    a later lane moved the cited line, the row was wired on whatever line now sits at the cited number (ENVCUBE jboy.h, the `tname` line)."""
    root = os.path.join(tmp, "repo5")
    os.makedirs(os.path.join(root, "docs", "research", "s999"))
    os.makedirs(os.path.join(root, "src"))
    zh = os.path.join(root, "src", "z.h")
    sp = os.path.join(root, "db", "seams", "zebra.json")
    n1 = os.path.join(root, "docs", "research", "s999", "ONE.md")

    def write(path, text):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def git(*a):
        r = subprocess.run(["git", "-C", root, "-c", "user.name=selftest", "-c", "user.email=selftest@example.invalid", "-c", "core.autocrlf=false"]
                           + list(a), capture_output=True, text=True, encoding="utf-8", errors="replace")
        check(r.returncode == 0, "fixture git %s: %s" % (" ".join(a), r.stdout + r.stderr))

    def lodge(path):
        return run([LODGE, path, "--root", root, "--resolver-table", table])

    write(zh, "// zebra\nint zebra_wait = 1;\nint zebra_post = 2;\n")
    git("init", "-q", "-b", "main")
    git("add", "src/z.h")
    git("commit", "-q", "-m", "base")
    git("checkout", "-q", "-b", "side")
    write(os.path.join(root, "src", "y.h"), "// side\n")
    git("add", "src/y.h")
    git("commit", "-q", "-m", "a lane's commit")
    git("checkout", "-q", "main")
    write(os.path.join(root, "src", "x.h"), "// main\n")
    git("add", "src/x.h")
    git("commit", "-q", "-m", "the session's commit")
    git("merge", "-q", "--no-ff", "--no-commit", "side")
    write(n1, note(["0x82331BE0 | C | the zebra loader waits for the card | evidence: x | seam=zebra ours=src/z.h:2 verdict=match"]))
    lodge(n1)
    os.remove(sp)   # the first lodge's seam file is not what is under test
    git("add", "docs/research/s999/ONE.md", "db/claims")
    git("commit", "-q", "-m", "merge side: the note's row is written in the merge")
    write(zh, "// zebra\nint zebra_new = 0;\nint zebra_wait = 1;\nint zebra_post = 2;\n")   # a later lane moves the cited line to 3
    git("add", "src/z.h")
    git("commit", "-q", "-m", "z.h moves the cited line")
    rc, out = lodge(n1)
    e = json.load(open(sp, encoding="utf-8")) if os.path.exists(sp) else {"ours": []}
    got = [o["anchor"] for o in e["ours"]]
    check(rc == 0 and got == ["int zebra_wait = 1;"],
          "a row cited in a merge commit is dated from that merge: the moved line is followed, not read at the cited number (rc %d): %s\n%s" % (rc, got, out))


def main():
    tmp = tempfile.mkdtemp(prefix="lodge_selftest_")
    try:
        root = os.path.join(tmp, "repo")
        os.makedirs(os.path.join(root, "db"))
        os.makedirs(os.path.join(root, "docs", "research", "s999"))
        with open(os.path.join(root, "db", "fix_a.json"), "w", encoding="utf-8", newline="") as f:
            f.write(FIX_A)
        with open(os.path.join(root, "db", "fix_b.json"), "w", encoding="utf-8", newline="") as f:
            f.write(FIX_B)
        table = os.path.join(tmp, "table.json")
        with open(table, "w", encoding="utf-8") as f:
            json.dump(TABLE, f)
        np = os.path.join(root, "docs", "research", "s999", "FIX.md")
        with open(np, "w", encoding="utf-8") as f:
            f.write(note(GOOD))
        base = [LODGE, np, "--root", root, "--resolver-table", table]
        cfile = os.path.join(root, "db", "claims", "s999_FIX.json")

        # q fact: nothing before the lodge
        sys.path.insert(0, os.path.join(REPO, "tools", "q"))
        import qfact
        old_root = (qfact.ROOT, qfact.INDEX)
        qfact.use_root(root)
        con, _ = qfact.connect(True)
        before_hit = qfact.search(con, ["zebra", "loader"])["hits"]
        con.close()
        check(not before_hit, "q fact finds the claim before it is lodged (it must not)")

        before = snap(root)
        rc, out = run(base + ["--dry"])
        check(rc == 0 and snap(root) == before, "--dry changes nothing (rc %d)\n%s" % (rc, out))
        rc, out = run(base)
        check(rc == 0 and "changed: db/claims/s999_FIX.json" in out, "the lodge exits 0 and names the claims file (got %d)\n%s" % (rc, out))
        after = snap(root)
        check({p: b for p, b in after.items() if p != cfile} == before, "the existing db files are byte-equal after the lodge (no edit, no new entry)")
        check(set(after) - set(before) == {cfile}, "exactly one new file, db/claims/s999_FIX.json: %s" % sorted(set(after) - set(before)))
        rows = json.loads(after.get(cfile, b"[]"))
        exp = [
            {"address": "0x82331BE0", "function": "0x82331BC0", "offset": "0x20", "mark": "C",
             "claim": "the fixture card waits for the zebra loader before it closes", "evidence": "decomp sub_82331BC0 +0x20",
             "note": "docs/research/s999/FIX.md"},
            {"address": "0x82340000", "function": "0x82340000", "offset": "0x0", "mark": "L",
             "claim": "the compact fixture function posts the zebra signal", "evidence": "callers listing", "note": "docs/research/s999/FIX.md"},
            {"address": "0x836F38D0", "function": None, "offset": None, "mark": "C", "claim": "the fixture zebra table holds one word per slot",
             "evidence": "field read in inputs_contrib.h:146", "note": "docs/research/s999/FIX.md", "system": "match"},
        ]
        check(rows == exp, "the claims file holds the three rows with function / offset (data: null):\n%s" % json.dumps(rows, indent=1))

        # idempotent
        rc, out = run(base)
        check(rc == 0 and snap(root) == after and "unchanged" in out and "changed:" not in out,
              "a second lodge of the same note changes nothing (rc %d)\n%s" % (rc, out))

        # validate.py: the lodged file passes and is not read as function entries; planted bad files fail
        val = load(os.path.join(REPO, "tools", "db", "validate.py"), "db_validate")

        def errors():
            res, _ = val.validate(argparse.Namespace(db=os.path.join(root, "db"), docs=os.path.join(root, "docs", "systems"), index="",
                                                     no_index=True, gen=os.path.join(root, "gen"), coverage=os.path.join(root, "cov.json"),
                                                     warn_layout=False))
            return res
        res = errors()
        check(res is not None and not res["report"].errors and res["entries"] == 2,
              "validate.py passes the lodged db and counts only the 2 function entries: %s, %s" % (res and res["report"].errors, res and res["entries"]))
        bad_rows = {
            "code text": dict(exp[0], claim="the card does mtctr r11 then calls it"),
            "bad mark": dict(exp[0], mark="X"),
            "missing field": {k: v for k, v in exp[0].items() if k != "evidence"},
            "function without offset": dict(exp[0], offset=None),
        }
        bad_path = os.path.join(root, "db", "claims", "s999_BAD.json")
        for what, r in bad_rows.items():
            with open(bad_path, "w", encoding="utf-8") as f:
                json.dump([r], f)
            res = errors()
            check(res is not None and any(k == "claims" for k, _, _ in res["report"].errors), "validate.py fails a claims file with %s: %s"
                  % (what, res and res["report"].errors))
        os.remove(bad_path)

        # q fact finds it, with the address; q.claims_for answers through address and function
        con, _ = qfact.connect(True)
        hits = qfact.search(con, ["zebra", "loader"])["hits"]
        con.close()
        check(hits and any(h["kind"] == "claim" and "0x82331BE0" in json.dumps(h) and "zebra loader" in json.dumps(h) for h in hits),
              "q fact finds the lodged claim (kind claim) with its address: %s" % hits)
        con, _ = qfact.connect(True)
        hits = qfact.search(con, ["zebra", "table", "slot"], kind="claim")["hits"]
        con.close()
        check(hits and "0x836F38D0" in hits[0]["lines"][0]["text"], "q fact --kind claim finds the data claim: %s" % hits)
        con, _ = qfact.connect(True)
        hits = qfact.search(con, ["S999", "fixture", "note"], kind="claim")["hits"]   # the note's title, not in any claim's own text
        con.close()
        check(len(hits) == 3, "q fact finds a note's claims by the note's title (3 rows): %s" % hits)
        qfact.ROOT, qfact.INDEX = old_root
        qm = load(os.path.join(REPO, "tools", "q", "q.py"), "q_selftest_copy")
        qm.REPO = root
        check([r["address"] for r in qm.claims_for(0x836F38D0)] == ["0x836F38D0"], "q claims_for: a data address")
        check([r["address"] for r in qm.claims_for(0x82331BC0)] == ["0x82331BE0"], "q claims_for: the function holding a claim (via `function`)")
        check([r["address"] for r in qm.claims_for(0x82340000)] == ["0x82340000"], "q claims_for: a function start")
        check(qm.claims_for(0x82331BE4) == [], "q claims_for: an address no claim names")
        check(qm.claim_line(qm.claims_for(0x82331BC0)[0]).startswith("[C] +0x20 the fixture card"), "q claim_line shows mark and offset")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            qm.print_db({"func": "sub_82331BC0", "entries": [], "claims": qm.claims_for(0x82331BC0), "docs": []}, argparse.Namespace(limit=8))
        check("1 lodged claim(s)" in buf.getvalue() and "zebra loader" in buf.getvalue(), "q db prints the claim: %s" % buf.getvalue())

        # an edited note replaces its rows (the dropped row is gone, the changed one is new)
        with open(np, "w", encoding="utf-8") as f:
            f.write(note([GOOD[0], GOOD[2].replace("one word per slot", "one word per wrestler slot")]))
        rc, out = run(base)
        rows2 = json.loads(open(cfile, encoding="utf-8").read())
        check(rc == 0 and "rows replaced" in out and [r["address"] for r in rows2] == ["0x82331BE0", "0x836F38D0"]
              and rows2[1]["claim"].endswith("one word per wrestler slot"), "an edited note replaces its rows (rc %d)\n%s" % (rc, out))
        check({p: b for p, b in snap(root).items() if p != cfile} == before, "the replace edits no other db file")

        # refusals: nothing written
        snap0 = snap(root)
        bad = {"address that does not resolve": ["0x82999999 | C | the fixture says something | evidence: nothing"],
               "bad mark": ["0x82331BC0 | X | the fixture claim | evidence: x"],
               "missing evidence": ["0x82331BC0 | C | the fixture claim | proof by hand"]}
        for c in CODE:
            bad["game code %r in the claim" % c] = ["0x82331BC0 | C | the fixture does %s and returns | evidence: decomp sub_82331BC0" % c]
            bad["game code %r in the evidence" % c] = ["0x82331BC0 | C | the fixture returns | evidence: the listing at +0x10: %s" % c]
        bp = os.path.join(root, "docs", "research", "s999", "FIX.md")   # the SAME note: a refused re-lodge must keep the old file
        for what, rows in bad.items():
            with open(bp, "w", encoding="utf-8") as f:
                f.write(note(rows))
            rc, out = run([LODGE, bp, "--root", root, "--resolver-table", table])
            check(rc != 0 and snap(root) == snap0, "%s is refused with nothing written (rc %d)\n%s" % (what, rc, out))
        # claims.check_rows alone (what note_sites.py --check runs, without validate.py's second check behind it)
        cl = load(os.path.join(HERE, "claims.py"), "claims_selftest_copy")
        tres = cl.TableResolver(TABLE["funcs"], TABLE["data"])
        for c in CODE:
            for rows in (cl.parse_claims(note(["0x82331BC0 | C | the fixture does %s and returns | evidence: decomp" % c])),
                         cl.parse_claims(note(["0x82331BC0 | C | the fixture returns | evidence: the listing: %s" % c]))):
                errs = cl.check_rows(rows, tres)
                check(any("game code" in x for x in errs), "claims.check_rows refuses %r: %s" % (c, errs))
        check(cl.check_rows(cl.parse_claims(note(["0x82331BC0 | C | " + ENGLISH + " | evidence: decomp sub_82331BC0 +0x40"])), tres) == [],
              "claims.check_rows passes plain English")
        # ABSLINT planted twins: an [L] row with an absolute word FAILS, the same row with scope= is honest, a [C] only warns
        for mk, tail, want in (("L", "", True), ("U", "", True), ("L", " | scope=4 of 34 refs read", False), ("C", "", False)):
            ab = cl.check_rows(cl.parse_claims(note(["0x82331BC0 | %s | the fixture is the only writer of the flag | evidence: decomp sub_82331BC0 +0x40%s" % (mk, tail)])), tres)
            check(bool(ab) == want and (not want or any("absolute" in x for x in ab)), "abslint [%s]%s: errors=%s" % (mk, tail, ab))
        # ABSLINT review: a lodged [C] row with an absolute word, re-marked [L] with its text unchanged (the PODGLASS correction), FAILS (the
        # no baseline exists to hide it); pull_claims keeps scope=; lodge_lint sees a scope= the claims file does not hold
        abm = load(os.path.join(HERE, "abslint.py"), "abslint_selftest_copy")
        warn_c = next(((n, r) for lv, n, r, _ in abm.findings(REPO) if lv == "WARN"), None)
        check(warn_c is not None, "abslint: the checkout's db\\claims holds a [C] row with an absolute word (the re-mark twin needs one)")
        if warn_c:
            ab = cl.check_rows(cl.parse_claims(note(["0x82331BC0 | L | %s | evidence: %s" % (warn_c[1]["claim"], warn_c[1]["evidence"])])), tres)
            check(any("absolute" in x for x in ab), "abslint: a lodged [C] claim of %s re-marked [L] FAILS: %s" % (warn_c[0], ab))
        pc = load(os.path.join(HERE, "pull_claims.py"), "pull_claims_selftest_copy")
        o = {"address": "0x82331BC0", "mark": "L", "claim": "the fixture is the only writer of the flag", "evidence": "decomp sub_82331BC0 +0x40",
             "scope": "4 of 34 refs read"}
        line = pc.render("0x82331BC0", o, None)
        back = cl.parse_claims(note([line]))
        check(back and back[0]["scope"] == "4 of 34 refs read" and pc.content(back[0]) == pc.content(o), "pull_claims keeps scope=: %s" % line)
        llm = load(os.path.join(HERE, "lodge_lint.py"), "lodge_lint_selftest_copy")
        check(llm.differences(back, [dict(o, scope=None)]) != [], "lodge_lint sees a scope= the claims file does not hold")
        for c in CODE:   # scope= is lodged into db\claims too: the same code-text refusal as the claim and the evidence
            ab = cl.check_rows(cl.parse_claims(note(["0x82331BC0 | L | the fixture is the only writer | evidence: decomp | scope=the listing: %s" % c])), tres)
            check(any("game code" in x for x in ab), "claims.check_rows refuses %r in scope=: %s" % (c, ab))
        with open(bp, "w", encoding="utf-8") as f:
            f.write(note(["0x82331BC0 | C | " + ENGLISH + " | evidence: decomp sub_82331BC0 +0x40, field 1176 --writes, run 328 log"]))
        rc, out = run([LODGE, bp, "--root", root, "--resolver-table", table, "--dry"])
        check(rc == 0, "plain English (adds / branches / returns) and a q-command evidence pass the code check (rc %d)\n%s" % (rc, out))
        with open(bp, "w", encoding="utf-8") as f:
            f.write(note(GOOD) + note(GOOD))
        rc, out = run([LODGE, bp, "--root", root, "--resolver-table", table])
        check(rc != 0 and "two" in out and snap(root) == snap0, "two claims blocks are refused (rc %d)\n%s" % (rc, out))
        # no block: nothing lodged, the earlier file stays
        with open(bp, "w", encoding="utf-8") as f:
            f.write("# S999 NOCLAIMS\n\n## Sites the build will touch\n\nNo sites: x\n")
        rc, out = run([LODGE, bp, "--root", root, "--resolver-table", table])
        check(rc == 0 and "no claims block" in out and "stays as it is" in out and snap(root) == snap0,
              "a note without claims lodges nothing and leaves its earlier claims file\n" + out)

        # note_sites --check: malformed block fails, no block passes silently
        with open(bp, "w", encoding="utf-8") as f:
            f.write(note(["0x82331BC0 | X | the claim | evidence: y"]))
        rc, out = run([SITES, "--check", bp])
        check(rc == 1 and "claims: ERROR" in out, "note_sites --check refuses a bad mark (rc %d)\n%s" % (rc, out))
        with open(bp, "w", encoding="utf-8") as f:
            f.write("# S999 NOCLAIMS\n\n## Sites the build will touch\n\nNo sites: x\n")
        rc, out = run([SITES, "--check", bp])
        check(rc == 0 and "claims" not in out, "note_sites --check passes a note without claims (rc %d)\n%s" % (rc, out))

        seam_cases(tmp, table)
        seam_anchor_cases(tmp, table)
        seam_reword_cases(tmp, table)
        seam_merge_cases(tmp, table)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("lodge_note_selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
