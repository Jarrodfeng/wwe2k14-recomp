#!/usr/bin/env python3
r"""S111 INDEX_PLAN lane B selftest: the TYPE column, the lodge lint, the site migration, the embed normalisation. Fixture trees in a temp folder, a fixed
address table instead of the code index (no game, no index; 0 runs).

  py -I tools\handback\typed_lodge_selftest.py

Cases: a typed note (site / role / field / embed [C] with a constructor row / embed [L] / enum rows) lodges kind / key / name / site into its claims file
and validate.py passes the file; an old 4-column note still lodges unchanged; `note_sites.py --check` FAILS a malformed TYPE column (an unknown kind, an
unaligned site, a site outside its function, a site in another function than the row, a field with a bad vtable form, an enum register, an embed row
marked C with no constructor row, two TYPE columns); the lodge lint (tools\handback\lodge_lint.py) in a scratch git repo: a note with a block and no
claims file FAILS and prints the exact lodge command, passes once lodged, FAILS again when the claim text, the mark or the TYPE column of the block
changes, ignores an untracked note and a note without a block; the migration (type_sites.py): a row citing its own `sub_X +0x20`, `sub_X+0x20` and a
bare `+2C` is typed `site`, a row citing another function, a bare all-digit offset, no offset is left alone, an unaligned or outside offset is skipped,
CRLF is kept, a second run changes nothing, and the relodged file carries the site; the embed normalisation: a slot function of the inner type with
`[in_r3-976]` prints as the outer type +224; check.py lists the `lodge` row."""
import contextlib
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
LINT = os.path.join(HERE, "lodge_lint.py")
MIGRATE = os.path.join(HERE, "type_sites.py")
PULL = os.path.join(HERE, "pull_claims.py")
fails = []
ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL: " + msg)


