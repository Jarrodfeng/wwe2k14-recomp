#!/usr/bin/env python3
r"""S110 LANDNOTE: land a settle note's carried edits on its OWN branch in its OWN worktree, compile, commit, prove, and report.

  py -I tools\handback\land_note.py NOTE --branch NAME [--base REF] [--proof FILE] [--keep-going] [--amend]

For a probe (which runs in the MAIN checkout and must never edit it) or the main session. The sequence is fixed and needs no model:
  1. refuse when the branch NAME or the worktree folder exists (remove a stale worktree with the tidy, py tools\harness\tidy.py; never
     a forced git worktree removal); a note that names a path under game_data/, generated/ (except generated/rexglue.cmake) or out/ is
     refused BEFORE anything is created (exit 4); a note with `No sites: <reason>` has nothing to land (exit 7, nothing created)
  2. `git worktree add -b NAME <main>\out\wt\land_NAME BASE` (BASE default: the main checkout's HEAD; out\wt is where tidy keeps plain
     worktrees: finished + merged go after 6 h idle, unmerged wait 24 h; the file <worktree>.owner holds `pid=<this process>` so a live
     landing is never swept). This is the ONLY change to the main repository. The tool runs no git command with the main checkout as its
     working directory except `worktree add` and read-only queries (git() enforces it). What it could have damaged is checked: the content
     hash of every path the note touches (and the note) in the main checkout's working tree is recorded before and compared after; a
     changed path fails loudly (exit 6) UNLESS main's HEAD moved and the path now equals the blob at the new HEAD (someone committed it).
     Main's HEAD moving or other paths changing is one informational line, never an error (the main session works while a landing runs)
  3. the note is copied into the worktree (an uncommitted note in the main checkout is the normal case), then note_sites.py --check and
     --apply with --root = the worktree. A site that is not OK: its lines are printed, the worktree stays, exit 2
  3b. S111: the note's ```claims rows are lodged as the note's own file db\claims\<sNNN>_<NOTE>.json in the worktree (tools\handback\lodge_note.py,
     rewritten whole; no other db file is touched; a note with no claims block lodges nothing and says so); the file is committed with the rest.
     A refused claim row: nothing lodged, the worktree stays, exit 2 (fix the row in the note, run again with --amend)
  3b2. S111 LANDLODGE: every OTHER note the branch changed that carries a claims block (a site editing another note's rows) is lodged the same way
     (tools\handback\lodge_diff.py); a note that fails to lodge fails the landing (exit 2) with the note named
  3c. S111 LANDRECITE (tools\handback\land_seam.py): seam rows hold no line number (the anchor locates them), so a moved line needs nothing; rows the
     note's own edits rewrote one-for-one are re-anchored (marked `re-anchored by landing <note>`), a row whose anchor line it deleted stops the landing (exit 2)
     with the topic, row, old anchor and site; the seam files written are committed with the landing
  4. when a path under src\ changed: this checkout's `game_syntax.py --root <worktree> --changed --build off` (the stream-free build the players run;
       S111 COMPILEOFF: nobody compiles the stream variant unless they ask for it by name). Fail: <= 40 error lines, exit 3
  5. commit on the branch: ONLY the files the note touched + the note (checked: a staged file the note did not name fails, exit 6)
     message `S110 LAND <NAME>: <the note's first heading>` + the Co-Authored-By trailer
  6. the proof: --proof FILE, else the note's ```proof block (prove.py's file format), run with prove.py in the worktree.
     None: `land_note: NO PROOF RUN`, exit 5 unless --keep-going
  7. ONE final block: branch + sha + worktree, applied counts, quick compile, prove's last line, the log path of each failed command.
     Exit 0 only when everything passed (1 = the proof did not pass).
--amend: re-land a corrected note onto the existing worktree of --branch (the touched paths go back to the base with a plain
`git checkout BASE -- path`; nothing is removed outside the worktree), a new commit, the proof again. S111 LANDAMEND: every db/seams/ and
db/claims/ file that differs from the base goes back too (a stopped first landing leaves re-cited seam files; the re-run rewrites them).
FULL COMPILE (S111 COMPILEOFF): there is none any more. The old detached full compile ran the same one variant as the quick compile (the off
variant) over the same tree, so it was the same work twice: a `compile` line of the proof is dropped from the run (prove.py is not given it) and
the final block says the quick compile is the compile. `--full-compile-status --branch NAME` still reads the log a landing made BEFORE this change
(exit 0 PASS, 1 FAIL + first error lines, 2 running, 3 no log).
A step longer than 20 s prints a progress line every 20 s. Stdlib only; 0 game runs.
Test hooks (selftest only): --compile-cmd SCRIPT / --prove-cmd SCRIPT replace game_syntax.py / prove.py; --repo DIR = the checkout to act in."""
import argparse
import hashlib
import importlib.util
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_SYNTAX = os.path.join(os.path.dirname(HERE), "compile", "game_syntax.py")   # this checkout's copy, run with --root <worktree>
TRAILER = "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
HEARTBEAT_S = 20


