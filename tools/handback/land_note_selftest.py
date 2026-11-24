#!/usr/bin/env python3
r"""Selftest of tools\handback\land_note.py. Everything happens in a scratch git repository made in a temp folder (NOT this repository).

  py -I tools\handback\land_note_selftest.py

STUBBED: only the quick compile (--compile-cmd points at a one-line python script that exits 0): the scratch repo has no
tools\compile\game_syntax.py and no game. prove.py is the REAL one (its commands are `py -I -c "..."` one-liners).
Planted twins (the selftest must catch both): (a) a landing that moves the main checkout's HEAD / edits a tracked file: main_changed()
must report it; (b) a commit that holds a file the note did not touch: check_commit_files() must report it. Each twin is also built for
real in the scratch repo (a real commit in main, a real two-file commit on a branch) so the check reads git, not a made-up value.
Last line: `land_note_selftest: OK` or `land_note_selftest: FAIL (n)`."""
import contextlib
import importlib.util
import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def check(cond, what):
    print("%s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


def git(cwd, *a):
    r = subprocess.run(["git", *a], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        raise RuntimeError("git %s: %s" % (" ".join(a), r.stderr))
    return r.stdout


def w(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def rmtree(p):
    def onerr(fn, path, exc):
        os.chmod(path, stat.S_IWRITE)
        fn(path)
    shutil.rmtree(p, onerror=onerr)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


PASS_CMD = 'py -I -c "import sys; sys.exit(0)"'
FAIL_CMD = 'py -I -c "import sys; sys.exit(1)"'


def note(title, edit_path="src/a.h", cur="int a = 1;", new="int a = 2;", proof=PASS_CMD, extra_path="tools/new.txt"):
    t = "# %s\n\nprose\n\n## Sites the build will touch\n\n" % title
    t += "### Site 1: %s:3\n```current\n%s\n```\n```new\n%s\n```\n\n" % (edit_path, cur, new)
    t += "### Site 2: %s\n```file\nhello\n```\n\n" % extra_path
    if proof:
        t += "```proof\n%s\n```\n" % proof
    return t


def run_land(ln, repo, args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ln.main(args + ["--repo", repo])
    return rc, buf.getvalue()


def run_land_status(ln, repo, branch):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ln.main(["--full-compile-status", "--branch", branch, "--repo", repo])
    return rc, buf.getvalue()


def main():
    ln = load("land_note")
    tmp = tempfile.mkdtemp(prefix="land_note_selftest_")
    try:
        repo = os.path.join(tmp, "repo")
        os.makedirs(repo)
        git(repo, "init", "-q", "-b", "main")
        git(repo, "config", "user.email", "t@example.invalid")
        git(repo, "config", "user.name", "t")
        w(os.path.join(repo, ".gitignore"), "out/\n")
        w(os.path.join(repo, "src", "a.h"), "// h\n// h\nint a = 1;\n")
        w(os.path.join(repo, "tools", "keep.txt"), "keep\n")
        git(repo, "add", "-A")
        git(repo, "commit", "-q", "-m", "base")
        stub = os.path.join(tmp, "compile_stub.py")
        w(stub, "import sys\nsys.exit(0)\n")
        notes = os.path.join(repo, "docs")
        base_args = ["--compile-cmd", stub]

        def snap():
            return git(repo, "rev-parse", "HEAD").strip(), git(repo, "status", "--porcelain"), open(os.path.join(repo, "src", "a.h")).read()

        # --- 1. a good note lands
        n1 = os.path.join(notes, "n1.md")
        w(n1, note("Good landing"))
        before = snap()
        git(repo, "status", "--porcelain")
        rc, out = run_land(ln, repo, [n1, "--branch", "s-good"] + base_args)
        after = snap()
        check(rc == 0, "good note: exit 0 (got %d)" % rc)
        if rc:
            print(out)
        check(before == after, "good note: main checkout HEAD / status / a.h equal before and after")
        names = set(git(repo, "show", "--name-only", "--format=", "s-good").split())
        check(names == {"src/a.h", "tools/new.txt", "docs/n1.md"}, "good note: the commit holds exactly the touched files + the note (%s)" % sorted(names))
        check("quick compile: PASS" in out and "applied: 2 sites (1 edits, 1 new files)" in out, "good note: the final block counts and compile line")
        check("prove: 1 passed, 0 failed" in out, "good note: the ```proof block was read and run by prove.py")
        check(git(repo, "log", "-1", "--format=%B", "s-good").strip().endswith("Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"), "good note: commit trailer")
        check(git(repo, "log", "-1", "--format=%s", "s-good").strip() == "S110 LAND s-good: Good landing", "good note: commit subject")
        wtp = os.path.join(repo, "out", "wt", "land_s-good")
        check(os.path.isdir(wtp) and os.path.exists(wtp + ".owner") and "pid=" in open(wtp + ".owner").read(), "good note: worktree under out\\wt with its .owner pid file")

        # --- 2. a bad anchor: exit 2, nothing committed
        n2 = os.path.join(notes, "n2.md")
        w(n2, note("Bad anchor", cur="int a = 99;"))
        rc, out = run_land(ln, repo, [n2, "--branch", "s-bad"] + base_args)
        check(rc == 2, "bad anchor: exit 2 (got %d)" % rc)
        check("NOT FOUND" in out, "bad anchor: note_sites' lines are printed")
        ahead = git(repo, "rev-list", "--count", "main..s-bad").strip()
        check(ahead == "0", "bad anchor: nothing committed on the branch")

        # --- 3. a path under out/ is refused (exit 4), nothing created
        n3 = os.path.join(notes, "n3.md")
        w(n3, note("Out path", extra_path="out/x.txt"))
        rc, out = run_land(ln, repo, [n3, "--branch", "s-out"] + base_args)
        check(rc == 4 and "out/x.txt" in out, "out/ path: exit 4 naming the path (got %d)" % rc)
        check(subprocess.run(["git", "rev-parse", "-q", "--verify", "refs/heads/s-out"], cwd=repo, capture_output=True).returncode != 0, "out/ path: no branch created")
        n3b = os.path.join(notes, "n3b.md")
        w(n3b, note("Generated path", extra_path="generated/foo.cpp"))
        rc, out = run_land(ln, repo, [n3b, "--branch", "s-gen"] + base_args)
        check(rc == 4, "generated/ path: exit 4 (got %d)" % rc)

        # --- 4. an existing branch is refused
        rc, out = run_land(ln, repo, [n1, "--branch", "s-good"] + base_args)
        check(rc == 1 and "REFUSED" in out and "exists" in out, "existing branch: refused (got %d)" % rc)

        # --- 5. --amend after correcting the note lands a second commit
        w(n2, note("Bad anchor, corrected"))
        rc, out = run_land(ln, repo, [n2, "--branch", "s-bad", "--amend"] + base_args)
        check(rc == 0, "amend: exit 0 (got %d)" % rc)
        if rc:
            print(out)
        check(git(repo, "rev-list", "--count", "main..s-bad").strip() == "1", "amend: the branch now holds one new commit")
        names = set(git(repo, "show", "--name-only", "--format=", "s-bad").split())
        check(names == {"src/a.h", "tools/new.txt", "docs/n2.md"}, "amend: the commit holds the touched files + the note (%s)" % sorted(names))
        w(n2, note("Bad anchor, corrected twice", new="int a = 3;"))
        rc, out = run_land(ln, repo, [n2, "--branch", "s-bad", "--amend"] + base_args)
        check(rc == 0 and git(repo, "rev-list", "--count", "main..s-bad").strip() == "2", "amend again: a further commit (rc %d)" % rc)
        check(git(repo, "show", "s-bad:src/a.h").strip().endswith("int a = 3;"), "amend again: the file holds the corrected edit")

        # --- 6. a failing proof command: exit 1 and its log path; no proof: exit 5 / --keep-going 0
        n6 = os.path.join(notes, "n6.md")
        w(n6, note("Failing proof", proof=FAIL_CMD))
        rc, out = run_land(ln, repo, [n6, "--branch", "s-pf"] + base_args)
        check(rc == 1 and "failed command log:" in out and "prove: 0 passed, 1 failed" in out, "failing proof: exit 1, summary and log path in the final block (rc %d)" % rc)
        n7 = os.path.join(notes, "n7.md")
        w(n7, note("No proof", proof=None))
        rc, out = run_land(ln, repo, [n7, "--branch", "s-np"] + base_args)
        check(rc == 5 and "NO PROOF RUN" in out, "no proof: NO PROOF RUN, exit 5 (got %d)" % rc)
        rc, out = run_land(ln, repo, [os.path.join(notes, "n7.md"), "--branch", "s-np2", "--keep-going"] + base_args)
        check(rc == 0 and "NO PROOF RUN" in out, "no proof + --keep-going: loud, exit 0 (got %d)" % rc)
        pf = os.path.join(tmp, "p.txt")
        w(pf, PASS_CMD + "\n")
        rc, out = run_land(ln, repo, [os.path.join(notes, "n7.md"), "--branch", "s-np3", "--proof", pf] + base_args)
        check(rc == 0 and "prove: 1 passed" in out, "--proof FILE is run (rc %d)" % rc)

        # --- 7. the full compile (a `compile` line of the proof) runs detached after the commit
        import time
        slow = os.path.join(tmp, "full_slow.py")
        w(slow, "import time\nprint('stub full compile start')\ntime.sleep(3)\nprint('stub full compile end')\n")
        bad = os.path.join(tmp, "full_bad.py")
        w(bad, "import sys, time\ntime.sleep(1)\nprint('src/a.h(3): error C9999: boom')\nsys.exit(1)\n")
        n8 = os.path.join(notes, "n8.md")
        w(n8, note("Full compile", proof="compile\n" + PASS_CMD))
        t0 = time.time()
        rc, out = run_land(ln, repo, [n8, "--branch", "s-fc", "--full-compile-cmd", slow] + base_args)
        took = time.time() - t0
        st_rc, st_out = run_land_status(ln, repo, "s-fc")
        check(rc == 0 and "full compile: running in background, log" in out, "full compile: the landing returns with 'running in background' (rc %d)" % rc)
        check(st_rc == 2 and "running" in st_out, "full compile: status reads running right after the landing (rc %d, %s)" % (st_rc, st_out.strip()[:60]))
        for _ in range(40):
            time.sleep(0.5)
            st_rc, st_out = run_land_status(ln, repo, "s-fc")
            if st_rc != 2:
                break
        check(st_rc == 0 and "PASS" in st_out, "full compile: status reads PASS once it ended (rc %d)" % st_rc)
        check("prove: 1 passed" in out, "full compile: the other proof commands ran blocking (the compile line left the proof)")
        n9 = os.path.join(notes, "n9.md")
        w(n9, note("Full compile fails", proof="compile"))
        rc, out = run_land(ln, repo, [n9, "--branch", "s-fcb", "--full-compile-cmd", bad] + base_args)
        check(rc == 0 and "running in background" in out, "failing full compile: the landing itself is not held up (rc %d)" % rc)
        for _ in range(40):
            time.sleep(0.5)
            st_rc, st_out = run_land_status(ln, repo, "s-fcb")
            if st_rc != 2:
                break
        check(st_rc == 1 and "FAIL" in st_out and "error C9999: boom" in st_out, "failing full compile: status reads FAIL with its error line (rc %d: %r)" % (st_rc, st_out))
        n10 = os.path.join(notes, "n10.md")
        w(n10, note("Full compile blocking", proof="compile"))
        rc, out = run_land(ln, repo, [n10, "--branch", "s-fcblock", "--full-compile", "block", "--full-compile-cmd", bad] + base_args)
        check(rc == 1 and "full compile: FAIL (exit 1)" in out and "failed command log:" in out, "--full-compile block: waits and fails the landing (rc %d)" % rc)

        # --- 8. a planted tool fault: the landing edits a touched path in main's WORKING TREE: FATAL, exit 6
        fault = os.path.join(tmp, "prove_fault.py")
        w(fault, "open(%r, 'w').write('// damaged by the tool\\n')\n" % os.path.join(repo, "src", "a.h"))
        n11 = os.path.join(notes, "n11.md")
        w(n11, note("Planted fault"))
        rc, out = run_land(ln, repo, [n11, "--branch", "s-fault", "--prove-cmd", fault] + base_args)
        check(rc == 6 and "FATAL" in out and "src/a.h" in out and "Next:" in out, "planted fault: a touched path edited in main is FATAL exit 6 (rc %d)" % rc)
        git(repo, "checkout", "--", "src/a.h")

        # --- 9. someone else commits in main during the landing (HEAD moves, an unrelated file, AND a touched file with other content): exit 0
        other = os.path.join(tmp, "prove_other.py")
        w(other, "import subprocess\nr = %r\n"
                 "open(r + '/tools/other.txt', 'w').write('other\\n')\n"
                 "open(r + '/src/a.h', 'w').write('// h\\n// h\\nint a = 7;\\n')\n"
                 "subprocess.run(['git', 'add', 'tools/other.txt', 'src/a.h'], cwd=r, check=True)\n"
                 "subprocess.run(['git', 'commit', '-q', '-m', 'someone else'], cwd=r, check=True)\n" % repo)
        n12 = os.path.join(notes, "n12.md")
        w(n12, note("Someone else commits"))
        h_before = git(repo, "rev-parse", "HEAD").strip()
        rc, out = run_land(ln, repo, [n12, "--branch", "s-other", "--prove-cmd", other] + base_args)
        h_after = git(repo, "rev-parse", "HEAD").strip()
        check(h_before != h_after, "someone else: main's HEAD really moved during the landing")
        check(rc == 0, "someone else: the landing is exit 0 (rc %d)" % rc)
        if rc:
            print(out)
        check("land_note: note: main moved %s -> %s during the landing (someone else's commits)" % (h_before[:10], h_after[:10]) in out,
              "someone else: ONE informational line names the move")
        check("FATAL" not in out, "someone else: no FATAL")

        # --- 10. a note with `No sites:` has nothing to land: exit 7, nothing created
        n13 = os.path.join(notes, "n13.md")
        w(n13, "# Nothing\n\n## Sites the build will touch\n\nNo sites: the code already does this\n")
        rc, out = run_land(ln, repo, [n13, "--branch", "s-nosites"] + base_args)
        check(rc == 7 and "NOTHING TO LAND" in out and "Next:" in out, "no sites: exit 7 with its message (rc %d)" % rc)
        check(subprocess.run(["git", "rev-parse", "-q", "--verify", "refs/heads/s-nosites"], cwd=repo, capture_output=True).returncode != 0, "no sites: no branch created")

        # --- planted twin (a): what the tool could damage in main's working tree (real edit, then a real commit)
        ln.MAIN_ROOT = repo
        h0 = git(repo, "rev-parse", "HEAD").strip()
        before = ln.snap_paths(repo, ["tools/keep.txt", "tools/absent.txt"])
        check(ln.damaged_paths(repo, before, h0, h0) == [], "twin (a): untouched paths read as untouched")
        w(os.path.join(repo, "tools", "keep.txt"), "edited by a bad landing\n")
        check(ln.damaged_paths(repo, before, h0, h0) == ["tools/keep.txt"], "twin (a): a working-tree edit of a touched path is caught")
        git(repo, "add", "tools/keep.txt")
        git(repo, "commit", "-q", "-m", "twin: someone commits it")
        h1 = git(repo, "rev-parse", "HEAD").strip()
        check(ln.damaged_paths(repo, before, h0, h1) == [], "twin (a): the same edit committed by someone (HEAD moved, content = new blob) is fine")
        w(os.path.join(repo, "tools", "keep.txt"), "edited again after the commit\n")
        check(ln.damaged_paths(repo, before, h0, h1) == ["tools/keep.txt"], "twin (a): an edit that differs from the new HEAD's blob is still caught")
        git(repo, "checkout", "--", "tools/keep.txt")
        try:
            ln.git(repo, "commit", "-q", "--allow-empty", "-m", "x")
            check(False, "twin (a): the tool's git() refuses a commit with main as its working directory")
        except RuntimeError:
            check(True, "twin (a): the tool's git() refuses a commit with main as its working directory")
        check(ln.git(repo, "status", "--porcelain").returncode == 0, "twin (a): a read-only query in main is allowed")

        # --- planted twin (b): a commit holding a file the note did not touch
        git(repo, "checkout", "-q", "-b", "twin-b")
        w(os.path.join(repo, "src", "a.h"), "// h\n")
        w(os.path.join(repo, "stray.c"), "int x;\n")
        git(repo, "add", "src/a.h", "stray.c")
        git(repo, "commit", "-q", "-m", "twin b")
        held = git(repo, "show", "--name-only", "--format=", "HEAD").split()
        check(ln.check_commit_files(held, ["src/a.h"]) == ["stray.c"], "twin (b): a stray file in the commit is caught")
        check(ln.check_commit_files(["src/a.h"], ["src/a.h", "docs/n.md"]) == [], "twin (b): a commit within the note's files is fine")
    finally:
        try:
            rmtree(tmp)
        except OSError as e:
            print("(could not remove %s: %s)" % (tmp, e))
    print("land_note_selftest: %s" % ("OK" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
