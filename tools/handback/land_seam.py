#!/usr/bin/env python3
r"""S111 LANDRECITE: the seam rows a landing's own edits made stale or moved are settled by the landing, not by the probe.

land_note.py calls reconcile(wt, edits, note_rel, say) on the landing worktree after the note's edits are applied (and its claims lodged):
  1. a STALE ours row (db/seams/*.json: no line of its file holds the anchor now) whose anchor line a note edit removed or rewrote:
     - the edit replaced it with exactly one new line (one line of the edit's `current` block gone, one new line in its `new` block):
       the row's anchor becomes that line's text (`occurrence` follows when the text is on several lines: the one the row had, else the
       first), and the row carries `reanchored: "re-anchored by landing <note>"`. One line is printed per row.
     - the edit only deleted it (or replaced it by several lines): the row is a STOP, printed with the topic, the row, the old anchor
       and the site; the landing exits 2 and the probe decides (re-anchor by hand in the note's claims, or drop the row).
     A stale row no edit of the note touched is not the landing's doing: one line, no stop.
Rows hold no line number (S111 SEAMNOLINE: the anchor alone locates a row), so a row whose line only MOVED needs nothing.
Every entry file written is returned (repo-relative, / separators) so land_note stages and commits it (the lodge rule: a file a tool wrote
is a file the landing commits). plant (selftest twin): "no_reanchor" skips the re-anchor of step 1.
Returns (stops, changed). Stdlib only; 0 game runs."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def load_qseam():
    spec = importlib.util.spec_from_file_location("qseam_landseam", os.path.join(REPO, "tools", "q", "qseam.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def edits_by_path(sites):
    """note_sites' sites -> {repo path: [(current text, new text)]} (edits only; ```file sites carry none)"""
    out = {}
    for s in sites:
        for cur, new in s["edits"]:
            out.setdefault(s["path"].replace("\\", "/"), []).append((cur, new))
    return out


def responsible(edits, row):
    """-> (current, new) of the first edit of the row's file whose `current` block holds the row's anchor line, or None"""
    want = str(row.get("anchor", "")).strip()
    for cur, new in edits.get(str(row.get("path", "")), []):
        if want and any(x.strip() == want for x in cur.split("\n")):
            return cur, new
    return None


def replacement(cur, new):
    """-> the ONE new line the edit put in place of the line(s) it removed, or None (a pure deletion, or several lines)"""
    cs = [x.strip() for x in cur.split("\n") if x.strip()]
    ns = [x.strip() for x in new.split("\n") if x.strip()]
    removed = [x for x in cs if x not in ns]
    added = [x for x in ns if x not in cs]
    return added[0] if len(removed) == 1 and len(added) == 1 else None


def reconcile(wt, edits, note_rel, say, plant=""):
    qs = load_qseam()
    stops, changed = [], []
    note = os.path.basename(note_rel)
    for topic in qs.topics(wt):
        p = qs.entry_path(topic, wt)
        try:
            e = qs.load(p)
        except (OSError, ValueError):
            continue
        ours = e.get("ours") if isinstance(e, dict) and isinstance(e.get("ours"), list) else []
        dirty = False
        for i, (o, st) in enumerate(zip(ours, qs._states(e, wt)), 1):
            if not isinstance(o, dict) or st["state"] != "stale":
                continue
            where = "%s [%s]" % (o.get("path"), str(o.get("anchor", "")).strip()[:60])
            hit = responsible(edits, o)
            if hit is None:
                say("land_note: seam %s ours row %d (%s) is STALE and no edit of this note touched it (it was stale before): left alone" % (topic, i, where))
                continue
            new_text = replacement(*hit)
            lines = qs._file_lines(o["path"], wt) or []
            at = [k + 1 for k, t in enumerate(lines) if new_text and t.strip() == new_text]
            if new_text is None or not at:
                why = "the edit deleted it with no single replacement line" if new_text is None else "the replacement line %r is not in the file after all the edits" % new_text
                stops.append("land_note: SEAM ROW STOPS THE LANDING: seam %s ours row %d (%s), old anchor: %s -- removed by the note's edit of %s (%s). "
                             "Next: add a claims row that re-anchors it, or drop the row from db\\seams\\%s.json with its game claim kept, then run again with --amend."
                             % (topic, i, where, o.get("anchor"), o["path"], why, topic))
                continue
            if plant == "no_reanchor":
                continue
            old_occ = o.get("occurrence") if isinstance(o.get("occurrence"), int) and o.get("occurrence") >= 1 else 1
            near = at[min(old_occ, len(at)) - 1]   # the occurrence the row had, else the last one there is
            old_anchor = str(o.get("anchor")).strip()
            qs.set_anchor_at(o, near, wt)
            o["reanchored"] = "re-anchored by landing %s" % note
            dirty = True
            say("land_note: seam %s ours row %d (%s): anchor %r rewritten by the note's edit of %s; re-anchored to %r (now at line %d)" % (
                topic, i, where, old_anchor, o["path"], new_text, near))
        if dirty:
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(qs._text(e))
            changed.append(qs._rel(p, wt))
    if changed:
        say("land_note: seam files re-written: " + ", ".join(changed))
    return stops, changed
