#!/usr/bin/env python3
r"""cite_line: a GUARD for one rule (CLAUDE.md "Cite our code by name, never by line"; Jarrod 2026-11-27): a live doc or a src / tools
comment names a file of ours and a function / type / cvar / constant / log text, never a line number.

  py -I tools\lint\cite_line_lint.py [--root DIR] [--selftest]

FAILS on every `FILE:N`, `FILE:N-M`, `FILE line(s) N`, bare `FILE N` (C-like / shader files) and `FILE:N, M` follow-on, where FILE is a
tracked file under src\ or tools\, found in
  - the live docs: docs\systems, docs\formats, docs\renderer (not history / archive), docs\ours, STATUS.md, PLAYBOOK.md, docs\LESSONS.md,
    docs\IDEAS.md, .claude\agents\*.md;
  - the COMMENTS of src\ and tools\ code (C-like // and /* */, `#` comments of .py / .ps1 / .cmake / .sh, .py docstrings); strings and code are not read.
Not read: docs\research, docs\history, SESSION*_*.md, db\, a doc whose opening carries `> Dated record (...)` (a session's plan / lane /
measurement: its code references are a snapshot), a file size or count (`FILE 7019 -> 409`, `FILE 6033 lines`, `FILE 1024 bytes`, `FILE 2048x1024`, `FILE 2026-11-27`), an SDK path, `d3_` + `binder.h` (the left
boundary), a name that is no file of ours.  The one-time conversion is tools\split\cite_by_name.py; this lint holds the rule after it.
Exceptions: tools\lint\cite_line_allow.txt, one per line `path | ref | why` (the ref exactly as printed; `#` lines and blanks are skipped).
Every run also feeds planted twins and requires each to fire.  Exit 0 = pass, 1 = a finding, 2 = tree missing.  0 game runs."""
import argparse
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "split"))
sys.path.insert(0, str(HERE.parent / "q"))
import cite_by_name as CB  # noqa: E402

ALLOW = "tools/lint/cite_line_allow.txt"


def load_allow(root):
    out = {}
    p = Path(root) / ALLOW
    if p.is_file():
        for ln in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            parts = [x.strip() for x in ln.split(" | ")]
            if len(parts) < 3 or not all(parts[:3]):
                out[("<bad line>", ln)] = True
            else:
                out[(parts[0].replace("\\", "/"), parts[1])] = True
    return out


def findings(root, opts=None, use_allow=True):
    """[(file, ref text)] of every line citation not allowlisted, and [bad allowlist lines]."""
    allow = load_allow(root) if use_allow else {}
    bad = [k[1] for k in allow if k[0] == "<bad line>"]
    out = [(rel, ref) for rel, ref in CB.list_refs(root, opts=opts) if (rel, ref) not in allow]
    return out, bad


