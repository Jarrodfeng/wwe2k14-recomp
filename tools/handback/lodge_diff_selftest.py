#!/usr/bin/env python3
r"""S111 CLEANUP3: lodge_diff's default base is the fork point from the SESSION branch, never main.

  py -I tools\handback\lodge_diff_selftest.py

A planted repo: main holds an old note, the session branch s7 adds two more notes (the "days old" ones), the lane branch adds one.
The default base must list only the lane's note; the twin (base = main, the old behaviour) lists the old ones too and must be seen to
differ; a repo with no session branch must give no base (lodge_diff then refuses and names --base). 0 game runs."""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def load():
    spec = importlib.util.spec_from_file_location("lodge_diff", os.path.join(HERE, "lodge_diff.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def git(root, *a):
    r = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *a], cwd=root, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("git %s: %s" % (" ".join(a), r.stderr))
    return r.stdout.strip()


def note(root, name):
    p = os.path.join(root, "docs", name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write("# %s\n\n```claims\nrow\n```\n" % name)
    git(root, "add", "docs/" + name)
    git(root, "commit", "-m", name)


def main():
    ld = load()
    tmp = tempfile.mkdtemp(prefix="lodge_diff_st_")
    fails = 0
    try:
        git(tmp, "init", "-b", "main")
        note(tmp, "old_main.md")
        git(tmp, "checkout", "-b", "s7")
        note(tmp, "s7_a.md")
        note(tmp, "s7_b.md")
        git(tmp, "checkout", "-b", "lane")
        note(tmp, "lane.md")
        base = ld.default_base(tmp)
        got = ld.changed_notes(tmp, base)
        ok = got == ["docs/lane.md"]
        print("%s  default base (session branch s7): lodges %s" % ("PASS" if ok else "FAIL", got))
        fails += not ok
        twin = ld.changed_notes(tmp, git(tmp, "merge-base", "HEAD", "main"))
        ok = len(twin) == 3
        print("%s  twin base = main: lodges %d notes (the old ones too: %s); the default must differ" % ("PASS" if ok else "FAIL", len(twin), twin))
        fails += not ok
        git(tmp, "branch", "-m", "s7", "work")
        ok = ld.default_base(tmp) is None
        print("%s  no session branch: no base (lodge_diff refuses, names --base)" % ("PASS" if ok else "FAIL"))
        fails += not ok
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "lodge_diff.py"), "--root", tmp, "--dry"], capture_output=True, text=True)
        ok = r.returncode == 2 and "--base" in r.stdout
        print("%s  main(): exit %d, message names --base" % ("PASS" if ok else "FAIL", r.returncode))
        fails += not ok
    finally:
        def rm(f, p, e):
            os.chmod(p, 0o700)
            f(p)
        shutil.rmtree(tmp, onerror=rm)
    print("lodge_diff_selftest: %s" % ("FAIL (%d)" % fails if fails else "all pass"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