def say(msg=""):
    print(msg)
    sys.stdout.flush()


AMEND_PLANT = ""  # selftest only: "old_reset" = the old amend (resets only the note's own paths)
SEAM_PLANT = ""   # selftest only: "no_reanchor" switches the re-anchor step of land_seam.reconcile off
LODGE_PLANT = ""  # selftest only: "own_only" = the old landing (lodges only the landing note's claims block)
LOG_PLANT = ""    # selftest only: "fixed" = the old single log name, deleted before each compile (the WinError 32 twin)
CODE_PATHS = ("src", "tools", "generated/rexglue.cmake")   # what the full compile covers: identical here = identical compile result
MAIN_ROOT = None  # set by main(): the main checkout; git() runs nothing but `worktree add` and read-only queries there
MAIN_READ_ONLY = {"rev-parse", "status", "cat-file", "hash-object", "check-ref-format", "show", "log", "diff", "config", "ls-files"}


def git(cwd, *args, check=False):
    if MAIN_ROOT is not None and os.path.normcase(os.path.normpath(cwd)) == os.path.normcase(os.path.normpath(MAIN_ROOT)):
        a = list(args)
        while len(a) >= 2 and a[0] == "-c":
            a = a[2:]
        ok = bool(a) and (a[0] in MAIN_READ_ONLY or (a[0] == "worktree" and len(a) > 1 and a[1] == "add"))
        if not ok:
            raise RuntimeError("land_note: refused to run `git %s` with the main checkout as its working directory (only `worktree add` and "
                               "read-only queries are allowed there)" % " ".join(args))
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), r.returncode, (r.stderr or r.stdout).strip()))
    return r


def run(cmd, cwd, label):
    """Runs a command, collects its output, prints a progress line every HEARTBEAT_S. -> (rc, lines)."""
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    lines = []

    def pump():
        for ln in p.stdout:
            lines.append(ln.rstrip("\r\n"))
    t = threading.Thread(target=pump, daemon=True)
    t.start()
    t0 = time.time()
    while True:
        t.join(HEARTBEAT_S)
        if not t.is_alive():
            break
        say("land_note: %s still running (%d s)" % (label, time.time() - t0))
    p.wait()
    return p.returncode, lines


def load_note_sites():
    return load_module("note_sites")


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def forbidden(rel):
    r = rel.replace("\\", "/").lstrip("./")
    if r.startswith("generated/"):
        return r != "generated/rexglue.cmake"
    return r.startswith("game_data/") or r.startswith("out/") or r in ("game_data", "out", "generated")


def check_commit_files(actual, expected):
    """-> the staged files the note did not name (empty = fine). The planted-twin (b) of the selftest feeds this a stray file."""
    return sorted(set(actual) - set(expected))


def stage_files(wt, files):
    """Stages exactly the named files (never `add -A`): the note's sites, the note, and every file the lodge reported as `changed:`
    (the claims json and each db/seams json seam_wire wrote). The planted twin of the selftest replaces this with a no-op."""
    for p in files:
        git(wt, "add", "--", p)