def selftest():
    ok = True
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        for d in ("src/x", "docs/systems", "docs/history", "tools/lint"):
            (root / d).mkdir(parents=True)
        (root / "src/x/big.cpp").write_text("int Fn() {\n  return 1;\n}\n" + "int pad;\n" * 120, encoding="utf-8")
        (root / "src/x/binder.h").write_text("int B;\n", encoding="utf-8")
        (root / "docs/systems/a.md").write_text(
            "bad ref big.cpp:2 here\n"                    # 1 finding
            "allowed big.cpp:3 here\n"                    # allowlisted
            "other d3_binder.h:2 is not binder.h\n"       # the left boundary: no finding
            "a size big.cpp 120 lines, big.cpp 120 -> 4\n"   # no finding
            "more sizes big.cpp 1024 bytes, big.cpp 2048x1024, big.cpp 2026-11-27\n"   # a byte count, a picture size, a date: no finding
            "by name `Fn` (big.cpp) is fine\n", encoding="utf-8")
        (root / "docs/history/old.md").write_text("old big.cpp:2\n", encoding="utf-8")   # not a live doc
        (root / "docs/systems/three.md").write_text("only a bare three-digit ref big.cpp 110 here\n", encoding="utf-8")   # 1 finding: the quick filter must not skip it
        (root / "docs/systems/dated.md").write_text("# T\n\n> Dated record (S101): code references are as of then; find the code by name with q.\n\nbig.cpp:2\n",
                                                    encoding="utf-8")   # a dated record: skipped
        (root / "src/x/c.h").write_text(
            "int v;  // see big.cpp:2\n"                  # comment: 1 finding
            "const char* s = \"big.cpp:2\";\n"            # string: none
            "int w;\n", encoding="utf-8")
        (root / "tools/lint/t.py").write_text('"""doc string big.cpp:1-2 here"""\nx = "big.cpp:2"\n# note big.cpp 5 line\n', encoding="utf-8")   # docstring + comment: 2 findings
        (root / ALLOW).write_text("# why list\ndocs/systems/a.md | big.cpp:3 | the one place the line IS the point\n", encoding="utf-8")
        want = {("docs/systems/a.md", "big.cpp:2"), ("src/x/c.h", "big.cpp:2"), ("tools/lint/t.py", "big.cpp:1-2"), ("docs/systems/three.md", "big.cpp 110")}
        got, bad = findings(root)
        got_set = set(got)
        # the comment `big.cpp 5 line` has no sep word before a number: not a ref
        c0 = got_set == want and not bad
        print("real rules: %d findings (want %d) -> %s" % (len(got), len(want), "ok" if c0 else "WRONG %s" % sorted(got_set)))
        ok &= c0
        twins = [("no allowlist (the allowlisted ref must fire)", dict(use_allow=False), ("docs/systems/a.md", "big.cpp:3")),
                 ("code anywhere (a string literal must fire)", dict(opts=CB.Opts(comments_only=False)), ("tools/lint/t.py", "big.cpp:2")),
                 ("no size rule (a file size must fire)", dict(opts=CB.Opts(size_rule=False)), ("docs/systems/a.md", "big.cpp 120")),
                 ("no size rule (a byte count must fire)", dict(opts=CB.Opts(size_rule=False)), ("docs/systems/a.md", "big.cpp 1024")),
                 ("no size rule (a date must fire)", dict(opts=CB.Opts(size_rule=False)), ("docs/systems/a.md", "big.cpp 2026")),
                 ("dated records read (a `> Dated record` doc must fire)", dict(opts=CB.Opts(dated_skip=False)), ("docs/systems/dated.md", "big.cpp:2"))]
        for label, kw, extra in twins:
            g, _ = findings(root, **kw)
            fired = any(x[0] == extra[0] and x[1].startswith(extra[1]) for x in g)
            print("twin %-52s fires: %s" % (label, "yes" if fired else "NO"))
            ok &= fired
        # a doc under history stays unread even when it is the only file with a ref
        c1 = not any(r.startswith("docs/history") for r, _ in got)
        print("twin docs/history is not read: %s" % ("ok" if c1 else "WRONG"))
        ok &= c1
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    root = Path(a.root).resolve()
    if a.selftest:
        ok = selftest()
        print("selftest " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    if not (root / "src").is_dir() or not (root / "tools").is_dir():
        print("cite_line: no src\\ / tools\\ under %s" % root)
        return 2
    out, bad = findings(root)
    for b in bad:
        print("cite_line: bad allowlist line (want `path | ref | why`): %s" % b)
    for rel, ref in out:
        print("FAIL %s: %s  (cite the file and the NAME, not a line; or an allowlist line with its why)" % (rel, ref))
    # the selftest twins ride every run
    if not selftest():
        print("FAIL cite_line selftest")
        return 1
    if out or bad:
        print("cite_line: %d line citation(s)" % len(out))
        return 1
    print("PASS cite_line: no line citation of our code in the live docs or the src / tools comments")
    return 0


if __name__ == "__main__":
    sys.exit(main())