def run(args, cwd=None):
    r = subprocess.run([sys.executable, "-I"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=cwd)
    return r.returncode, r.stdout + r.stderr


def git(root, *a):
    r = subprocess.run(["git", "-C", root] + list(a), env=ENV, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


TABLE = {"funcs": {"0x82331BC0": 256, "0x82340000": 128, "0x825A3908": 4096, "0x825A20A0": 4096},
         "data": ["0x836F38D0"],
         "facts": {"vts": ["0x8201E6E8", "0x8201E6D8"], "ctors": {"0x825A20A0": "0x8201E6E8"},
                   "embeds": [["0x8201E6E8", 1200, "0x8201E6D8"]], "slots": {"0x825A3908": ["0x8201E6D8"]}}}


def note(rows, extra=""):
    return ("# S999 FIX: fixture note\n\n## Sites the build will touch\n\nNo sites: fixture\n\n" + extra +
            "```claims\n" + "\n".join(rows) + "\n```\n")


TYPED = [
    "0x82331BE0 | C | the fixture card waits for the zebra loader | evidence: decomp sub_82331BC0 +0x20 | site sub_82331BC0+0x20",
    "sub_82340000 | L | the fixture function posts the zebra signal | evidence: callers listing | role sub_82340000",
    "0x825A20F4 | C | the fixture constructor stores the node vptr | evidence: asm sub_825A20A0 0x825A20F4 | embed vt:0x8201E6E8+1200 => vt:0x8201E6D8 draw node",
    "0x825A21E0 | C | the created flag is 1 when the id is small | evidence: asm sub_825A20A0 0x825A21E0 | field vt:0x8201E6E8+5076 created_flag | seam=zebra ours=src/z.h:2 verdict=match",
    "0x82331BC0 | C | the fixture consumer reads register 4 as a category | evidence: args sub_82331BC0 | enum arg:sub_82331BC0.r4=33 category 33",
    "0x836F38D0 | L | table of zebra slots | evidence: field read | field g:0x836F38D0+8 slot_word | system=match",
]


def write(root, rel, text, crlf=False):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    return p


def pull_cases(tmp, table):
    """Main session decision (S111 lane B review, rule 6): the note's block is the only source; db\\claims / claims-driven db\\seams rows are built
    from it. In a git fixture: (1) a landed note whose ours line moved and was re-anchored by a later lane, then typed by type_sites.py, relodges
    with 0 new seam rows (the junk-anchor twin: the TYPE column made the row 'a new citation' read at today's line); (2) a claims file edited by
    hand FAILS the lint as 'db\\claims edited by hand' with both sides' last commit; (3) pull_claims.py carries the file's rows into the block
    (a rewritten row keeps its TYPE column, a file-only row is inserted, a note-only row is kept and listed) and the relodge is identity."""
    root = os.path.join(tmp, "pullrepo")
    zh = write(root, "src/z.h", "// zebra\nint zebra_wait = 1;\nint zebra_post = 2;\n")
    rel = "docs/research/s999/P.md"
    r1 = "0x82331BE0 | C | the zebra loader waits | evidence: decomp sub_82331BC0 +0x20 | seam=zebra ours=src/z.h:2 verdict=match"
    r2 = "0x82340000 | L | the zebra post is late | evidence: callers"
    np = write(root, rel, note([r1, r2]))
    sp, cp = os.path.join(root, "db", "seams", "zebra.json"), os.path.join(root, "db", "claims", "s999_P.json")
    git(root, "init", "-q")
    rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
    check(rc == 0 and os.path.exists(sp), "pull fixture: the first lodge wires the seam row (rc %d)\n%s" % (rc, out))
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "land")
    # a later lane inserts a line above the cited one; the landing's recite re-anchors the seam row (land_seam.py)
    with open(zh, "w", encoding="utf-8", newline="\n") as f:
        f.write("// zebra\nint zebra_new = 0;\nint zebra_wait = 1;\nint zebra_post = 2;\n")
    e = json.load(open(sp, encoding="utf-8"))
    e["ours"][0].update(reanchored="re-anchored by landing s999/OTHER.md")
    with open(sp, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(e, indent=1) + "\n")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "later lane")
    rc, out = run([MIGRATE, "--root", root, "--resolver-table", table])
    check("1 rows typed in 1 notes" in out, "pull fixture: type_sites types the row (rc %d)\n%s" % (rc, out))
    b_seam = open(sp, "rb").read()
    rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
    e = json.load(open(sp, encoding="utf-8"))
    check(rc == 0 and len(e["ours"]) == 1 and not any(o["anchor"] == "int zebra_new = 0;" for o in e["ours"]) and "ours +0" in out,
          "relodging a typed, re-anchored note adds no seam row and no junk anchor (rc %d): %s\n%s" % (rc, [o["anchor"] for o in e["ours"]], out))
    check(open(sp, "rb").read() == b_seam, "relodging the unchanged (only typed) note leaves the seam file byte-equal")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "typed")
    # a review edits the claims file by hand (the S111 practice the decision retires): a mark and a claim, and a new row
    objs = json.load(open(cp, encoding="utf-8"))
    objs[0]["claim"] = "the zebra loader waits for the card"   # reworded in the claims file only: the citation is found by address + ours, not by the text
    objs[1].update(mark="C", claim="the zebra post is late by one frame")
    objs.append({"address": "0x82340010", "function": "0x82340000", "offset": "0x10", "mark": "L", "claim": "the zebra post repeats",
                 "evidence": "callers listing", "note": rel})
    with open(cp, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(objs, indent=2, ensure_ascii=False) + "\n")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "review edits the claims file")
    rc, out = run([LINT, "--root", root])
    check(rc == 1 and "db\\claims edited by hand" in out and "put the change in the note's block" in out and "newer: the claims file" in out
          and "review edits the claims file" in out and "lodge_note.py docs\\research\\s999\\P.md" in out,
          "a hand-edited claims file FAILS the lint naming both sides' last commit and the newer side (rc %d)\n%s" % (rc, out))
    rc, out = run([PULL, "--root", root, np])
    text = open(np, encoding="utf-8").read()
    check(rc == 0 and "the zebra post is late by one frame" in text and "0x82340010 | L | the zebra post repeats | evidence: callers listing" in text
          and "| site sub_82331BC0+0x20 |" in text, "pull_claims carries the file's rows into the block, keeping the TYPE column (rc %d)\n%s\n%s" % (rc, out, text))
    b_claims = open(cp, "rb").read()
    rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
    e = json.load(open(sp, encoding="utf-8"))
    check(rc == 0 and open(cp, "rb").read() == b_claims, "after the pull the lodge is identity: the claims file is byte-equal (rc %d)\n%s" % (rc, out))
    check([(o["anchor"], o["why"]) for o in e["ours"]] == [("int zebra_wait = 1;", "the zebra loader waits for the card")]
          and e["game"][0]["what"] == "the zebra loader waits for the card",
          "the reworded claim updates its seam rows in place: no second row, no junk anchor: %s\n%s" % ([o["anchor"] for o in e["ours"]], out))
    b_seam = open(sp, "rb").read()
    rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
    check(rc == 0 and open(sp, "rb").read() == b_seam and open(cp, "rb").read() == b_claims, "a second lodge is identity (rc %d)\n%s" % (rc, out))
    rc, out = run([LINT, "--root", root])
    check(rc == 0, "after the pull the lint passes (rc %d)\n%s" % (rc, out))
    with open(np, "w", encoding="utf-8", newline="\n") as f:
        f.write(text.replace("0x82340010 | L |", "0x82340020 | U | a note-only zebra row | evidence: x\n0x82340010 | L |"))
    rc, out = run([PULL, "--root", root, "--dry", np])
    check(rc == 0 and "1 kept (note only)" in out and "0x82340020" in out, "a row only in the note is kept and listed (rc %d)\n%s" % (rc, out))