def uncommitted(wt):
    """-> the paths `git status --porcelain` still lists in the worktree after the commit (empty = the landing is whole)."""
    return [x[3:].strip() for x in git(wt, "status", "--porcelain", "-uall").stdout.splitlines() if x.strip()]


def amend_leftovers(wt, base):
    """-> the files under db/seams/ and db/claims/ (the ones land_seam / lodge_note write) that differ from `base` in the worktree,
    committed, modified or untracked: what a landing that stopped half-way left behind. --amend puts them back to the base."""
    names = git(wt, "diff", "--name-only", base, "--", "db/seams", "db/claims").stdout.splitlines()
    names += git(wt, "ls-files", "--others", "--exclude-standard", "--", "db/seams", "db/claims").stdout.splitlines()
    return sorted({x.strip() for x in names if x.strip().startswith(("db/seams/", "db/claims/"))})


def main_state(main_root):
    h = git(main_root, "rev-parse", "HEAD").stdout.strip()
    s = git(main_root, "status", "--porcelain").stdout
    return h, s


def index_blob(main_root, rel):
    """-> the blob id of REL in main's INDEX (stage 0) now, or None (not in the index, or a conflict)."""
    r = git(main_root, "rev-parse", "-q", "--verify", ":" + rel)
    return r.stdout.strip() if r.returncode == 0 else None


def snap_paths(main_root, rels):
    """-> {rel: (sha256 of the working-tree file or None, modified in the working tree, its index blob or None)} for the main checkout."""
    out = {}
    for rel in sorted(set(rels)):
        full = os.path.join(main_root, rel)
        h = None
        if os.path.isfile(full):
            with open(full, "rb") as f:
                h = hashlib.sha256(f.read()).hexdigest()
        out[rel] = (h, bool(git(main_root, "status", "--porcelain", "--", rel).stdout.strip()), index_blob(main_root, rel))
    return out


def equals_head_blob(main_root, rel, head):
    """True when the working-tree file equals the blob of REL at `head` (or both are absent). `head` = "" = main's index."""
    exists = os.path.isfile(os.path.join(main_root, rel))
    r = git(main_root, "rev-parse", "-q", "--verify", "%s:%s" % (head, rel))
    blob = r.stdout.strip() if r.returncode == 0 else None
    if not exists:
        return blob is None
    if blob is None:
        return False
    return git(main_root, "hash-object", "--", rel).stdout.strip() == blob


def damaged_paths(main_root, before):
    """-> the touched paths whose content in main's working tree differs from before, except those whose content now equals main's INDEX
    entry when that entry MOVED since the snapshot: someone else staged or committed them (a commit, a merge, or a merge in progress whose
    files are staged while HEAD has not moved yet: `git merge --no-commit`, a conflict being resolved). This tool never writes main's index
    (git() runs only read-only queries and `worktree add` there), so a moved index entry is never its doing; a revert of someone's unstaged
    work to HEAD leaves the index where it was and is still caught. (S111 FOLLOWUPS3 review: the old rule asked for a MOVED HEAD and called
    LMPREV's merge in progress in main damage.) The planted-twin (a) of the selftest feeds this real edits, a stage, a commit and a revert."""
    now = snap_paths(main_root, before.keys())
    bad = []
    for rel, (h, _mod, ix) in before.items():
        if now[rel][0] == h:
            continue
        if now[rel][2] != ix and equals_head_blob(main_root, rel, ""):
            continue
        bad.append(rel)
    return bad


def first_heading(text):
    for ln in text.replace("\r\n", "\n").split("\n"):
        m = re.match(r"^#{1,6}\s+(.+?)\s*$", ln)
        if m:
            return m.group(1)
    return "(note has no heading)"


def full_log_path(wt, sha):
    """One log per compiled commit: a re-land never touches (or fights a running compile for) an older commit's log."""
    if LOG_PLANT == "fixed":
        return os.path.join(wt, "out", "land", "full_compile.log")
    return os.path.join(wt, "out", "land", "full_compile.%s.log" % sha[:8])


