#!/usr/bin/env python3
r"""S110 NOTE_SITES: check and apply the `Sites the build will touch` section of a settle note, mechanically.

  py -I tools\handback\note_sites.py --check NOTE [--root DIR]
  py -I tools\handback\note_sites.py --apply NOTE [--root DIR]
  py -I tools\handback\note_sites.py --template

FORMAT (one fixed format; docs\agents\note_sites.md repeats it). The section is the heading "Sites the build will touch" (any level 1-6,
case-insensitive) up to the next heading of the same or a higher level. Inside it each site is a heading line

  ### Site N: <repo-relative path>            (optionally  <path>:<line>  : the line is a hint only, never used)

followed by EITHER one or more edit pairs OR one new file:
  - an edit pair: a fenced block opened by a line "```current" holding the exact current text (never empty; an insertion quotes the line it
    goes after or before), then a fenced block opened by "```new" holding the replacement in full (an insertion repeats the quoted line).
    Several pairs may follow one heading (same file); each is checked against the file as the pairs above it left it.
  - a new file: ONE fenced block opened by "```file" holding the whole file.
Fence lines start at column 0. A fence of four or more backticks carries text that holds triple backticks: it closes only on a line of the
same or a longer run of backticks. Anything else in the section (prose, other fences, the proof commands) is ignored; a heading that is not
a site heading ends the current site. The carried text has no trailing newline of its own: lines are joined with LF.

--check: per site OK / OK (already applied) / NOT FOUND / AMBIGUOUS / ERROR. Texts are compared after normalising CRLF to LF on both sides.
  `current` must occur in the file EXACTLY ONCE. A `file` site must not exist yet (or be identical: already applied).
  Last line: `note_sites: N sites, M edits, K new files: OK|FAIL`; exit 0 only on OK.
--apply: runs the check first and changes NOTHING unless every site is OK; then writes the edits (each file keeps its own line endings; a file
  with mixed endings is refused) and the new files (folders created). A second --apply changes nothing ("already applied").
--template: prints an empty, commented example of the section.
NOTHING TO CHANGE: a probe whose honest answer is "no edits" writes, inside the section, ONE line `No sites: <reason>` (case-insensitive, column
  0, a non-empty reason) and no `### Site` heading. --check prints `note_sites: 0 sites: OK (no sites: <reason>)` and exits 0; --apply changes
  nothing and exits 0; land_note.py refuses it (exit 7: nothing to land). An empty reason, or that line together with a site heading, is an error.
Stdlib only; reads the note and the files it names; 0 game runs."""
import argparse
import difflib
import os
import re
import sys

SECTION_RE = re.compile(r"^(#{1,6})\s+sites the build will touch\b", re.I)
HEADING_RE = re.compile(r"^(#{1,6})\s+\S")
SITE_RE = re.compile(r"^#{1,6}\s+Site\s+\d+\s*:\s*(.+?)\s*$")
NOSITES_RE = re.compile(r"^no sites:(.*)$", re.I)
FENCE_RE = re.compile(r"^(\s*)(`{3,})\s*([A-Za-z]*)\s*$")
LINEHINT_RE = re.compile(r":\d+(-\d+)?$")

TEMPLATE = """## Sites the build will touch

<!-- note_sites format (docs/agents/note_sites.md): one "### Site N: path" heading per file, then fenced blocks at column 0.
     An edit = a ```current block (exact current text, every line to be replaced, never empty) then a ```new block (full replacement).
     An insertion quotes the line it goes after/before in current and repeats it in new. A new file = ONE ```file block (the whole file).
     Text that contains triple backticks: open and close that block with four (or more) backticks. -->

### Site 1: src/example/thing.h:120
```current
  int a = 1;
  int b = 2;
```
```new
  int a = 1;
  int b = 3;
```

### Site 2: tools/example/new_unit.cpp
```file
// the whole new file
int main() { return 0; }
```

Nothing to change? Replace the sites by ONE line at column 0 (and no "### Site" heading): No sites: <the reason, not empty>

The proof (optional; tools/handback/land_note.py runs it): ONE ```proof block, prove.py's file format, one command per line:
```proof
py -I tools\\example\\thing_selftest.py
compile
lints
```
"""


