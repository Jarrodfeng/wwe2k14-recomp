#!/usr/bin/env python3
r"""S110 NOTE_SITES selftest: py -I tools\handback\note_sites_selftest.py
Builds scratch trees in a temp folder, runs tools\handback\note_sites.py on them and proves the check / apply rules; then runs the same cases
against two planted twins of the tool (a check that accepts 2 matches; an apply that goes on when a site failed): each must be caught.
Last line: `selftest: OK` or `selftest: FAIL`. 0 game runs."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "note_sites.py")

A_H = "// a.h\nint x = 1;\nint y = 2;\nint z = 3;\n"
DUP_H = "int x = 1;\nint q = 0;\nint x = 1;\n"
B_CPP = "void f() {\r\n  go(1);\r\n  go(2);\r\n}\r\n"


def note(*sections):
    return "# Note\n\nprose\n\n## Sites the build will touch\n\nsome prose, ignored\n\n" + "\n".join(sections) + "\n\n## Proof\n\n```\nnot a site\n```\n"


def edit(path, cur, new, fence="```"):
    return "### Site 1: %s:12\n%scurrent\n%s\n%s\n%snew\n%s\n%s\n" % (path, fence, cur, fence, fence, new, fence)


def newfile(path, body, fence="```"):
    return "### Site 9: %s\n%sfile\n%s\n%s\n" % (path, fence, body, fence)


def make_tree(root):
    os.makedirs(os.path.join(root, "src"), exist_ok=True)
    for name, data in (("a.h", A_H), ("dup.h", DUP_H), ("b.cpp", B_CPP)):
        with open(os.path.join(root, "src", name), "wb") as f:
            f.write(data.encode())


def tree_hash(root):
    h = hashlib.sha256()
    for dp, dn, fn in sorted(os.walk(root)):
        dn.sort()
        h.update(dp[len(root):].encode())
        for n in sorted(fn):
            h.update(n.encode())
            with open(os.path.join(dp, n), "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def rd(root, rel):
    with open(os.path.join(root, rel), "rb") as f:
        return f.read()


def run_cases(tool):
    fails = []
    tmp = tempfile.mkdtemp(prefix="note_sites_st_")

    def case(name):
        root = os.path.join(tmp, name)
        os.makedirs(root)
        make_tree(root)
        return root

    def go(root, mode, text, nm="note.md"):
        np = os.path.join(tmp, nm)
        with open(np, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        p = subprocess.run([sys.executable, "-I", tool, mode, np, "--root", root], capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    def expect(name, cond, detail=""):
        if not cond:
            fails.append("%s %s" % (name, detail))

    try:
        # 1 a good note: LF edit, an insertion, a CRLF edit, a new file in new folders
        r = case("good")
        good = note(
            edit("src/a.h", "int y = 2;\nint z = 3;", "int y = 20;\nint z = 3;")
            + "```current\n// a.h\n```\n```new\n// a.h\n// inserted\n```\n",
            edit("src/b.cpp", "  go(1);\n  go(2);", "  go(1);\n  go(22);\n  go(3);"),
            newfile("src/new/deep/n.cpp", "int n() { return 1; }"))
        rc, out = go(r, "--check", good)
        expect("good check", rc == 0 and out.splitlines()[-1] == "note_sites: 3 sites, 3 edits, 1 new files: OK", out)
        expect("good check changed nothing", not os.path.exists(os.path.join(r, "src", "new")))
        rc, out = go(r, "--apply", good)
        expect("good apply", rc == 0 and "changed:" in out, out)
        expect("good a.h", rd(r, "src/a.h") == b"// a.h\n// inserted\nint x = 1;\nint y = 20;\nint z = 3;\n", repr(rd(r, "src/a.h")))
        b = rd(r, "src/b.cpp")
        expect("CRLF kept", b == b"void f() {\r\n  go(1);\r\n  go(22);\r\n  go(3);\r\n}\r\n", repr(b))
        expect("new file with folders", rd(r, "src/new/deep/n.cpp") == b"int n() { return 1; }\n")
        # 2 second apply is a no-op
        h = tree_hash(r)
        rc, out = go(r, "--apply", good)
        expect("second apply no-op", rc == 0 and "already applied" in out and "changed:" not in out and tree_hash(r) == h, out)
        rc, out = go(r, "--check", good)
        expect("check after apply", rc == 0 and "already applied" in out, out)

        # 3 an anchor that matches twice
        r = case("ambig")
        rc, out = go(r, "--check", note(edit("src/dup.h", "int x = 1;", "int x = 9;")))
        expect("ambiguous", rc == 1 and "AMBIGUOUS (2 matches)" in out, out)
        h = tree_hash(r)
        rc, out = go(r, "--apply", note(edit("src/dup.h", "int x = 1;", "int x = 9;")))
        expect("ambiguous apply refused", rc == 1 and tree_hash(r) == h, out)

        # 4 a wrong character
        r = case("nf")
        rc, out = go(r, "--check", note(edit("src/a.h", "int y = 2;\nint z = 4;", "int y = 7;\nint z = 4;")))
        expect("not found", rc == 1 and "NOT FOUND" in out and "column 9" in out and "file line 4" in out, out)

        # 5 one site fails: nothing changes, not even the good site or the new file
        r = case("refuse")
        h = tree_hash(r)
        bad = note(edit("src/a.h", "int y = 2;", "int y = 5;"), newfile("src/n2.cpp", "x"), edit("src/b.cpp", "go(99);", "go(100);"))
        rc, out = go(r, "--apply", bad)
        expect("apply refused", rc == 1 and "REFUSED" in out and tree_hash(r) == h, out)

        # 6 a four-backtick fence carries triple backticks
        r = case("fence")
        md = "Use:\n```cpp\nint a;\n```\n"
        fenced = note(newfile("docs/x.md", md.rstrip("\n"), "````"), edit("src/a.h", "int y = 2;", "int y = 2;\n```\n// fenced", "````"))
        rc, out = go(r, "--apply", fenced)
        expect("four-backtick apply", rc == 0, out)
        expect("four-backtick file", rd(r, "docs/x.md") == md.encode(), repr(rd(r, "docs/x.md")))
        expect("four-backtick edit", b"int y = 2;\n```\n// fenced\n" in rd(r, "src/a.h"), repr(rd(r, "src/a.h")))

        # 7 format errors
        r = case("errs")
        rc, out = go(r, "--check", "# Note\n\nno section\n")
        expect("section missing", rc == 1 and "not found" in out, out)
        rc, out = go(r, "--check", note("### Site 1: src/a.h\njust prose\n"))
        expect("site without block", rc == 1 and "no ```current" in out, out)
        rc, out = go(r, "--check", note("### Site 1: src/a.h\n```current\n```\n```new\nx\n```\n"))
        expect("empty current", rc == 1 and "empty" in out, out)
        rc, out = go(r, "--check", note(newfile("src/a.h", "other")))
        expect("existing file differs", rc == 1 and "already exists" in out, out)
        rc, out = go(r, "--check", note(newfile("../escape.txt", "x")))
        expect("path escape", rc == 1 and "not repo-relative" in out, out)

        # 8 `No sites: <reason>`: nothing to change
        r = case("nosites")
        h = tree_hash(r)
        ns = "# Note\n\n## Sites the build will touch\n\nno SITES: the code already does it\n\n## Proof\n"
        rc, out = go(r, "--check", ns)
        expect("no sites ok", rc == 0 and out.strip() == "note_sites: 0 sites: OK (no sites: the code already does it)", out)
        rc, out = go(r, "--apply", ns)
        expect("no sites apply", rc == 0 and tree_hash(r) == h and "nothing to apply" in out, out)
        rc, out = go(r, "--check", "# N\n\n## Sites the build will touch\n\nNo sites:   \n")
        expect("no sites empty reason", rc == 1 and "needs a reason" in out, out)
        rc, out = go(r, "--check", "# N\n\n## Sites the build will touch\n\nNo sites: because\n\n" + edit("src/a.h", "int y = 2;", "int y = 5;"))
        expect("no sites with a site heading", rc == 1 and "together with" in out, out)
        rc, out = go(r, "--check", "# N\n\nNo sites: outside the section\n\n## Sites the build will touch\n\nprose\n")
        expect("no sites outside the section is not the form", rc == 1 and "no '### Site" in out, out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def make_twin(tmp, marker, old, new):
    with open(TOOL, encoding="utf-8") as f:
        src = f.read()
    lines = src.split("\n")
    hit = [i for i, ln in enumerate(lines) if marker in ln]
    assert len(hit) == 1 and old in lines[hit[0]], "twin marker %s not found once" % marker
    lines[hit[0]] = lines[hit[0]].replace(old, new)
    p = os.path.join(tmp, "twin_%s.py" % marker.split(":")[1])
    with open(p, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return p


def main():
    bad = 0
    fails = run_cases(TOOL)
    print("tool: %d failing checks" % len(fails))
    for f in fails:
        print("  FAIL " + f.replace("\n", "\n       ")[:600])
    bad += len(fails)
    tmp = tempfile.mkdtemp(prefix="note_sites_twin_")
    try:
        for marker, old, new, what in (("TWIN:ambiguous", "count > 1", "count > 1000", "a check that accepts 2 matches"),
                                       ("TWIN:refuse", "not all_ok", "False", "an apply that goes on when a site failed")):
            twin = make_twin(tmp, marker, old, new)
            tf = run_cases(twin)
            print("twin (%s): %s (%d checks failed)" % (what, "CAUGHT" if tf else "NOT CAUGHT", len(tf)))
            if not tf:
                bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest: %s" % ("OK" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
