#!/usr/bin/env python3
r"""S111 Part 2 ABSLINT: absolute words in claim rows (DECISIONS 2026-11-26: "words like that should ONLY apply to a C claim, and be under heavy scrutiny
if only likely").

THE RULE (one definition; claims.py check_rows, lodge_lint.py and the sweep all call it).
  ABSOLUTE WORDS in the claim text (whole words, case-insensitive): only, never, always, all, every, none, cannot, "no other", "nothing else".
  Text inside a "double-quoted string", a `backtick span` or a 'single-quoted string', and any code identifier (a token holding _ or digits or
  0x, e.g. all_callers, sub_82331BC0) is not read. A COUNTED SET is not read either: all / every / none followed by a number ("58 at all four
  call sites", "none of the nine records", "all 4 callers"): it names its own scope, the count is the claim.
  - mark L / U with an absolute word: FAIL (a likely or open claim cannot say "only": say what was searched, or drop the word).
  - mark C with an absolute word: WARN unless the evidence names a COMPLETE search. COMPLETE_RE (the simple, testable rule): the evidence holds one of
        "all callers|refs|references|writers|stores|readers|sites|users"   "N of N" (4 of 4, the same N: 4 of 34 is a partial read)   "--stores" /
        "--writes" / "--reads" (q field flags: the index lists every access)   "callers ... total" / "refs ... total"   "exhaustive"
        "N callers|refs|stores|writes|readers|sites"  (a counted list; not the denominator after "of": "4 of 34 refs")
  - `scope=TEXT` on the row (the last field; the text is free, e.g. `scope=4 of 34 refs read`) opts out: it names what was searched, the absolute
    word then reads as "within that scope". An empty scope is refused by claims.py.
NO BASELINE (S111 ABSFIX: the 41 grandfathered [L] / [U] rows were reworded, scoped or re-marked [C] in their notes and the baseline deleted): every
[L] / [U] row with an absolute word FAILS, so a lodged [C] row re-marked [L] with its text unchanged FAILS too. Stdlib only; 0 game runs.

  py -I tools\handback\abslint.py                 # lint db\claims: any [L] / [U] finding FAILS (exit 1), [C] findings are counted as WARN
  py -I tools\handback\abslint.py --sweep [OUT]   # write the sweep list (default out\harness\abslint_sweep.md), grouped by note
  py -I tools\handback\abslint.py --selftest"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

ABS_RE = re.compile(r"(?i)(?<![\w-])(only|never|always|all|every|none|cannot|no other|nothing else)(?![\w-])")
COMPLETE_RE = re.compile(
    r"(?i)(\ball\s+(?:\w+\s+)?(?:callers|refs|references|writers|stores|readers|sites|users)\b"
    r"|\b(?P<n>\d+)\s+of\s+(?P=n)\b"
    r"|--stores|--writes|--reads"
    r"|\b(?:callers?|refs?)\b[^|]*\btotal\b"
    r"|\bexhaustive"
    r"|(?<!\bof )\b\d+\s+(?:callers|refs|stores|writes|readers|sites)\b)")
_QUOTED = re.compile(r"\"[^\"]*\"|`[^`]*`|(?<![\w])'[^'\s][^']*'(?![\w])")
_COUNTED = re.compile(r"(?i)(?<![\w-])(?:all|every|none)\s+(?:of\s+)?(?:the\s+|its\s+|these\s+)?"
                      r"(?:\d+|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve)(?![\w-])")
_IDENT = re.compile(r"\S*[_\d]\S*")


def strip_literals(text):
    """claim text without quoted strings, backtick spans, counted sets (all four, none of the nine) and code-identifier tokens."""
    return _IDENT.sub(" ", _COUNTED.sub(" ", _QUOTED.sub(" ", text)))


def absolute_words(claim):
    return [m.group(1).lower() for m in ABS_RE.finditer(strip_literals(claim))]


def complete_search(evidence):
    return bool(COMPLETE_RE.search(evidence or ""))


def judge(mark, claim, evidence, scope=None):
    """-> (level, message): level 'FAIL' / 'WARN' / None."""
    if scope:
        return None, ""
    w = absolute_words(claim)
    if not w:
        return None, ""
    words = ", ".join(sorted(set(w)))
    if mark in ("L", "U"):
        return "FAIL", "[%s] claim uses absolute word(s) %s: a likely / open claim cannot say that; name what was searched (scope=...) or drop the word" % (mark, words)
    if not complete_search(evidence):
        return "WARN", "[C] claim uses absolute word(s) %s but the evidence names no complete search (all callers / N of N / --stores / --writes / callers with a total), add one or scope=..." % words
    return None, ""


def db_rows(root):
    """-> [(file rel, index, row)] of every db\\claims file."""
    out = []
    for p in sorted(glob.glob(os.path.join(root, "db", "claims", "*.json"))):
        try:
            with open(p, encoding="utf-8-sig") as f:
                rows = json.load(f)
        except (OSError, ValueError):
            continue
        for i, r in enumerate(rows if isinstance(rows, list) else []):
            if isinstance(r, dict):
                out.append((os.path.basename(p), i, r))
    return out


def findings(root):
    """-> [(level, note, row, message)] over db\\claims (level as judge)."""
    out = []
    for fn, i, r in db_rows(root):
        lv, msg = judge(r.get("mark"), r.get("claim", ""), r.get("evidence", ""), r.get("scope"))
        if lv:
            out.append((lv, r.get("note") or fn, r, msg))
    return out


def lint(root):
    """-> (FAIL lines for the [L] / [U] findings, number of [C] WARNs)"""
    fails, warns = [], 0
    for lv, note, r, msg in findings(root):
        if lv != "FAIL":
            warns += 1
        else:
            fails.append("FAIL %s %s: %s" % (note, r.get("address"), msg))
    return fails, warns


def sweep(root, out):
    fs = findings(root)
    by = {}
    for lv, note, r, msg in fs:
        by.setdefault(note, []).append((lv, r))
    nl = sum(1 for lv, _, _, _ in fs if lv == "FAIL")
    nc = len(fs) - nl
    lines = ["# ABSLINT sweep of db\\claims (absolute words in claims)", "",
             "Generated by `py -I tools\\handback\\abslint.py --sweep`. Rows: %d. [L]/[U] with an absolute word (would FAIL): %d. [C] with an absolute word and no complete-search evidence (WARN): %d. Notes: %d." % (len(db_rows(root)), nl, nc, len(by)),
             "Fix in the NOTE's claims block (reword, add `scope=`, or add the complete search to the evidence), then lodge; db\\claims is never hand-edited.", ""]
    for note in sorted(by):
        lines.append("## %s" % note)
        for lv, r in by[note]:
            lines.append("- %s %s [%s] words: %s | claim: %s | evidence: %s" % (
                "FAIL" if lv == "FAIL" else "WARN", r.get("address"), r.get("mark"), ", ".join(sorted(set(absolute_words(r.get("claim", ""))))),
                r.get("claim", "").replace("\n", " "), r.get("evidence", "")))
        lines.append("")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return nl, nc, len(by)


def selftest():
    bad = []
    ncase = [0]

    def chk(c, m):
        ncase[0] += 1
        if not c:
            bad.append(m)
    chk(judge("L", "this is the only writer of the flag", "decomp sub_X")[0] == "FAIL", "[L] only must FAIL")
    chk(judge("U", "it never runs in a match", "")[0] == "FAIL", "[U] never must FAIL")
    chk(judge("L", "the only writer among 4 of 34 refs read", "x", scope="4 of 34 refs read")[0] is None, "scope= row must be honest")
    chk(judge("C", "the only writer of the flag", "decomp sub_X +0x40")[0] == "WARN", "[C] only without complete search must WARN")
    chk(judge("C", "the only writer of the flag", "field 1176 --stores (all 7 stores read)")[0] is None, "[C] with --stores passes")
    chk(judge("C", "it is called from every menu", "4 of 4 callers read")[0] is None, "[C] N of N passes")
    chk(judge("C", "it is called from every menu", "callers sub_X (12 total)")[0] is None, "[C] callers total passes")
    chk(judge("L", "the flag is set when the \"only\" button is held", "x")[0] is None, "quoted word is not read")
    chk(judge("L", "reads `all` entries of all_callers and sub_82331BC0", "x")[0] is None, "code spans / identifiers are not read")
    chk(judge("L", "the table holds ten entries", "x")[0] is None, "a plain claim passes")
    chk(judge("L", "Only the first runs", "x")[0] == "FAIL", "case-insensitive")
    chk(judge("L", "there is no other caller, nothing else writes it", "x")[0] == "FAIL", "phrases")
    chk(judge("L", "the onlyness of it, an overall total", "x")[0] is None, "whole words only")
    # S111 ABSLINT review: a partial read is not a complete search (N of N means the same N; "34 refs" after "of" is the denominator)
    chk(judge("C", "the only writer of the flag", "4 of 34 refs read")[0] == "WARN", "[C] with a partial read (4 of 34 refs) must WARN")
    chk(judge("C", "the only writer of the flag", "q vtable --of X (slot 42 of 5 vtables)")[0] == "WARN", "[C] with 'slot 42 of 5' must WARN")
    chk(judge("C", "the only writer of the flag", "2601 of 2615 records")[0] == "WARN", "[C] with 2601 of 2615 must WARN")
    chk(judge("C", "the only writer of the flag", "q callers X (6 sites)")[0] is None, "[C] with a counted list passes")
    # a counted set names its own scope ("all four call sites", "none of the nine records"); the bare word still FAILS
    chk(judge("L", "58 at all four call sites", "x")[0] is None, "[L] 'all four' is a counted set")
    chk(judge("L", "answers 1 when none of the nine records is loading", "x")[0] is None, "[L] 'none of the nine' is a counted set")
    chk(judge("L", "all 4 callers pass 58", "x")[0] is None, "[L] 'all 4' is a counted set")
    chk(judge("L", "all callers pass 58", "x")[0] == "FAIL", "[L] 'all' with no count must FAIL")
    chk(judge("L", "it runs on every frame of all four pods", "x")[0] == "FAIL", "[L] a bare 'every' beside a counted set must FAIL")
    chk(judge("L", "that bank loads only in the Create-a-Story modes", "GMS.md")[0] == "FAIL", "[L] PODGLASS's sentence must FAIL")
    # no baseline: a lodged [C] row re-marked [L] with the same text (the PODGLASS correction) FAILS, and lint() counts the two levels apart
    chk(judge("L", "the only writer of the flag", "field 1176 --stores (all 7 stores read)")[0] == "FAIL", "[L] with complete-search evidence still FAILS (the evidence only helps a [C])")
    chk(judge("U", "the only writer of the flag", "")[0] == "FAIL", "[U] FAILS")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "db", "claims"))
        with open(os.path.join(td, "db", "claims", "n.json"), "w", encoding="utf-8") as f:
            f.write('[{"note": "n.md", "address": "0x1", "mark": "L", "claim": "the only writer", "evidence": "x"},'
                    ' {"note": "n.md", "address": "0x2", "mark": "C", "claim": "the only writer", "evidence": "x"},'
                    ' {"note": "n.md", "address": "0x3", "mark": "L", "claim": "a writer found", "evidence": "x"}]')
        fl, wn = lint(td)
        chk(len(fl) == 1 and wn == 1, "lint(): one [L] FAIL and one [C] WARN, no grandfathering (got %d FAIL, %d WARN)" % (len(fl), wn))
    # ABSFIX review twin: a row the deleted baseline held (MAT1's pre-ABSFIX text, key docs/research/s111/MAT1.md|740e5f4ac436) FAILS again;
    # the pre-removal lint() counted it as baselined (0 FAIL), so this case is the one that fails if grandfathering comes back
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "db", "claims"))
        with open(os.path.join(td, "db", "claims", "s111_MAT1.json"), "w", encoding="utf-8") as f:
            json.dump([{"note": "docs/research/s111/MAT1.md", "address": "0x825AB3C8", "mark": "L", "evidence": "x",
                        "claim": "the spec power cubes of a character are rebaked only when asked or when its light rows change in mode 0"}], f)
        fl = lint(td)[0]
        chk(len(fl) == 1, "lint(): a row the deleted baseline grandfathered FAILS (got %d FAIL)" % len(fl))
    for m in bad:
        print("abslint selftest FAIL: " + m)
    print("abslint selftest: %s" % ("FAIL (%d of %d)" % (len(bad), ncase[0]) if bad else "OK (%d cases)" % ncase[0]))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--sweep", nargs="?", const="out/harness/abslint_sweep.md")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    if a.selftest:
        return selftest()
    if a.sweep:
        nl, nc, nn = sweep(root, os.path.join(root, a.sweep) if not os.path.isabs(a.sweep) else a.sweep)
        print("abslint sweep: %d [L]/[U] rows with absolute words, %d [C] rows without complete-search evidence, in %d notes -> %s" % (nl, nc, nn, a.sweep))
        return 0
    fails, warns = lint(root)
    for x in fails:
        print(x)
    print("abslint: %d FAIL, %d [C] WARN (not blocking; --sweep lists them)" % (len(fails), warns))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