class NoteError(Exception):
    pass


class NoSites(NoteError):
    """The section holds `No sites: <reason>`: the note's honest answer is 'nothing to change'."""
    def __init__(self, reason):
        NoteError.__init__(self, "no sites: " + reason)
        self.reason = reason


def parse_note(text):
    """-> list of sites {path, edits: [(cur, new)], file: str|None, line: int}. Raises NoteError."""
    return parse_note_full(text)[0]


def parse_proof(text):
    """-> the note's ```proof block text (prove.py's file format) or None. Raises NoteError like parse_note."""
    return parse_note_full(text)[1]


def parse_note_full(text):
    """-> (sites, proof|None). A ```proof block (column 0, anywhere in the section) holds one prove.py command per line; validated
    (not empty, at most one) and otherwise ignored by check / apply."""
    proof = None
    lines = text.lstrip("﻿").replace("\r\n", "\n").split("\n")
    sites, cur_site, in_sec, sec_level = [], None, False, 0
    i, n = 0, len(lines)
    found = False
    nosites = []  # (line, reason) of each `No sites:` line inside the section
    pend_cur = None  # (text, line) of a `current` block waiting for its `new`
    def close_site():
        nonlocal cur_site, pend_cur
        if pend_cur is not None:
            raise NoteError("line %d: a ```current block has no ```new block after it" % pend_cur[1])
        cur_site = None
    while i < n:
        ln = lines[i]
        fm = FENCE_RE.match(ln)
        if fm:
            indent, ticks, tag = fm.groups()
            j = i + 1
            while j < n:
                cm = FENCE_RE.match(lines[j])
                if cm and cm.group(3) == "" and len(cm.group(2)) >= len(ticks):
                    break
                j += 1
            if j >= n:
                raise NoteError("line %d: fence is never closed (a block carrying triple backticks needs a four-backtick fence)" % (i + 1))
            body = "\n".join(lines[i + 1:j])
            if in_sec and indent == "" and tag.lower() == "proof":
                if proof is not None:
                    raise NoteError("line %d: two ```proof blocks" % (i + 1))
                if not [x for x in body.split("\n") if x.strip() and not x.strip().startswith("#")]:
                    raise NoteError("line %d: empty ```proof block (one prove.py command per line)" % (i + 1))
                proof = body + "\n"
            elif in_sec and cur_site is not None and indent == "" and tag.lower() in ("current", "new", "file"):
                tag = tag.lower()
                if tag == "current":
                    if pend_cur is not None:
                        raise NoteError("line %d: two ```current blocks in a row" % (i + 1))
                    if cur_site["file"] is not None:
                        raise NoteError("line %d: site %r mixes a file block and an edit" % (i + 1, cur_site["path"]))
                    if body.strip() == "":
                        raise NoteError("line %d: empty ```current block (an insertion quotes the line it goes after or before)" % (i + 1))
                    pend_cur = (body, i + 1)
                elif tag == "new":
                    if pend_cur is None:
                        raise NoteError("line %d: ```new block without a ```current block before it" % (i + 1))
                    cur_site["edits"].append((pend_cur[0], body))
                    pend_cur = None
                else:
                    if cur_site["edits"] or pend_cur is not None or cur_site["file"] is not None:
                        raise NoteError("line %d: site %r has a ```file block and another block" % (i + 1, cur_site["path"]))
                    cur_site["file"] = body + "\n"
            i = j + 1
            continue
        nm = NOSITES_RE.match(ln)
        if nm and in_sec:
            nosites.append((i + 1, nm.group(1).strip()))
        hm = HEADING_RE.match(ln)
        if hm:
            level = len(hm.group(1))
            if in_sec and level <= sec_level:
                close_site()
                in_sec = False
            sm = SECTION_RE.match(ln)
            if sm and not in_sec:
                in_sec, sec_level, found = True, len(sm.group(1)), True
                close_site()
            elif in_sec:
                close_site()
                s = SITE_RE.match(ln)
                if s:
                    path = LINEHINT_RE.sub("", s.group(1).strip().strip("`"))
                    cur_site = {"path": path, "edits": [], "file": None, "line": i + 1}
                    sites.append(cur_site)
        i += 1
    close_site()
    if not found:
        raise NoteError("section 'Sites the build will touch' not found")
    if nosites:
        if sites:
            raise NoteError("line %d: a `No sites:` line together with a '### Site' heading: keep one of them" % nosites[0][0])
        if len(nosites) > 1:
            raise NoteError("line %d: more than one `No sites:` line" % nosites[1][0])
        if not nosites[0][1]:
            raise NoteError("line %d: `No sites:` needs a reason after the colon" % nosites[0][0])
        raise NoSites(nosites[0][1])
    if not sites:
        raise NoteError("section found, but it has no '### Site N: <path>' heading (nothing to change? write one line: `No sites: <reason>`)")
    for s in sites:
        if not s["edits"] and s["file"] is None:
            raise NoteError("line %d: site %r has no ```current/```new pair and no ```file block" % (s["line"], s["path"]))
    return sites, proof