def log_result(log):
    """-> (state, lines): state 'pass' / 'fail' / 'running' (no verdict line yet, or the log is held by the compile and unreadable)."""
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return "running", []
    last = [x for x in lines if x.startswith("full compile: ")]
    if not last:
        return "running", lines
    return ("fail" if " FAIL " in last[-1] else "pass"), lines


def equivalent_log(root, wt, sha):
    """The log a compile of `sha` needs: its own, else the newest older commit's log whose code (CODE_PATHS) is identical.
    -> (path, exact) or (None, False)."""
    own = full_log_path(wt, sha)
    if os.path.isfile(own):
        return own, True
    d = os.path.join(wt, "out", "land")
    older = []
    if os.path.isdir(d):
        for nm in os.listdir(d):
            m = re.match(r"^full_compile\.([0-9a-f]{8})\.log$", nm)
            if m:
                older.append((os.path.getmtime(os.path.join(d, nm)), m.group(1), os.path.join(d, nm)))
    for _t, osha, path in sorted(older, reverse=True):
        r = git(root, "diff", "--quiet", osha, sha, "--", *CODE_PATHS)
        if r.returncode == 0:
            return path, False
    legacy = os.path.join(d, "full_compile.log")   # S111 LANDLOG (main session): a landing made before per-commit logs wrote this one name
    if LOG_PLANT != "fixed" and os.path.isfile(legacy):
        return legacy, False
    return None, False