DERIVE_LISTING = """DEFINE_REX_FUNC(sub_82000000) {
\t// lis r11,-32254
\t// addi r4,r11,-6440
\t// lis r10,-32254
\t// addi r7,r10,-6440
\t// lbz r7,0(r8)
\t// stw r4,1200(r3)
\t// stw r7,428(r3)
\t// addi r30,r11,-6440
\t// bl 0x82001000
\t// stw r30,8(r3)
\t// addi r5,r11,-6440
\t// bl 0x82001000
\t// stw r5,12(r3)
\t// addi r6,r11,-6440
loc_82000038:
\t// stw r6,16(r3)
\t// blr
}
"""


def derive_cases(fk, tmp):
    """One constructor of vt 0x8201E6E8 (fixture listing + field / code index tables): the stored vptr 0x8201E6D8 at +1200 (straight line, the
    S111 character shape) and at +8 (a non-volatile register across a call) is an embed; +428 (a byte load over the register, sub_82398EA8
    0x82398FC4's shape), +12 (a volatile register across a call) and +16 (a join label before the store) are not. -> the derived offsets."""
    import sqlite3
    if os.path.join(REPO, "tools", "q") not in sys.path:
        sys.path.insert(0, os.path.join(REPO, "tools", "q"))   # qdecomp (read_listing), as IndexFacts() puts it
    gen = os.path.join(tmp, "gen")
    os.makedirs(gen, exist_ok=True)
    with open(os.path.join(gen, "fix.cpp"), "w", encoding="utf-8", newline="\n") as f:
        f.write(DERIVE_LISTING)
    f0, vt, inner = 0x82000000, 0x8201E6E8, 0x8201E6D8
    fcon, icon = sqlite3.connect(":memory:"), sqlite3.connect(":memory:")
    fcon.executescript("create table vtables(vt); create table ctors(func, vt); create table roots(id, text);"
                       "create table rows(addr, off, func, rw, width, root);")
    fcon.executemany("insert into vtables values (?)", [(vt,), (inner,)])
    fcon.execute("insert into ctors values (?, ?)", (f0, vt))
    fcon.execute("insert into roots values (1, 'a0')")
    fcon.executemany("insert into rows values (?, ?, ?, 'W', 4, 1)", [(f0 + a, k, f0) for a, k in ((0x14, 1200), (0x18, 428), (0x24, 8), (0x30, 12), (0x38, 16))])
    icon.executescript("create table funcs(addr, file, line, name); create table imms(func, site, value, op); create table rtti_vtable(vt); create table tables(start);")
    icon.execute("insert into funcs values (?, 'fix.cpp', 1, 'sub_82000000')", (f0,))
    icon.executemany("insert into imms values (?, ?, ?, 'lis+addi')", [(f0, f0 + a, inner) for a in (0x04, 0x0C, 0x1C, 0x28, 0x34)])
    ix = fk.IndexFacts.__new__(fk.IndexFacts)
    ix.gen, ix.fcon, ix.icon, ix.image, ix._img, ix._emb, ix._vts = gen, fcon, icon, None, None, {}, None
    return sorted((e["off"] for e in ix.derive_ctor(f0)), key=lambda k: (k != 1200, k))