def read_file(path):
    with open(path, "rb") as f:
        raw = f.read().decode("utf-8", "surrogateescape")
    crlf = "\r\n" in raw
    mixed = crlf and re.search(r"(?<!\r)\n", raw) is not None
    return raw.replace("\r\n", "\n"), ("\r\n" if crlf else "\n"), mixed


def first_diff(a, b):
    k = 0
    while k < len(a) and k < len(b) and a[k] == b[k]:
        k += 1
    return k


def closest(cur, text):
    """A short report for a `current` text that does not occur: the closest file line and the first differing character."""
    flines = text.split("\n")
    fset = set(flines)
    for idx, cl in enumerate(cur.split("\n")):
        if cl not in fset:
            best = difflib.get_close_matches(cl, flines, n=1, cutoff=0.4)
            if best:
                ln = flines.index(best[0]) + 1
                k = first_diff(cl, best[0])
                return ("note line %d of the block differs from file line %d at column %d (note has %r, file has %r)\n        note: %s\n        file: %s"
                        % (idx + 1, ln, k + 1, cl[k:k + 1], best[0][k:k + 1], cl, best[0]))
            return "note line %d of the block (%s) is close to no line of the file" % (idx + 1, cl)
    return "every line of the block exists in the file, but not as these consecutive lines (order, blank lines or a missing line)"


