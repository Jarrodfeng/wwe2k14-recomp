"""split_check: prove a file split moved text and changed nothing.

  py -I tools\\split\\split_check.py FILE [--base REV]     (REV default HEAD~1)
  py -I tools\\split\\split_check.py --selftest

Expands every marked include line (#include "dir/part.inc"  // split: part) of FILE back in place and compares the
result byte for byte with FILE at git revision REV.  Exit 0 only when identical; otherwise prints the first
differing line.  The include is plain text substitution, so identical text means the compiler sees the same tokens.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_unit import expand_bytes, read_unit  # noqa: E402


def first_diff(a, b):
    """None when equal, else a description of the first differing line."""
    if a == b:
        return None
    la, lb = a.splitlines(keepends=True), b.splitlines(keepends=True)
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else None
        y = lb[i] if i < len(lb) else None
        if x != y:
            return "line %d differs\n  expanded: %r\n  base    : %r" % (i + 1, x, y)
    return "texts differ"


def git_show(repo, rev, rel):
    r = subprocess.run(["git", "-C", str(repo), "show", "%s:%s" % (rev, rel)], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("git show %s:%s failed: %s" % (rev, rel, r.stderr.decode(errors="replace").strip()))
    return r.stdout


def check(repo, rel, base):
    # git may hand back LF while the working tree is CRLF (core.autocrlf): compare line-ending-normalised
    now = expand_bytes(Path(repo) / rel).replace(b"\r\n", b"\n")
    old = git_show(repo, base, rel).replace(b"\r\n", b"\n")
    d = first_diff(now, old)
    if d is None:
        return 0, "SAME %d lines" % len(old.splitlines())
    return 1, "DIFFERENT: " + d


def selftest():
    def git(repo, *a):
        subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t", *a],
                       check=True, capture_output=True)

    with tempfile.TemporaryDirectory() as d:
        git(d, "init", "-q")
        git(d, "config", "core.autocrlf", "false")
        body = "".join("  int f%d() { return %d; }\r\n" % (i, i) for i in range(10))
        (Path(d) / "p.cpp").write_bytes(("// head\r\nclass A {\r\n" + body + "};\r\n").encode())
        git(d, "add", "p.cpp")
        git(d, "commit", "-q", "-m", "base")
        lines = (Path(d) / "p.cpp").read_bytes().splitlines(keepends=True)
        (Path(d) / "parts").mkdir()
        (Path(d) / "parts" / "mid.inc").write_bytes(b"".join(lines[4:8]))
        (Path(d) / "p.cpp").write_bytes(b"".join(lines[:4]) + b'#include "parts/mid.inc"  // split: mid\r\n'
                                        + b"".join(lines[8:]))
        ok = True
        rc, msg = check(d, "p.cpp", "HEAD")
        print("good split :", rc, msg)
        ok &= rc == 0
        # the text a consumer reads (read_unit) equals a text-mode read of the old file: the checkout's CRLF is '\n' to it, as it was
        # before the split (a tool's '\n' anchor or `$` regex would otherwise stop matching without an error)
        old = Path(d) / "old.cpp"
        old.write_bytes(git_show(d, "HEAD", "p.cpp"))
        with open(old, encoding="utf-8") as f:
            want = f.read()
        same = read_unit(d, "p.cpp") == want
        print("text read  :", "same as a text-mode read of the old file" if same else "DIFFERENT from a text-mode read of the old file")
        ok &= same
        part = Path(d) / "parts" / "mid.inc"
        part.write_bytes(part.read_bytes().replace(b"return 5", b"return 6"))
        rc, msg = check(d, "p.cpp", "HEAD")
        print("planted twin:", rc, msg.splitlines()[0])
        ok &= rc == 1
        part.write_bytes(part.read_bytes().replace(b"return 6", b"return 5"))
        part.write_bytes(part.read_bytes()[:-2])  # lost line ending
        rc, msg = check(d, "p.cpp", "HEAD")
        print("lost eol    :", rc, msg.splitlines()[0])
        ok &= rc == 1
    print("selftest", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    base = "HEAD~1"
    if "--base" in argv:
        i = argv.index("--base")
        base = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) != 1:
        print(__doc__)
        return 2
    f = Path(argv[0]).resolve()
    repo = subprocess.run(["git", "-C", str(f.parent), "rev-parse", "--show-toplevel"], capture_output=True,
                          text=True).stdout.strip()
    rel = os.path.relpath(str(f), repo).replace("\\", "/")
    rc, msg = check(repo, rel, base)
    print(msg)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