def main():
    tmp = tempfile.mkdtemp(prefix="typed_lodge_")
    try:
        table = os.path.join(tmp, "table.json")
        with open(table, "w", encoding="utf-8") as f:
            json.dump(TABLE, f)
        fk = load(os.path.join(REPO, "tools", "q", "qfactkey.py"), "qfactkey")

        # ---- the grammar
        t = fk.parse_type("embed vt:0x8201E6E8+1200 => vt:0x8201E6D8 draw node")
        check(t["key"] == "vt:0x8201E6E8+1200 => vt:0x8201E6D8" and t["name"] == "draw node" and t["off"] == 1200, "embed parses: %s" % t)
        check(fk.parse_type("field vt:0x8201E6E8+0x13D4 x")["key"] == "vt:0x8201E6E8+5076", "a hex offset is canonicalised to decimal")
        check(fk.parse_type("site sub_825a3908+808")["key"] == "sub_825A3908+0x328", "a site key is canonical hex")
        check(fk.parse_type("enum arg:sub_82C30A10.r4=0x21")["key"] == "arg:sub_82C30A10.r4=33", "an enum key is canonical")
        for bad in ("site sub_825A3908+0x3", "site sub_825A3908", "field vt:8201E6E8", "field vt:0x8201E6E8", "embed vt:0x8201E6E8+4 => vt:0x8201E6D8+4",
                    "enum arg:sub_82C30A10.r40=1", "wat sub_82C30A10", "role"):
            try:
                fk.parse_type(bad)
                check(False, "grammar accepted %r" % bad)
            except fk.FactKeyError:
                pass
        for bad in ("site sub_825A3908+0040", "field vt:0x8201E6E8+08 x"):   # review: a leading-zero decimal crashed int(s, 0) with a traceback
            try:
                fk.parse_type(bad)
                check(False, "grammar accepted %r" % bad)
            except Exception as e:   # noqa: BLE001 (the planted twin: a ValueError escaped before the fix)
                check(isinstance(e, fk.FactKeyError), "%r raises FactKeyError, not %s: %s" % (bad, type(e).__name__, e))

        # ---- a typed note lodges; the file carries the TYPE fields; validate.py passes it
        root = os.path.join(tmp, "repo")
        np = write(root, "docs/research/s999/FIX.md", note(TYPED))
        rc, out = run([SITES, "--check", np, "--root", root, "--resolver-table", table])
        check(rc == 0 and "claims: 6 rows: OK" in out, "a typed note passes note_sites --check (rc %d)\n%s" % (rc, out))
        rc, out = run([LODGE, np, "--root", root, "--resolver-table", table])
        cp = os.path.join(root, "db", "claims", "s999_FIX.json")
        check(rc == 0 and os.path.exists(cp), "the typed note lodges (rc %d)\n%s" % (rc, out))
        objs = json.load(open(cp, encoding="utf-8")) if os.path.exists(cp) else []
        check([o.get("kind") for o in objs] == ["site", "role", "embed", "field", "enum", "field"], "kinds lodged: %s" % [o.get("kind") for o in objs])
        check(objs and objs[0].get("site") == "0x82331BE0" and objs[0].get("key") == "sub_82331BC0+0x20", "the site row carries key + site: %s" % (objs[:1],))
        check(objs and objs[2].get("name") == "draw node" and "site" not in objs[2], "the embed row carries its name and no site")
        check(objs and objs[3].get("seam") == "zebra" and objs[3].get("key") == "vt:0x8201E6E8+5076", "the TYPE column and the seam fields coexist")
        val = load(os.path.join(REPO, "tools", "db", "validate.py"), "db_validate")
        rep = val.Report()
        val.check_claim_rows(rep, "db/claims/s999_FIX.json", objs)
        check(not rep.errors, "validate.py passes the typed claims file: %s" % (rep.errors[:3],))
        bad = [dict(objs[0], key="sub_82331BC0+0x24")] + [dict(objs[1], kind="site")]
        rep = val.Report()
        val.check_claim_rows(rep, "x.json", bad)
        check(len(rep.errors) >= 2, "validate.py fails a site that differs from its key and a bad kind / key pair: %s" % (rep.errors,))

        # ---- an old 4-column note still lodges
        np4 = write(root, "docs/research/s999/OLD.md", note(["0x82331BE0 | C | the fixture card waits for the zebra loader | evidence: decomp sub_82331BC0 +0x20"]))
        rc, out = run([LODGE, np4, "--root", root, "--resolver-table", table])
        o4 = json.load(open(os.path.join(root, "db", "claims", "s999_OLD.json"), encoding="utf-8"))
        check(rc == 0 and "kind" not in o4[0] and "site" not in o4[0], "an old 4-column note still lodges with no TYPE fields (rc %d)\n%s" % (rc, out))

        # ---- malformed TYPE columns fail note_sites --check
        cases = [
            ("an unknown kind", "0x82331BE0 | C | c | evidence: e | wat sub_82331BC0+0x20", "not key=value"),
            ("an unaligned site", "0x82331BE0 | C | c | evidence: e | site sub_82331BC0+0x21", "multiple of 4"),
            ("a site outside its function", "0x82331BE0 | C | c | evidence: e | site sub_82331BC0+0x400", "not an instruction of sub_82331BC0"),
            ("a site in another function than the row", "0x82340010 | C | c | evidence: e | site sub_82331BC0+0x20", "another function"),
            ("a field with a bad vtable", "0x82331BE0 | C | c | evidence: e | field vt:zebra+8", "field key"),
            ("a field on an unknown vtable", "0x82331BE0 | C | c | evidence: e | field vt:0x82000000+8", "not a vtable"),
            ("an enum register", "0x82331BE0 | C | c | evidence: e | enum arg:sub_82331BC0.r99=1", "enum key"),
            ("an embed [C] with no constructor row", "0x82331BE0 | C | c | evidence: e | embed vt:0x8201E6E8+8 => vt:0x8201E6D8", "constructor stores"),
            ("two TYPE columns", "0x82331BE0 | C | c | evidence: e | role sub_82331BC0 | site sub_82331BC0+0x20", "two TYPE columns"),
        ]
        for what, row, want in cases:
            p = write(root, "docs/research/s999/BAD.md", note([row]))
            rc, out = run([SITES, "--check", p, "--root", root, "--resolver-table", table])
            check(rc != 0 and want in out, "%s fails note_sites --check with %r (rc %d)\n%s" % (what, want, rc, out))
        p = write(root, "docs/research/s999/BAD.md", note(["0x82331BE0 | L | c | evidence: e | embed vt:0x8201E6E8+8 => vt:0x8201E6D8"]))
        rc, out = run([SITES, "--check", p, "--root", root, "--resolver-table", table])
        check(rc == 0, "an embed row marked L needs no constructor row (rc %d)\n%s" % (rc, out))
        os.remove(p)

        # ---- the lodge lint in a scratch git repo
        lr = os.path.join(tmp, "lintrepo")
        os.makedirs(lr)
        git(lr, "init", "-q")
        write(lr, "docs/research/s999/A.md", note(TYPED))
        write(lr, "docs/research/s999/PLAIN.md", "# a note without a block\n")
        git(lr, "add", "docs")
        git(lr, "commit", "-q", "-m", "n")
        rc, out = run([LINT, "--root", lr])
        check(rc == 1 and "not lodged" in out and "lodge_note.py docs\\research\\s999\\A.md" in out,
              "a committed note with a block and no claims file FAILS with the exact lodge command (rc %d)\n%s" % (rc, out))
        write(lr, "docs/research/s999/UNTRACKED.md", note(TYPED))
        rc, out = run([LODGE, os.path.join(lr, "docs", "research", "s999", "A.md"), "--root", lr, "--resolver-table", table])
        check(rc == 0, "lodge for the lint fixture (rc %d)\n%s" % (rc, out))
        rc, out = run([LINT, "--root", lr])
        check(rc == 0 and "1 notes with a claims block, 0 not lodged" in out, "lodged: the lint passes; untracked and block-less notes are not judged (rc %d)\n%s" % (rc, out))
        for what, old, new in (("a changed claim", "the fixture card waits for the zebra loader", "the fixture card waits for the yak loader"),
                               ("a changed mark", "0x82331BE0 | C |", "0x82331BE0 | L |"),
                               ("a changed TYPE column", "site sub_82331BC0+0x20", "site sub_82331BC0+0x24"),
                               ("a dropped TYPE column", "| role sub_82340000", "")):
            ap = os.path.join(lr, "docs", "research", "s999", "A.md")
            text = open(ap, encoding="utf-8").read()
            with open(ap, "w", encoding="utf-8", newline="") as f:
                f.write(text.replace(old, new))
            rc, out = run([LINT, "--root", lr])
            check(rc == 1 and "differs from the block" in out and "lodge_note.py" in out, "%s fails the lint (rc %d)\n%s" % (what, rc, out))
            with open(ap, "w", encoding="utf-8", newline="") as f:
                f.write(text)
        rc, out = run([LINT, "--root", lr])
        check(rc == 0, "restored: the lint passes again (rc %d)\n%s" % (rc, out))
        hb = load(os.path.join(HERE, "check.py"), "check_mod")
        check(any(r.name == "lodge" for r in hb.LINT_ROWS), "check.py lists the `lodge` row")

        # ---- the migration
        mr = os.path.join(tmp, "migrepo")
        rows = [
            "0x82331BC0 | C | one zebra | evidence: decomp sub_82331BC0 +0x20 | seam=zebra ours=src/z.h:2 verdict=match",
            "0x82331BC0 | C | two zebra | evidence: asm sub_82331BC0+0x24..0x30; decomp sub_82340000 +0x8",
            "0x82331BC0 | C | three zebra | evidence: decomp sub_82331BC0 +2C, +30",
            "0x82331BC0 | C | four zebra | evidence: decomp sub_82340000 +0x8",
            "0x82331BC0 | C | five zebra | evidence: decomp sub_82331BC0 +10..+40",
            "0x82331BC0 | C | six zebra | evidence: decomp sub_82331BC0 +0x21",
            "0x82331BC0 | C | seven zebra | evidence: decomp sub_82331BC0 +0x400",
            "0x82331BC0 | C | eight zebra | evidence: decomp sub_82331BC0 +0x28 | role sub_82331BC0",
            "0x836F38D0 | L | nine zebra | evidence: field read sub_82331BC0 +0x20",
        ]
        mp = write(mr, "docs/research/s999/MIG.md", note(rows), crlf=True)
        git(mr, "init", "-q")
        git(mr, "add", "docs")
        git(mr, "commit", "-q", "-m", "n")
        rc, out = run([MIGRATE, "--root", mr, "--resolver-table", table, "--dry"])
        check(rc == 0 and "3 rows typed in 1 notes, 2 skipped" in out and open(mp, "rb").read().decode().count("site sub_") == 0,
              "--dry reports 3 typed and 2 skipped and writes nothing (rc %d)\n%s" % (rc, out))
        rc, out = run([MIGRATE, "--root", mr, "--resolver-table", table])
        text = open(mp, "rb").read().decode("utf-8")
        lines = text.split("\r\n")
        row = [l for l in lines if "| site sub_" in l]
        check(rc == 0 and "3 rows typed in 1 notes, 2 skipped" in out and len(row) == 3, "the migration types 3 rows (rc %d)\n%s\n%s" % (rc, out, row))
        check(any("one zebra | evidence: decomp sub_82331BC0 +0x20 | site sub_82331BC0+0x20 | seam=zebra" in l for l in lines),
              "the TYPE column goes right after the evidence, before the seam fields")
        check(any("two zebra" in l and "| site sub_82331BC0+0x24 |" not in l and l.endswith("site sub_82331BC0+0x24") for l in lines) or
              any("two zebra" in l and "site sub_82331BC0+0x24" in l for l in lines), "a range's first own offset is the site")
        check(any("three zebra" in l and l.endswith("site sub_82331BC0+0x2C") for l in lines), "a bare offset with a hex letter is hex")
        check(all(("site" not in l) for l in lines if any(w in l for w in ("four zebra", "five zebra", "six zebra", "seven zebra", "nine zebra"))),
              "another function's offset, a bare all-digit offset, an unaligned and an outside offset stay untyped")
        check(text.count("\r\n") == text.count("\n"), "CRLF line endings are kept")
        before = text
        rc, out = run([MIGRATE, "--root", mr, "--resolver-table", table])
        check(rc == 0 and "0 rows typed in 0 notes" in out and open(mp, "rb").read().decode() == before, "a second run changes nothing (rc %d)\n%s" % (rc, out))
        rc, out = run([LODGE, mp, "--root", mr, "--resolver-table", table])
        mo = json.load(open(os.path.join(mr, "db", "claims", "s999_MIG.json"), encoding="utf-8"))
        check(rc == 0 and [o.get("site") for o in mo[:3]] == ["0x82331BE0", "0x82331BE4", "0x82331BEC"],
              "the relodged file carries the migrated sites: %s (rc %d)\n%s" % ([o.get("site") for o in mo[:3]], rc, out))

        # ---- the embed normalisation (the fixture's TableFacts)
        ix = fk.TableFacts(TABLE["facts"])
        got = fk.normalise(ix, 0x825A3908, -976)
        check(len(got) == 1 and got[0]["vt"] == 0x8201E6E8 and got[0]["off"] == 224 and got[0]["embed_off"] == 1200,
              "[in_r3-976] of a slot of the inner type normalises to outer +224: %s" % got)
        check(fk.normalise(ix, 0x82331BC0, -976) == [], "a function that is no slot of an embedded type has no normal form")
        rows_t = [dict(objs[3], file="x.json")]
        fl = fk.facts_for_field(rows_t, 0x8201E6E8, 5076, ix)
        check(len(fl) == 1, "facts_for_field finds the field row by its key")
        emb = [{"kind": "field", "key": "vt:0x8201E6D8-976", "claim": "c", "mark": "C", "note": "n.md"}]
        check(fk.facts_for_field(emb, 0x8201E6E8, 224, ix) == emb, "a claim keyed on the inner type is found from the outer (+224 = inner -976)")

        # ---- derive_ctor through the real listing reader (review: the walk-back skipped writers outside its list, calls and joins)
        check(derive_cases(fk, tmp) == [1200, 8], "derive_ctor keeps only the vptr stores nothing can have changed: %s" % derive_cases(fk, tmp))

        # ---- the note is the only source (main session decision): relodge identity, the lint's direction, the pull
        pull_cases(tmp, table)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("typed_lodge_selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