def check_sites(sites, root):
    """-> (results, plans). results: per site (status, [messages]); status OK / APPLIED / FAIL.
    plans: per site ('edit', path, newtext, eol) / ('file', path, text) / None (already applied or failed)."""
    results, plans, seen_new = [], [], set()
    for s in sites:
        rel = s["path"].replace("\\", "/")
        full = os.path.normpath(os.path.join(root, rel))
        msgs, status, plan = [], "OK", None
        if os.path.isabs(rel) or not os.path.abspath(full).startswith(os.path.abspath(root)):
            results.append(("FAIL", ["ERROR: path %r is not repo-relative" % s["path"]]))
            plans.append(None)
            continue
        if rel in seen_new:
            status, msgs = "FAIL", ["ERROR: the path has two site headings: put all its pairs under one heading"]
        elif s["file"] is not None:
            if os.path.exists(full):
                old, _, _ = read_file(full)
                if old == s["file"]:
                    status, msgs = "APPLIED", ["OK (already applied: the file is identical)"]
                else:
                    status, msgs = "FAIL", ["ERROR: the file already exists and differs (a new file must not exist yet)"]
            else:
                msgs = ["OK (new file, %d lines)" % s["file"].count("\n")]
                plan = ("file", full, s["file"])
        elif not os.path.isfile(full):
            status, msgs = "FAIL", ["NOT FOUND: the file does not exist"]
        else:
            text, eol, mixed = read_file(full)
            if mixed:
                status, msgs = "FAIL", ["ERROR: the file has mixed line endings (CRLF and LF); fix it first"]
            else:
                changed, applied = False, 0
                for k, (cur, new) in enumerate(s["edits"], 1):
                    tag = "edit %d" % k
                    nnew = text.count(new)
                    ncur = text.count(cur)
                    # applied already: the new text is there and the old is gone (or the new text contains the old: an insertion)
                    if nnew >= 1 and (ncur == 0 or cur in new):
                        msgs.append("%s: OK (already applied)" % tag)
                        applied += 1
                        continue
                    count = ncur
                    if count > 1:  # TWIN:ambiguous
                        status = "FAIL"
                        msgs.append("%s: AMBIGUOUS (%d matches): quote more lines in ```current so it occurs once" % (tag, count))
                    elif count == 0:
                        status = "FAIL"
                        msgs.append("%s: NOT FOUND: %s" % (tag, closest(cur, text)))
                    else:
                        msgs.append("%s: OK (1 match)" % tag)
                        text = text.replace(cur, new, 1)
                        changed = True
                if status == "OK":
                    if changed:
                        plan = ("edit", full, text, eol)
                    elif applied == len(s["edits"]):
                        status = "APPLIED"
        seen_new.add(rel)
        results.append((status, msgs))
        plans.append(plan if status == "OK" else None)
    return results, plans


def report(sites, results):
    for s, (status, msgs) in zip(sites, results):
        print("%s  %s" % ("OK  " if status in ("OK", "APPLIED") else "FAIL", s["path"]))
        for m in msgs:
            print("      " + m)
    nedit = sum(len(s["edits"]) for s in sites)
    nfile = sum(1 for s in sites if s["file"] is not None)
    ok = all(st in ("OK", "APPLIED") for st, _ in results)
    print("note_sites: %d sites, %d edits, %d new files: %s" % (len(sites), nedit, nfile, "OK" if ok else "FAIL"))
    return ok


def write_text(path, text, eol):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = text.replace("\n", eol).encode("utf-8", "surrogateescape")
    with open(path, "wb") as f:
        f.write(data)


def main(argv=None):
    ap = argparse.ArgumentParser(description="check / apply the 'Sites the build will touch' section of a note")
    ap.add_argument("--check", metavar="NOTE")
    ap.add_argument("--apply", metavar="NOTE")
    ap.add_argument("--template", action="store_true")
    ap.add_argument("--root", default=".", help="repo root the site paths are relative to (default: the current folder)")
    a = ap.parse_args(argv)
    if a.template:
        sys.stdout.write(TEMPLATE)
        return 0
    note = a.check or a.apply
    if not note or (a.check and a.apply):
        ap.error("give exactly one of --check NOTE, --apply NOTE, --template")
    root = os.path.abspath(a.root)
    try:
        with open(note, "rb") as f:
            sites = parse_note(f.read().decode("utf-8", "replace"))
    except NoSites as e:
        print("note_sites: 0 sites: OK (no sites: %s)" % e.reason)
        if a.apply:
            print("note_sites: nothing to apply, nothing changed")
        return 0
    except (NoteError, OSError) as e:
        print("note_sites: ERROR: %s" % e)
        print("note_sites: 0 sites, 0 edits, 0 new files: FAIL")
        return 1
    results, plans = check_sites(sites, root)
    ok = report(sites, results)
    if a.check:
        return 0 if ok else 1
    all_ok = ok
    if not all_ok:  # TWIN:refuse
        print("note_sites: apply REFUSED, nothing changed")
        return 1
    done = 0
    for p in plans:
        if p is None:
            continue
        if p[0] == "edit":
            write_text(p[1], p[2], p[3])
        else:
            write_text(p[1], p[2], "\n")
        print("changed: " + os.path.relpath(p[1], root))
        done += 1
    if done == 0:
        print("note_sites: already applied, nothing changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
