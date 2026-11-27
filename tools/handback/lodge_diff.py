#!/usr/bin/env python3
r"""S111 LANDLODGE: lodge EVERY note a branch changed that carries a ```claims block, not only the landing note.

  py -I tools\handback\lodge_diff.py [--base REF] [--root DIR] [--skip NOTE_REL ...] [--dry]

A lane that edits ANOTHER note's claims block (a correction, a refuted mark) leaves that note's db\claims\<sNNN>_<NOTE>.json stale when only
its own note is lodged, and check.py's `lodge` row goes red for the lane and the tip. This lists the .md files that differ from --base
(default: the fork point from the SESSION branch (local branch named s<digits>, an ancestor of HEAD, fewest commits behind HEAD); never
main, whose merge-base is days old on a session branch and would re-lodge hundreds of unrelated notes; none found = refuse, name --base;
committed, modified and untracked, in the --root checkout), keeps the ones holding a
```claims block, and runs lodge_note.py on each (--note-as = its repo-relative path). The first note that fails to lodge fails the run
(exit 1) with that note named; the others are still tried and named. Prints `changed: PATH` for each file a lodge wrote (lodge_note's own line).
The reviewer runs this after editing notes (instead of lodge_note.py NOTE per note); land_note.py calls lodge_changed() for the notes
its sites edited. Stdlib only; 0 game runs."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LODGE = os.path.join(HERE, "lodge_note.py")
CLAIMS_FENCE = re.compile(r"^```claims\s*$", re.M)


def _git(root, *args):
    r = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout.splitlines() if r.returncode == 0 else []


SESSION_BRANCH = re.compile(r"^s\d+$")


def default_base(root):
    """-> merge-base of HEAD with the NEWEST session branch (the local branch s<N> with the highest N that shares history with HEAD), or
    None when no such branch exists. Never main. Not "an ancestor of HEAD": the session branch moves on after a lane merges it in, and an
    ancestor test would then fall back to the previous session's branch and re-lodge that whole session's notes (main session, CLEANUP3 review)."""
    best = None
    for b in _git(root, "for-each-ref", "--format=%(refname:short)", "refs/heads"):
        b = b.strip()
        if not SESSION_BRANCH.match(b):
            continue
        sha = _git(root, "merge-base", b, "HEAD")
        if not sha:
            continue
        n = int(b[1:])
        if best is None or n > best[0]:
            best = (n, sha[0].strip())
    return best[1] if best else None


def changed_notes(root, base):
    """-> the sorted repo-relative .md paths that differ from BASE in ROOT (committed, modified, untracked) and hold a ```claims block."""
    names = set(_git(root, "diff", "--name-only", base))
    names |= set(_git(root, "ls-files", "--others", "--exclude-standard"))
    out = []
    for n in sorted(x.strip().replace("\\", "/") for x in names if x.strip()):
        if not n.endswith(".md") or n.startswith(("game_data/", "out/")):
            continue
        try:
            with open(os.path.join(root, *n.split("/")), "rb") as f:
                text = f.read().decode("utf-8", "replace")
        except OSError:
            continue   # deleted in the branch
        if CLAIMS_FENCE.search(text):
            out.append(n)
    return out


def lodge_changed(root, base, say=print, skip=(), extra=(), dry=False):
    """Lodges every changed note with a claims block except those in SKIP. -> (changed files, [(note, exit code)] of the failures)."""
    changed, failed = [], []
    for n in changed_notes(root, base):
        if n in skip:
            continue
        cmd = [sys.executable, "-I", LODGE, os.path.join(root, *n.split("/")), "--root", root, "--note-as", n, *extra]
        if dry:
            cmd.append("--dry")
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
        lines = (r.stdout + r.stderr).splitlines()
        for ln in lines:
            say("lodge_diff: [%s] %s" % (n, ln))
        if r.returncode:
            failed.append((n, r.returncode))
        changed += [x[len("changed: "):].strip() for x in lines if x.startswith("changed: ")]
    return changed, failed


def main(argv=None):
    ap = argparse.ArgumentParser(description="lodge every changed note that carries a claims block")
    ap.add_argument("--base", default=None, help="default: the fork point of HEAD from its session branch s<N> (never main); refused when none")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--skip", action="append", default=[])
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--resolver-table", default=None, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    base = a.base
    if not base:
        base = default_base(root)
        if not base:
            print("lodge_diff: no session branch (local branch s<N>, an ancestor of HEAD) found in %s, so no base can be chosen (main would re-lodge every old note). Next: pass --base REF (the commit your lane was cut from)." % root)
            return 2
    extra = ["--resolver-table", os.path.abspath(a.resolver_table)] if a.resolver_table else []
    changed, failed = lodge_changed(root, base, print, set(a.skip), extra, a.dry)
    for n, rc in failed:
        print("lodge_diff: NOT LODGED: %s (lodge_note exit %d). Next: fix the claim rows named above in that note, then run again." % (n, rc))
    print("lodge_diff: %d file(s) changed, %d note(s) failed" % (len(changed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
