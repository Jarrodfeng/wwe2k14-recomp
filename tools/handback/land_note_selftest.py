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

        # --- 7. S111 COMPILEOFF: a `compile` line of the proof starts no detached full compile (the quick compile is the compile)
        n8 =os.path.join(notes, "n8.md")
        w(n8, note("Compile line", proof="compile\n" + PASS_CMD))
        rc, out = run_land(ln, repo, [n8, "--branch", "s-fc"] + base_args)
        check(rc == 0 and "full compile: none" in out and "running in background" not in out, "compile line: no detached full compile is started (rc %d)" % rc)
        check("prove: 1 passed" in out, "compile line: the other proof commands still ran (the compile line left the proof)")
        check(not os.path.isdir(os.path.join(repo, "out", "wt", "land_s-fc", "out", "land")), "compile line: no full_compile log folder was made")
        n9 = os.path.join(notes, "n9.md")
        w(n9, note("Compile line only", proof="compile"))
        rc, out = run_land(ln, repo, [n9, "--branch", "s-fcb"] + base_args)
        check(rc == 0 and "the compile line: the quick compile above is it" in out, "compile-only proof: the landing says the quick compile is it (rc %d)" % rc)
        check(ln.full_compile_status.__name__ == "full_compile_status" and not hasattr(ln, "start_full_compile"), "the detached launcher is gone (status reader kept for old logs)")


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

        # --- 11. S111 LODGE: a ```claims block is lodged as the note's own db/claims file and committed with the sites; a refused row stops
        #     the landing (exit 2, nothing committed, no claims file) and --amend after the fix lands it
        table = os.path.join(tmp, "lodge_table.json")
        w(table, '{"funcs": {"0x82331BC0": 256}, "data": []}\n')
        largs = base_args + ["--lodge-resolver-table", table]
        claim = "0x82331BE0 | C | the fixture card waits for the loader | evidence: decomp sub_82331BC0 +0x20"
        n14 = os.path.join(notes, "n14.md")
        w(n14, note("Claims landing", cur="int a = 7;", new="int a = 8;") + "\n```claims\n%s\n```\n" % claim)
        before = snap()
        rc, out = run_land(ln, repo, [n14, "--branch", "s-claims"] + largs)
        check(rc == 0, "claims: exit 0 (got %d)" % rc)
        if rc:
            print(out)
        names = set(git(repo, "show", "--name-only", "--format=", "s-claims").split())
        check(names == {"src/a.h", "tools/new.txt", "docs/n14.md", "db/claims/lodged_n14.json"},
              "claims: the commit holds the sites, the note and its claims file (%s)" % sorted(names))
        check("lodged: db/claims/lodged_n14.json" in out, "claims: the final block names the lodged file")
        check('"function": "0x82331BC0"' in git(repo, "show", "s-claims:db/claims/lodged_n14.json"), "claims: the row resolved to its function")
        check(snap() == before and not os.path.exists(os.path.join(repo, "db")), "claims: nothing written in the main checkout")
        n15 = os.path.join(notes, "n15.md")
        w(n15, note("Claims refused", cur="int a = 7;", new="int a = 8;") + "\n```claims\n0x82999999 | C | the fixture says something | evidence: nothing\n```\n")
        rc, out = run_land(ln, repo, [n15, "--branch", "s-claims-bad"] + largs)
        wt15 = os.path.join(repo, "out", "wt", "land_s-claims-bad")
        check(rc == 2 and "claims: ERROR" in out and "0x82999999" in out and "--amend" in out,
              "refused claim row: exit 2 naming the row and the --amend rerun (rc %d)" % rc)
        check(git(repo, "rev-list", "--count", "main..s-claims-bad").strip() == "0", "refused claim row: nothing committed")
        check(not os.path.exists(os.path.join(wt15, "db", "claims", "lodged_n15.json")), "refused claim row: no claims file in the worktree")
        w(n15, note("Claims refused, fixed", cur="int a = 7;", new="int a = 8;") + "\n```claims\n%s\n```\n" % claim)
        rc, out = run_land(ln, repo, [n15, "--branch", "s-claims-bad", "--amend"] + largs)
        names = set(git(repo, "show", "--name-only", "--format=", "s-claims-bad").split())
        check(rc == 0 and "db/claims/lodged_n15.json" in names, "refused claim row, fixed: --amend lands it with the claims file (rc %d, %s)" % (rc, sorted(names)))

        # --- 12. S111 LANDSEAM: a claims row with seam=/ours= writes db/seams/<topic>.json in the worktree; the landing's commit holds it and
        #     the worktree is clean afterwards (before this fix the seam file was left uncommitted)
        srow = claim + " | seam=zebra ours=src/a.h:3 verdict=match"
        n16 = os.path.join(notes, "n16.md")
        w(n16, note("Seam landing", cur="int a = 7;", new="int a = 8;") + "\n```claims\n%s\n```\n" % srow)
        rc, out = run_land(ln, repo, [n16, "--branch", "s-seam"] + largs)
        wt16 = os.path.join(repo, "out", "wt", "land_s-seam")
        if rc:
            print(out)
        check(rc == 0, "seam: exit 0 (got %d)" % rc)
        names = set(git(repo, "show", "--name-only", "--format=", "s-seam").split())
        check(names == {"src/a.h", "tools/new.txt", "docs/n16.md", "db/claims/lodged_n16.json", "db/seams/zebra.json"},
              "seam: the commit holds the claims file AND the seam file (%s)" % sorted(names))
        check(git(wt16, "status", "--porcelain").strip() == "", "seam: git status --porcelain is empty after the landing")
        check("changed: db/seams/zebra.json" in out and "db/seams/zebra.json" in out.split("lodged:")[-1], "seam: seam_wire reported the file and the final block names it")
        # planted twin (c): the stage step skipped -> the landing must NOT report success (the leftover check catches the unstaged files)
        n17 = os.path.join(notes, "n17.md")
        w(n17, note("Seam twin", cur="int a = 7;", new="int a = 8;") + "\n```claims\n%s\n```\n" % srow.replace("zebra", "okapi"))
        real_stage = ln.stage_files
        ln.stage_files = lambda wt, files: None
        try:
            rc, out = run_land(ln, repo, [n17, "--branch", "s-seam-twin"] + largs)
        finally:
            ln.stage_files = real_stage
        check(rc == 6 and "FATAL" in out and "uncommitted" in out and "okapi.json" in out,
              "twin (c): with the stage step skipped the landing fails loudly naming the seam file (rc %d)" % rc)

        # --- 12b. S111 LANDLODGE: a landing whose sites edit ANOTHER note's claims row lodges that note too (its claims json matches its block
        #     afterwards); twin: lodging only the landing's own note leaves the other note's json stale (the lodge check sees "rows replaced")
        lodge_py = os.path.join(HERE, "lodge_note.py")
        old_row = claim
        new_row = claim.replace("waits for the loader", "never waits for the loader")
        other = os.path.join(repo, "docs", "other.md")
        w(other, "# Other note\n\n```claims\n%s\n```\n" % old_row)
        subprocess.run([sys.executable, "-I", lodge_py, other, "--root", repo, "--resolver-table", table], cwd=repo, capture_output=True)
        git(repo, "add", "docs/other.md", "db/claims/lodged_other.json")
        git(repo, "commit", "-q", "-m", "other note, lodged")

        def other_json_matches(branch_wt):
            r = subprocess.run([sys.executable, "-I", lodge_py, os.path.join(branch_wt, "docs", "other.md"), "--root", branch_wt, "--note-as", "docs/other.md",
                                "--resolver-table", table, "--dry"], cwd=branch_wt, capture_output=True, text=True)
            return "(unchanged)" in r.stdout
        n20 = os.path.join(notes, "n20.md")
        w(n20, note("Edits another note", edit_path="docs/other.md", cur=old_row, new=new_row))
        rc, out = run_land(ln, repo, [n20, "--branch", "s-lodge-other"] + largs)
        if rc:
            print(out)
        wt20 = os.path.join(repo, "out", "wt", "land_s-lodge-other")
        names = set(git(repo, "show", "--name-only", "--format=", "s-lodge-other").split())
        check(rc == 0 and "db/claims/lodged_other.json" in names and "never waits" in git(repo, "show", "s-lodge-other:db/claims/lodged_other.json"),
              "other note: landing exit 0 and the commit holds the other note's refreshed claims json (rc %d, %s)" % (rc, sorted(names)))
        check(other_json_matches(wt20), "other note: its claims json matches its block after the landing")
        n21 = os.path.join(notes, "n21.md")
        w(n21, note("Edits another note, twin", edit_path="docs/other.md", cur=old_row, new=new_row))
        ln.LODGE_PLANT = "own_only"
        try:
            rc, out = run_land(ln, repo, [n21, "--branch", "s-other-twin"] + largs)
        finally:
            ln.LODGE_PLANT = ""
        wt21 = os.path.join(repo, "out", "wt", "land_s-other-twin")
        check(not other_json_matches(wt21), "twin: lodging only the own note leaves the other note's claims json stale (the lodge check fails)")
        # a note that fails to lodge fails the landing with the note named
        n22 = os.path.join(notes, "n22.md")
        w(n22, note("Edits another note, bad row", edit_path="docs/other.md", cur=old_row, new=new_row.replace("0x82331BE0", "0x82999999")))
        rc, out = run_land(ln, repo, [n22, "--branch", "s-other-bad"] + largs)
        check(rc == 2 and "NOT LODGED" in out and "docs/other.md" in out, "other note with a refused row: exit 2 naming docs/other.md (rc %d)" % rc)

        # --- 13. S111 LANDRECITE: seam rows
        # --- 13. S111 LANDRECITE: seam rows the note's own edits moved (no change: no row stores a line), rewrote one-for-one (re-anchored) or deleted (stop)
        import json
        w(os.path.join(repo, "src", "b.h"), "// x\nint keepme = 1;\nint other = 2;\n")
        entry = {"topic": "mango", "summary": "fixture.",
                 "game": [{"addr": "0x82331BE0", "what": "w", "evidence": "e", "mark": "C", "source": "docs/n1.md"}],
                 "ours": [{"path": "src/b.h", "anchor": "int keepme = 1;", "verdict": "match", "why": "same"}], "open": []}
        w(os.path.join(repo, "db", "seams", "mango.json"), json.dumps(entry, indent=1) + "\n")
        git(repo, "add", "src/b.h", "db/seams/mango.json")
        git(repo, "commit", "-q", "-m", "seam fixture")

        def seam_row(branch):
            return json.loads(git(repo, "show", "%s:db/seams/mango.json" % branch))["ours"][0]
        ins = note("Seam insert", edit_path="src/b.h", cur="// x", new="// x\n// y\n// z")
        n18 = os.path.join(notes, "n18.md")
        w(n18, ins)
        rc, out = run_land(ln, repo, [n18, "--branch", "s-rc-ins"] + base_args)
        if rc:
            print(out)
        names = set(git(repo, "show", "--name-only", "--format=", "s-rc-ins").split())
        check(rc == 0 and "db/seams/mango.json" not in names and "line" not in seam_row("s-rc-ins") and "re-cited" not in out,
              "lines inserted above the row: ONE landing, the seam file is not touched (the anchor locates the row, no line to re-cite) (rc %d)" % rc)
        n19 = os.path.join(notes, "n19.md")
        w(n19, note("Seam rewrite", edit_path="src/b.h", cur="int keepme = 1;", new="int keepme = 2;"))
        rc, out = run_land(ln, repo, [n19, "--branch", "s-rc-rw"] + base_args)
        if rc:
            print(out)
        row = seam_row("s-rc-rw") if rc == 0 else {}
        check(rc == 0 and row.get("anchor") == "int keepme = 2;" and "line" not in row and row.get("reanchored") == "re-anchored by landing n19.md"
              and "re-anchored to" in out, "re-anchor: a one-for-one rewrite lands, the row follows the new line and is marked (rc %d)" % rc)
        ln.SEAM_PLANT = "no_reanchor"
        try:
            rc, out = run_land(ln, repo, [n19, "--branch", "s-rc-rw-twin"] + base_args)
        finally:
            ln.SEAM_PLANT = ""
        check(rc == 0 and seam_row("s-rc-rw-twin").get("anchor") == "int keepme = 1;", "twin (e): with the re-anchor skipped the row keeps its dead anchor (the check above fails first)")
        n20 = os.path.join(notes, "n20.md")
        w(n20, note("Seam delete", edit_path="src/b.h", cur="int keepme = 1;\nint other = 2;", new="int other = 2;"))
        rc, out = run_land(ln, repo, [n20, "--branch", "s-rc-del"] + base_args)
        check(rc == 2 and "SEAM ROW STOPS" in out and "int keepme = 1;" in out and "mango" in out and "src/b.h" in out and "--amend" in out,
              "deleted anchor: stops (exit 2) naming topic, row, old anchor and site (rc %d)" % rc)
        check(git(repo, "rev-list", "--count", "main..s-rc-del").strip() == "0", "deleted anchor: nothing committed")

        # --- 13b. S111 LANDAMEND: a first landing that stops after its recite (quick compile fails) leaves re-cited seam files modified;
        #      --amend must reset them, redo the recite and commit them (before: the re-run recited nothing, FATAL "uncommitted", exit 6)
        failstub = os.path.join(tmp, "compile_fail.py")
        w(failstub, "import sys\nsys.exit(1)\n")
        n23 = os.path.join(notes, "n23.md")
        w(n23, note("Seam rewrite", edit_path="src/b.h", cur="int keepme = 1;", new="int keepme = 2;"))

        def amend_case(branch, plant):
            rc1, _ = run_land(ln, repo, [n23, "--branch", branch, "--compile-cmd", failstub])
            wt = os.path.join(repo, "out", "wt", "land_" + branch)
            dirty = git(wt, "status", "--porcelain").split()
            ln.AMEND_PLANT = plant
            try:
                rc2, out2 = run_land(ln, repo, [n23, "--branch", branch, "--amend"] + base_args)
            finally:
                ln.AMEND_PLANT = ""
            return rc1, dirty, rc2, out2, wt
        rc1, dirty, rc2, out2, wt23 = amend_case("s-am", "")
        check(rc1 == 3 and "db/seams/mango.json" in dirty, "amend: the first landing stops after the recite with the seam file modified (rc %d, %s)" % (rc1, dirty))
        names = set(git(repo, "show", "--name-only", "--format=", "s-am").split())
        check(rc2 == 0 and "db/seams/mango.json" in names and seam_row("s-am")["anchor"] == "int keepme = 2;", "amend: the re-land commits the re-anchored seam file (rc %d, %s)" % (rc2, sorted(names)))
        check(git(wt23, "status", "--porcelain").strip() == "", "amend: git status --porcelain is empty after the re-land")
        check(names == {"src/b.h", "tools/new.txt", "docs/n23.md", "db/seams/mango.json"}, "amend: the commit holds exactly its own files (%s)" % sorted(names))
        rc1, dirty, rc2, out2, _ = amend_case("s-am-twin", "old_reset")
        check(rc2 == 6 and "uncommitted" in out2 and "mango.json" in out2, "twin (g): the old amend reset leaves the seam file uncommitted: FATAL exit 6 (rc %d)" % rc2)

        # --- planted twin (a): what the tool could damage in main's working tree (real edit, then a real commit)
        # --- planted twin (a): what the tool could damage in main's working tree (real edit, then a real commit)
        ln.MAIN_ROOT = repo
        keep = os.path.join(repo, "tools", "keep.txt")
        before = ln.snap_paths(repo, ["tools/keep.txt", "tools/absent.txt"])
        check(ln.damaged_paths(repo, before) == [], "twin (a): untouched paths read as untouched")
        w(keep, "edited by a bad landing\n")
        check(ln.damaged_paths(repo, before) == ["tools/keep.txt"], "twin (a): a working-tree edit of a touched path is caught")
        # S111 FOLLOWUPS3 review: someone's merge in progress (`git merge --no-commit`, then `git commit`): main's index holds the merged file while HEAD has not moved yet.
        # The old rule (HEAD moved and the file equals its blob) called this damage: the FATAL of the FOLLOWUPS3 landing (LMPREV's merge into main, reflog "commit (merge)").
        git(repo, "add", "tools/keep.txt")
        check(ln.damaged_paths(repo, before) == [], "twin (a): the same edit STAGED by someone (a merge in progress: the index moved, HEAD not yet) is fine")
        git(repo, "commit", "-q", "-m", "twin: someone commits it")
        check(ln.damaged_paths(repo, before) == [], "twin (a): the same edit committed by someone (content = the new index / HEAD blob) is fine")
        w(keep, "edited again after the commit\n")
        check(ln.damaged_paths(repo, before) == ["tools/keep.txt"], "twin (a): an edit that differs from main's index is still caught")
        git(repo, "checkout", "--", "tools/keep.txt")
        w(keep, "someone's unstaged work\n")
        before2 = ln.snap_paths(repo, ["tools/keep.txt"])
        git(repo, "checkout", "--", "tools/keep.txt")   # a bad landing throws that work away: the file is back at HEAD, the index never moved
        check(ln.damaged_paths(repo, before2) == ["tools/keep.txt"], "twin (a): a revert of someone's unstaged work to HEAD is caught (the index did not move)")
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