def full_compile_status(a, main_root):
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", a.branch)
    wt = os.path.join(main_root, "out", "wt", "land_" + safe)
    head = git(main_root, "rev-parse", "--verify", "-q", a.branch + "^{commit}").stdout.strip()
    log, exact = equivalent_log(main_root, wt, head) if head else (None, False)
    if not log:
        say("full compile: no log for branch %s (head %s, folder %s): none was started" % (a.branch, head[:8], os.path.dirname(full_log_path(wt, head or "0" * 8))))
        return 3
    if not exact:
        say("full compile: no log for head %s; the log of an older commit with identical code is used" % head[:8])
    state, lines = log_result(log)
    if state == "running":
        say("full compile: running (no verdict line yet), log %s" % log)
        return 2
    last = [x for x in lines if x.startswith("full compile: ")]
    say("%s, log %s" % (last[-1], log))
    if state == "fail":
        errs = [x for x in lines if re.search(r"error|FAIL|fatal", x, re.I) and not x.startswith("full compile: ")] or lines
        for x in errs[:40]:
            say("  " + x)
        return 1
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    try:
        sys.stdout.reconfigure(errors="backslashreplace")
    except (AttributeError, OSError):
        pass
    ap = argparse.ArgumentParser(description="land a note's carried edits on its own branch + worktree")
    ap.add_argument("note", nargs="?")
    ap.add_argument("--full-compile-status", action="store_true", help="print running / PASS / FAIL of the log an earlier landing's background full compile left (none is started any more)")
    ap.add_argument("--branch", required=True)
    ap.add_argument("--base", default=None)
    ap.add_argument("--proof", default=None)
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--amend", action="store_true")
    ap.add_argument("--repo", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--compile-cmd", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--prove-cmd", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--lodge-resolver-table", default=None, help=argparse.SUPPRESS)   # selftest: lodge_note.py's fixed address table (no code index)
    a = ap.parse_args(argv)
    if not a.full_compile_status and not a.note:
        ap.error("NOTE is required (except with --full-compile-status)")

    here_root = os.path.abspath(a.repo or os.getcwd())
    top = git(here_root, "rev-parse", "--show-toplevel")
    cd = git(here_root, "rev-parse", "--path-format=absolute", "--git-common-dir")
    if top.returncode or cd.returncode:
        say("land_note: not inside a git checkout: " + here_root)
        return 1
    src_root = os.path.normpath(top.stdout.strip())
    main_root = os.path.dirname(os.path.normpath(cd.stdout.strip()))
    if a.full_compile_status:
        return full_compile_status(a, main_root)
    global MAIN_ROOT
    MAIN_ROOT = main_root
    before = main_state(main_root)
    ctx = {}
    rc = land(a, src_root, main_root, ctx)
    after = main_state(main_root)
    if "watch" in ctx:
        bad = damaged_paths(main_root, ctx["watch"])
        if bad:
            say("land_note: FATAL: the landing changed %s in the MAIN CHECKOUT's working tree (the tool must never do that). Next: run "
                "`git -C %s diff -- %s` to see the damage and restore the path(s) with `git -C %s checkout -- <path>`, then report this as a bug "
                "in tools\\handback\\land_note.py." % (", ".join(bad), main_root, " ".join(bad), main_root))
            return 6
    if before[0] != after[0]:
        say("land_note: note: main moved %s -> %s during the landing (someone else's commits)" % (before[0][:10], after[0][:10]))
    elif before[1] != after[1]:
        say("land_note: note: main's working tree changed during the landing (someone else's edits; none in the paths this landing touches)")
    return rc


def land(a, src_root, main_root, ctx):
    name = a.branch
    ns = load_note_sites()
    # --- the note: parse, forbidden paths (nothing is created yet)
    note_abs = os.path.abspath(a.note)
    try:
        with open(note_abs, "rb") as f:
            note_text = f.read().decode("utf-8", "replace")
        sites, note_proof = ns.parse_note_full(note_text)
    except ns.NoSites as e:
        say("land_note: NOTHING TO LAND: the note says `No sites: %s`. Next: nothing: this is information (no branch or worktree was created)." % e.reason)
        return 7
    except (OSError, ns.NoteError) as e:
        say("land_note: cannot read the note's sites: %s. Next: fix the note (`py -I tools\\handback\\note_sites.py --check NOTE` shows it), then run again." % e)
        return 2
    touched = [s["path"].replace("\\", "/") for s in sites]
    n_edits = sum(len(s["edits"]) for s in sites)
    n_files = sum(1 for s in sites if s["file"] is not None)
    for p in touched:
        if forbidden(p):
            say("land_note: REFUSED: the note touches %s (never committed from a note: game_data/, generated/ except generated/rexglue.cmake, out/). "
                "Next: remove that site from the note and run again (nothing was created)." % p)
            return 4
    # where the note lives inside the repo
    note_rel = None
    for root in (src_root, main_root):
        rel = os.path.relpath(os.path.realpath(note_abs), os.path.realpath(root))
        if not rel.startswith(".."):
            note_rel = rel.replace("\\", "/")
            break
    if note_rel is None:
        note_rel = "docs/research/landed_notes/" + os.path.basename(note_abs)
    if forbidden(note_rel):
        say("land_note: REFUSED: the note itself lies under %s (it is committed with the branch). Next: move the note out of game_data/, generated/, out/ "
            "and run again." % note_rel)
        return 4
    # what this tool could damage in the main checkout: the touched paths and the note (hash + modified + index entry, before anything is created)
    claims_path = load_module("lodge_note").claims_rel(note_rel)   # S111 LODGE: the claims file the lodge writes (in the worktree, never in main)
    ctx["watch"] = snap_paths(main_root, list(touched) + [note_rel, claims_path])

    safe = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    wt = os.path.join(main_root, "out", "wt", "land_" + safe)
    scratch = os.path.join(wt, "out", "land_note")
    base_file = os.path.join(scratch, "base_%s.txt" % safe)
    branch_exists = git(main_root, "rev-parse", "--verify", "-q", "refs/heads/" + name).returncode == 0
    if git(main_root, "check-ref-format", "--branch", name).returncode:
        say("land_note: %r is not a valid branch name. Next: run again with another --branch NAME." % name)
        return 1

    old_paths = []
    if a.amend:
        if not (branch_exists and os.path.isdir(wt)):
            say("land_note: --amend needs the branch %s and the worktree %s (one of them is missing). Next: land without --amend under a new --branch NAME." % (name, wt))
            return 1
        if git(wt, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip() != name:
            say("land_note: the worktree %s is not on branch %s. Next: `git -C %s switch %s`, then run --amend again." % (wt, name, wt, name))
            return 1
        try:
            with open(base_file, encoding="utf-8") as f:
                base = f.read().strip()
        except OSError:
            say("land_note: --amend: %s is missing (the base of the first landing); cannot reset the touched paths. Next: land under a new --branch NAME." % base_file)
            return 1
        if a.base:
            base = git(main_root, "rev-parse", a.base, check=True).stdout.strip()
        old_paths = [x for x in git(wt, "diff", "--name-only", base, "HEAD").stdout.splitlines() if x]
        # S111 LANDAMEND: the files the pipeline can write are fixed by what it may touch, not by what one run printed: a first landing
        # that stopped after its seam recite left db/seams/*.json and db/claims/*.json modified (uncommitted); they go back to the base too
        leftover = amend_leftovers(wt, base) if AMEND_PLANT != "old_reset" else []
        if leftover:
            say("land_note: amend: also resetting %d pipeline file(s) a stopped landing left in the worktree: %s" % (len(leftover), ", ".join(leftover)))
        reset_set = set(old_paths) | set(touched) | set(leftover)
        say("land_note: amend: resetting %d path(s) to %s in %s" % (len(reset_set), base[:10], wt))
        for p in sorted(reset_set):
            if p == note_rel:
                continue
            if git(wt, "cat-file", "-e", "%s:%s" % (base, p)).returncode == 0:
                git(wt, "checkout", base, "--", p, check=True)
            elif os.path.exists(os.path.join(wt, p)):
                git(wt, "rm", "-q", "--cached", "--ignore-unmatch", "--", p)
                os.remove(os.path.join(wt, p))
    else:
        if branch_exists:
            say("land_note: REFUSED: the branch %s exists. Next: run again with another --branch NAME, or add --amend to re-land onto its worktree." % name)
            return 1
        if os.path.exists(wt):
            say("land_note: REFUSED: the worktree folder %s exists. Next: run `py tools\\harness\\tidy.py --guard %s`, remove it with the tidy "
                "(py tools\\harness\\tidy.py --plan / --now), never a forced git worktree removal; then run again." % (wt, wt))
            return 1
        base = git(main_root, "rev-parse", "--verify", (a.base or "HEAD") + "^{commit}")
        if base.returncode:
            say("land_note: --base %s is not a commit. Next: give a commit, branch or tag that exists in the main checkout." % a.base)
            return 1
        base = base.stdout.strip()
        os.makedirs(os.path.dirname(wt), exist_ok=True)
        r = git(main_root, "-c", "core.longpaths=true", "worktree", "add", "-b", name, wt, base)
        if r.returncode:
            say("land_note: git worktree add failed: " + (r.stderr or r.stdout).strip() + ". Next: fix what git names above (often a stale worktree: py tools\\harness\\tidy.py --plan), then run again.")
            return 1
        with open(wt + ".owner", "w", encoding="utf-8") as f:
            f.write("pid=%d\nlane=LANDNOTE %s\n" % (os.getpid(), name))
        os.makedirs(scratch, exist_ok=True)
        with open(base_file, "w", encoding="utf-8") as f:
            f.write(base + "\n")
    say("land_note: worktree %s on %s (base %s)" % (wt, name, base[:10]))

    # --- the note into the worktree, then check + apply
    dst = os.path.join(wt, note_rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(note_abs, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        f.write(data)
    ns_py = os.path.join(HERE, "note_sites.py")
    for mode in ("--check", "--apply"):
        ncmd = [sys.executable, "-I", ns_py, mode, dst, "--root", wt]
        ncmd += ["--resolver-table", os.path.abspath(a.lodge_resolver_table)] if a.lodge_resolver_table else []
        rc, lines = run(ncmd, wt, "note_sites " + mode)
        for ln in lines:
            say(ln)
        if rc:
            say("land_note: the note's sites are not OK (%s); the worktree stays for inspection: %s. Next: fix the failing site above in the note, then run "
                "again with --amend --branch %s." % (mode, wt, name))
            return 2

    # --- S111 LODGE: the note's ```claims rows become its own db/claims/<sNNN>_<NOTE>.json in this worktree (travels in the same commit)
    lcmd = [sys.executable, "-I", os.path.join(HERE, "lodge_note.py"), dst, "--root", wt, "--note-as", note_rel]
    lcmd += ["--resolver-table", os.path.abspath(a.lodge_resolver_table)] if a.lodge_resolver_table else []
    rc, lines = run(lcmd, wt, "lodge_note")
    for ln in lines:
        say(ln)
    if rc:
        say("land_note: the note's claims were not lodged (lodge_note exit %d); the worktree stays: %s. Next: fix the claim rows above in the note, then "
            "run again with --amend --branch %s." % (rc, wt, name))
        return 2
    lodged = [x[len("changed: "):].strip() for x in lines if x.startswith("changed: ")]
    # S111 LANDLODGE: every OTHER note the branch changed (a site that edits another note's claims block) is lodged too, else its claims json is stale
    if LODGE_PLANT != "own_only":
        more, bad_notes = load_module("lodge_diff").lodge_changed(
            wt, base, say, skip={note_rel},
            extra=["--resolver-table", os.path.abspath(a.lodge_resolver_table)] if a.lodge_resolver_table else [])
        if bad_notes:
            say("land_note: NOT LODGED: %s; the worktree stays: %s. Next: fix the claim rows above in that note, then run again with --amend --branch %s."
                % (", ".join("%s (lodge_note exit %d)" % x for x in bad_notes), wt, name))
            return 2
        lodged += [p for p in more if p not in lodged]
    touched = list(touched) + [p for p in lodged if p not in touched]

    # --- S111 LANDRECITE: the seam rows this note's own edits rewrote are re-anchored, the ones it deleted stop us
    lseam = load_module("land_seam")
    stops, seam_files = lseam.reconcile(wt, lseam.edits_by_path(sites), note_rel, say, SEAM_PLANT)
    if stops:
        for s in stops:
            say(s)
        return 2
    touched += [p for p in seam_files if p not in touched]

    # --- the quick compile
    compiled = "skipped (no src)"
    if any(p.startswith("src/") for p in touched):
        if a.compile_cmd:
            cmd = [sys.executable, "-I", os.path.abspath(a.compile_cmd)]
        else:
            # S111: MAIN's compile tool on the worktree's tree (--root), like every other tool here: a lane cut before a fix to the tool must not run the old one
            cmd = [sys.executable, "-I", GAME_SYNTAX, "--root", wt, "--changed", "--build", "off"]
        rc, lines = run(cmd, wt, "quick compile")
        if rc:
            errs = [x for x in lines if re.search(r"error|FAIL|fatal", x, re.I)] or lines
            say("land_note: quick compile FAILED (exit %d); first lines:" % rc)
            for x in errs[:40]:
                say("  " + x)
            say("land_note: the worktree stays: %s. Next: fix the error lines above in the note (or the code they name), then run again with --amend --branch %s." % (wt, name))
            return 3
        compiled = "PASS"
        say("land_note: quick compile PASS")

    # --- commit
    files = sorted(set(touched) | {note_rel})
    expected = set(files) | set(old_paths)
    stage_files(wt, files)
    for p in old_paths:
        if p not in files:
            git(wt, "add", "-A", "--", p)
    staged = [x for x in git(wt, "diff", "--cached", "--name-only").stdout.splitlines() if x]
    bad = [x for x in staged if forbidden(x)]
    if bad:
        say("land_note: REFUSED: staged path %s is under game_data/, generated/ or out/. Next: remove that path from the note and run again with --amend." % bad[0])
        git(wt, "reset", "-q", "--")
        return 4
    extra = check_commit_files(staged, expected)
    if extra:
        say("land_note: FATAL: the commit would hold a file the note did not touch: %s. Next: look in the worktree %s (`git status`), remove the stray file(s), "
            "then run again with --amend." % (", ".join(extra), wt))
        git(wt, "reset", "-q", "--")
        return 6
    msg = "S110 LAND %s: %s\n\n%s\n" % (name, first_heading(note_text), TRAILER)
    msgfile = os.path.join(scratch, "commit_msg.txt")
    os.makedirs(scratch, exist_ok=True)
    with open(msgfile, "w", encoding="utf-8") as f:
        f.write(msg)
    cfg = []
    if not git(wt, "config", "user.email").stdout.strip():
        cfg = ["-c", "user.name=land_note", "-c", "user.email=land_note@localhost"]
    if staged:
        r = git(wt, *cfg, "commit", "-q", "-F", msgfile)
        if r.returncode:
            say("land_note: commit failed: " + (r.stderr or r.stdout).strip() + ". Next: fix what git names above in the worktree " + wt + ", then run again with --amend.")
            return 6
    else:
        say("land_note: nothing new to commit (the worktree already holds this landing)")
    left = uncommitted(wt)
    if left:
        say("land_note: FATAL: the landing left %d file(s) uncommitted in the worktree: %s. Next: a tool wrote a file the commit did not take "
            "(the lodge reports each file it writes as `changed: PATH`): fix that in tools\\handback, then `git -C %s add` the file(s) and commit."
            % (len(left), ", ".join(left[:8]), wt))
        return 6
    sha = git(wt, "rev-parse", "HEAD").stdout.strip()

    # --- the proof
    proof_text = None
    if a.proof:
        try:
            with open(a.proof, encoding="utf-8-sig") as f:
                proof_text = f.read()
        except OSError as e:
            say("land_note: cannot read --proof file: %s. Next: give an existing proof file (prove.py's format) and run again with --amend." % e)
            return 5
    elif note_proof:
        proof_text = note_proof
    has_proof = proof_text is not None
    # a `compile` line leaves the proof: the quick compile above (the off variant) already is that compile (S111 COMPILEOFF)
    plines = proof_text.splitlines() if has_proof else []
    rest = [x for x in plines if x.strip() != "compile"]
    has_cmds = any(x.strip() and not x.strip().startswith("#") and not x.strip().startswith("base ") for x in rest)
    proof_file = None
    if has_cmds:
        proof_file = os.path.join(scratch, "proof_%s.txt" % safe)
        with open(proof_file, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(rest) + "\n")
    proof_summary, failed_logs, proof_rc = None, [], 0
    if has_proof and not has_cmds:
        proof_summary = "prove: (the proof held only the compile line: the quick compile above is it)"
    if proof_file:
        pcmd = [sys.executable, "-I", os.path.abspath(a.prove_cmd) if a.prove_cmd else os.path.join(HERE, "prove.py"), proof_file, "--root", wt]
        prc, lines = run(pcmd, wt, "proof")
        proof_rc = proof_rc or prc
        for ln in lines:
            say(ln)
        pl = [x for x in lines if x.startswith("prove:")]
        proof_summary = pl[-1] if pl else "(prove printed no summary line; exit %d)" % prc
        failed_logs += [x.strip() for x in lines if x.strip().startswith("log: ")]

    say("")
    say("land_note: branch %s at %s, worktree %s" % (name, sha[:10], wt))
    say("applied: %d sites (%d edits, %d new files)" % (len(sites), n_edits, n_files))
    say("lodged: %s" % (", ".join(lodged) if lodged else "no change (no claims block, or the claims file already holds these rows)"))
    say("quick compile: %s" % compiled)
    say("full compile: none (the quick compile, the off variant, is the compile)")
    if not has_proof:
        say("land_note: NO PROOF RUN. Next: add a ```proof block to the note or pass --proof FILE (run again with --amend), or --keep-going to accept no proof.")
        return 0 if a.keep_going else 5
    if proof_summary:
        say(proof_summary)
    for lg in failed_logs:
        say("failed command " + lg)
    return 0 if proof_rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
