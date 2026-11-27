#!/usr/bin/env python3
r"""S111 SEAMWIRE: a lodged note's claims rows that carry `seam=TOPIC` update the seam index (db\seams\<TOPIC>.json) by themselves.

lodge_note.py calls wire(objs, root, dry) with the claims-file objects it just built (and `py -I tools\handback\seam_wire.py [CLAIMS.json...]`
wires already lodged claims files for the backfill). Per claims row with `seam`:
  - a GAME row (qseam.game_row): addr = the claim's address, what = the claim, evidence = the claim's evidence, mark, source = the note.
    Key: topic + address. Present with the same source: updated in place when the text differs ("updated"); present with ANOTHER source:
    left alone ("kept, owned by <source>"). Two rows of one note on the same topic + address make one game row (the first claim).
  - when `ours=PATH:LINE` is given, an OURS row (qseam.ours_row: the anchor is read from that line of the file now): verdict = the row's
    `verdict`, why = the claim, `source` = the note. Key: topic + path + anchor. Present: left alone, or updated (verdict / why) when its
    `source` is this note; a row without `source` (hand-made) is never touched. A line that is out of range / blank / a file that is gone,
    or ours= without verdict=, is REFUSED and reported; the game row is still added.
  - a topic with no entry yet: the entry is created only when it gets at least one game row and one ours row (qseam's rule), its summary
    names the note; otherwise "no entry for TOPIC: row kept in claims only".
Nothing is deleted: a row a later edit of the note dropped stays in the entry (the seam store also holds hand-made rows). Rows of another
note are never edited. Several claims of one note on one ours line make one ours row: the first claim's verdict / why, on every lodge.
The cited line is checked against the tree the note cited it in (SEAMWIRE review): the commit that first held the claims row's whole text
in the note (git log -m -S: a merge commit that wrote the row while resolving counts, SEAMCITE review; the row is the note's line holding its `ours=PATH:LINE` and its claim), its TYPE column left out (S111 lane B
review: inserting `| site sub_X+OFF` is not a re-citation), or, when that is LATER, the first commit whose claims file held that address with
that ours= (S111 SEAMCITE: a reword of the claim moves the row's text into a new commit, never its citation; also the row whose text is in no
commit of the note, a correction carried into the note from the file, pull_claims.py); when PATH has changed since that commit,
the line's text in that commit must be the text of line LINE now, or of exactly one other line now (followed: "PATH:LINE -> M", the row
takes M); otherwise the row is REFUSED "anchor unverified" (the game row still lands): re-read the line and re-cite it (a new PATH:LINE
is a new citation; an edit of the claim that keeps its ours= value keeps the first claims commit's date, SEAMCITE). A row in no commit (the note is being written now), a root that is not a git checkout, or a file unchanged since:
read as it is now. One ours row per note, claim and path: a claim of the note that already has a row on that path (re-anchored or
re-cited by a later commit) gets no second row, so relodging an unchanged note adds no seam row.
wire() returns {topic: {"game_added": n, "game_updated": n, "game_present": n, "ours_added": n, "ours_updated": n, "ours_present": n,
"refused": [str], "notes": [str]}}. Each seam file written prints an unindented `changed: db/seams/T.json` line (land_note commits it). Stdlib only; 0 game runs."""
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def load_qseam():
    spec = importlib.util.spec_from_file_location("qseam_wire", os.path.join(REPO, "tools", "q", "qseam.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _stat():
    return {"game_added": 0, "game_updated": 0, "game_present": 0, "ours_added": 0, "ours_updated": 0, "ours_present": 0,
            "refused": [], "notes": []}


def _git(root, *args):
    """git -C root ARGS -> stdout, or None (no git, not a checkout, or a failing command)"""
    try:
        r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def _row_line(root, o):
    """the note's line that holds this claims row (its ours= token and its claim), or None"""
    try:
        with open(os.path.join(root, *o["note"].split("/")), encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    tok = re.compile(re.escape("ours=" + o["ours"]) + r"(?![0-9])")
    return next((t for t in lines if tok.search(t) and o["claim"] in t), None)


def _untyped(raw):
    """the note's row line without its TYPE column (`| site sub_X+OFF`, tools\\q\\qfactkey.py is_type_field), the text it was cited with"""
    spec = importlib.util.spec_from_file_location("qfactkey_sw", os.path.join(REPO, "tools", "q", "qfactkey.py"))
    fk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fk)
    f = raw.split(" | ")
    return " | ".join(f[:4] + [x for x in f[4:] if not fk.is_type_field(x)])


def _claims_commit(root, o):
    """the oldest commit whose claims file of the note holds a row of this address with this ours= value, or None (by address, not by the
    claim's text: a review rewords claims, the citation stays)"""
    import json
    spec = importlib.util.spec_from_file_location("lodge_note_sw", os.path.join(HERE, "lodge_note.py"))
    ln = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ln)
    crel = ln.claims_rel(o["note"])
    log = _git(root, "log", "-m", "--format=%H", "-S", json.dumps(o["ours"], ensure_ascii=False), "--", crel)   # -m: a merge's own resolution counts
    for c in reversed((log or "").split()):
        try:
            rows = json.loads(_git(root, "show", "%s:%s" % (c, crel)) or "[]")
        except ValueError:
            continue
        if any(isinstance(r, dict) and str(r.get("address")).upper() == str(o["address"]).upper() and r.get("ours") == o["ours"] for r in rows):
            return c
    return None


def _cited_commit(root, o):
    """the commit the citation was made in (see the docstring), or None (read in today's tree)"""
    raw = _row_line(root, o)
    c = _claims_commit(root, o)   # the earliest claims commit of this address + ours= value: a reword of the claim never moves it (SEAMCITE)
    if raw is not None:
        log = _git(root, "log", "-m", "--format=%H", "-S", _untyped(raw), "--", o["note"])   # -m: a row written in a merge (ENVCUBE 9ababa37c)
        if log and log.split():
            n = log.split()[-1]   # the oldest commit that held the row's text
            # the earlier of the two holds the citation: a reworded row's text is only in the reword's commit, its claims row is older
            return c if c and c != n and _git(root, "merge-base", "--is-ancestor", c, n) is not None else n
    if c or raw is None:
        return c
    log = _git(root, "log", "-m", "--format=%H", "-S", raw, "--", o["note"])   # a note written with its TYPE column from the start
    return log.split()[-1] if log and log.split() else None


def _cited_text(root, o, path, line):
    """-> None when the citation is read in today's tree (the row's text in no commit of the note, no git, or PATH unchanged since);
    else (commit, the stripped text PATH:LINE had in that commit's tree, or None when it had no such line)"""
    c = _cited_commit(root, o)
    if c is None:
        return None
    since = _git(root, "log", "--format=%H", "-1", c + "..HEAD", "--", path)
    if since is None or not since.strip():
        return None
    blob = _git(root, "show", "%s:%s" % (c, path))
    lines = blob.splitlines() if blob is not None else []
    return c, (lines[line - 1].strip() if 1 <= line <= len(lines) else None)


def _ours_for(qs, o, root):
    """the claims row's ours=PATH:LINE -> (ours row, None, moved note or None) or (None, reason, None)"""
    m = re.fullmatch(r"(.+):(\d+)", o["ours"])
    if not m:
        return None, "ours=%r is not PATH:LINE" % o["ours"], None
    if o.get("verdict") not in qs.VERDICTS:
        return None, "%s: ours= needs verdict=match|replace|repeat" % o["ours"], None
    path, line = m.group(1).replace("\\", "/"), int(m.group(2))
    expect, moved = None, None
    cited = _cited_text(root, o, path, line)
    if cited:
        c, then = cited
        if not then:
            return None, "%s: anchor unverified: %s has changed since the note cited it (commit %s) and had no such line then" % (
                o["ours"], path, c[:9]), None
        try:
            with open(os.path.join(root, *path.split("/")), encoding="utf-8", errors="replace") as f:
                now = f.read().splitlines()
        except OSError:
            now = []
        if not (1 <= line <= len(now) and now[line - 1].strip() == then):
            hits = [i + 1 for i, t in enumerate(now) if t.strip() == then]
            if len(hits) != 1:
                return None, "%s: anchor unverified: the line held %r when the note cited it (commit %s); %s changed since and that text is %s now: re-read it and re-cite" % (
                    o["ours"], then, c[:9], path, "on lines %s" % hits[:5] if hits else "on no line"), None
            moved = "%s:%d -> %d: the line the note cited (commit %s) has moved; re-cite it" % (path, line, hits[0], c[:9])
            line = hits[0]
        expect = then
    try:
        row = qs.ours_row(path, line, o["verdict"], o["claim"], root=root, expect=expect)
    except qs.SeamError as e:
        return None, "%s: stale / unreadable anchor: %s" % (o["ours"], e), None
    row["lodged"] = o["note"]
    return row, None, moved


def _apply(qs, entry, topic, rows, root, st):
    """rows = [(claims obj, ours row or None)]; mutates entry; -> set of new ours row numbers (1-based) for the anchor check"""
    new_ours = set()
    seen_game = {}
    fresh = []   # ours rows this run has already handled: a later claim of the note on the same row never rewrites them (first claim wins)
    for o, orow in rows:
        addr = qs.norm_addr(o["address"])
        g = qs.game_row(addr, o["claim"], o["evidence"], o["mark"], o["note"])
        g["lodged"] = o["note"]
        if addr not in seen_game:
            seen_game[addr] = True
            ex = next((x for x in entry["game"] if qs.norm_addr(x.get("addr", "")) == addr), None)
            if ex is None:
                entry["game"].append(g)
                st["game_added"] += 1
            elif ex.get("lodged") == o["note"]:
                if any(ex.get(k) != g[k] for k in ("what", "evidence", "mark", "source")):
                    ex.update(g)
                    st["game_updated"] += 1
                else:
                    st["game_present"] += 1
            else:   # a hand-made row, or another note's: never touched
                st["game_present"] += 1
                if ex.get("lodged") and ex.get("lodged") != o["note"]:
                    st["notes"].append("game %s present, owned by %s: kept" % (addr, ex.get("lodged")))
        if orow is None:
            continue
        cands = [x for x in entry["ours"] if x.get("path") == orow["path"] and str(x.get("anchor", "")).strip() == orow["anchor"]]
        ex = next((x for x in cands if x.get("occurrence") == orow.get("occurrence")), None)   # the same text on another line of the file is another row: its occurrence says which
        if ex is None:   # one row per note, claim and path: this claim's row re-anchored by a later commit is that row, never a second
            same = next((x for x in entry["ours"] if x.get("path") == orow["path"] and x.get("lodged") == o["note"] and x.get("why") == orow["why"]), None)
            if same is not None:
                st["ours_present"] += 1
                st["notes"].append("ours %s [%s]: this claim's row is already in the entry (%s): kept, not added again" % (
                    orow["path"], orow["anchor"][:60], same.get("reanchored") or "re-anchored"))
                continue
        if ex is None:
            entry["ours"].append(orow)
            fresh.append(orow)
            new_ours.add(len(entry["ours"]))
            st["ours_added"] += 1
        elif any(ex is f for f in fresh):
            st["ours_present"] += 1
        else:
            if ex.get("lodged") == o["note"]:
                fresh.append(ex)
                if ex.get("verdict") != orow["verdict"] or ex.get("why") != orow["why"]:
                    ex["verdict"], ex["why"] = orow["verdict"], orow["why"]
                    st["ours_updated"] += 1
                else:
                    st["ours_present"] += 1
            else:
                st["ours_present"] += 1
    return new_ours


def _report_changed(qs, topic, root, dry, out):
    """prints `changed: db/seams/<topic>.json` (unindented: the line land_note.py stages, read from lodge_note's output) for a file written"""
    if not dry:
        out("changed: " + os.path.relpath(qs.entry_path(topic, root), root).replace("\\", "/"))


def wire(objs, root, dry=False, qs=None, out=print):
    qs = qs or load_qseam()
    by_topic = {}
    for o in objs:
        if o.get("seam"):
            by_topic.setdefault(o["seam"], []).append(o)
    res = {}
    for topic, orows in by_topic.items():
        st = res[topic] = _stat()
        if not qs.TOPIC_RE.match(topic):
            st["refused"].append("topic %r: not a seam topic name" % topic)
            continue
        rows = []
        for o in orows:
            orow = None
            if o.get("ours"):
                orow, why, moved = _ours_for(qs, o, root)
                if why:
                    st["refused"].append("%s %s: %s" % (o["address"], topic, why))
                if moved:
                    st["notes"].append(moved)
            rows.append((o, orow))
        path = qs.entry_path(topic, root)
        try:
            entry = qs.load(path)
            exists = True
        except (OSError, ValueError):
            exists = False
        try:
            if exists:
                entry.setdefault("game", [])
                entry.setdefault("ours", [])
                new = _apply(qs, entry, topic, rows, root, st)
                if any(st[k] for k in ("game_added", "game_updated", "ours_added", "ours_updated")):
                    qs._write(entry, root, dry, anchors=new)
                    _report_changed(qs, topic, root, dry, out)
            else:
                if not any(r for _, r in rows):
                    st["notes"].append("no entry for %s: row kept in claims only (no usable ours line)" % topic)
                    st["game_present"] = 0
                    continue
                entry = {"topic": topic, "game": [], "ours": [], "open": [],
                         "summary": "Seeded from %s: %s" % (orows[0]["note"], " ".join(orows[0]["claim"].split())[:200])}
                new = _apply(qs, entry, topic, rows, root, st)
                qs._write(entry, root, dry, anchors=new)
                _report_changed(qs, topic, root, dry, out)
                st["notes"].append("entry %s created from %s" % (topic, orows[0]["note"]))
        except qs.SeamError as e:
            st["refused"].append("topic %s refused, nothing written: %s" % (topic, e))
            for k in ("game_added", "game_updated", "ours_added", "ours_updated"):
                st[k] = 0
    for topic, st in res.items():
        out("  seam %s: game +%d ~%d =%d, ours +%d ~%d =%d%s" % (
            topic, st["game_added"], st["game_updated"], st["game_present"], st["ours_added"], st["ours_updated"], st["ours_present"],
            "" if not dry else " (dry)"))
        for x in st["notes"]:
            out("    " + x)
        for x in st["refused"]:
            out("    REFUSED: " + x)
    return res


def main(argv=None):
    """backfill: wire the seam rows of already lodged claims files (db\\claims\\*.json) into db\\seams; the claims files are only read.
    py -I tools\\handback\\seam_wire.py [--dry] [--root DIR] [CLAIMS.json ...]   (no file: every db\\claims\\*.json)"""
    import glob
    import json
    argv = list(sys.argv[1:] if argv is None else argv)
    dry = "--dry" in argv
    root = REPO
    if "--root" in argv:
        i = argv.index("--root")
        root = os.path.abspath(argv[i + 1])
        del argv[i:i + 2]
    files = [a for a in argv if not a.startswith("--")] or sorted(glob.glob(os.path.join(root, "db", "claims", "*.json")))
    qs = load_qseam()
    for f in files:
        with open(f, encoding="utf-8") as fh:
            objs = json.load(fh)
        if not any(o.get("seam") for o in objs):
            continue
        print("== " + os.path.basename(f))
        wire(objs, root, dry, qs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
