"""Sonnet bulk labelling (Session 10): form fixes for db/bulk_sonnet.json (meaning unchanged).

  py tools/labelling/tidy.py list [--limits strict|validator]   flagged addresses as JSON (stdout) + counts (stderr)
  py tools/labelling/tidy.py apply JOURNAL [--dry-run]          merge a tidy workflow's rewrites into the file

Flags, with tools/db/validate.py's own patterns: a name that is not lower-case snake_case, 3 or more quoted PPC
instructions in summary + evidence, a long summary or evidence item. --limits strict (default) uses the settle
prompt's targets (summary 600, evidence 300 characters); validator uses validate.py's warning limits (800 / 600).

apply: reads tidy results ({"entries": [{address, name, summary, evidence}]}) from the journal and replaces only name,
summary and evidence of those addresses (confidence, system and the rest stay); keeps the `labels: A ...; B ...`
evidence item (re-appended when a rewrite dropped it); appends "; tidied <date>" to source. Rewrites that still fail
the validator patterns are reported and skipped.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DB = REPO / "db" / "bulk_sonnet.json"
_spec = importlib.util.spec_from_file_location("validate", REPO / "tools" / "db" / "validate.py")
V = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(V)


def flags(e: dict, summary_max: int, evidence_max: int) -> list[str]:
    out = []
    if not V.NAME_RE.match(str(e.get("name", ""))):
        out.append("name")
    ev = [x for x in (e.get("evidence") or []) if isinstance(x, str)]
    texts = [str(e.get("summary", ""))] + ev
    if sum(len(list(V.DISASM_RE.finditer(x))) for x in texts) >= V.DISASM_WARN_AT:
        out.append("disassembly")
    if len(str(e.get("summary", ""))) > summary_max:
        out.append("summary")
    if any(len(x) > evidence_max and not x.startswith("labels:") for x in ev):
        out.append("evidence")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("list")
    l.add_argument("--limits", choices=["strict", "validator"], default="strict")
    a_ = sub.add_parser("apply")
    a_.add_argument("journal")
    a_.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    entries = json.loads(DB.read_text(encoding="utf-8"))
    if a.cmd == "list":
        smax, emax = (600, 300) if a.limits == "strict" else (V.SUMMARY_MAX, V.EVIDENCE_MAX)
        flagged = {e["address"]: flags(e, smax, emax) for e in entries}
        flagged = {k: v for k, v in flagged.items() if v}
        counts = {}
        for v in flagged.values():
            for f in v:
                counts[f] = counts.get(f, 0) + 1
        print(json.dumps(sorted(flagged)))
        print(f"{len(flagged)} of {len(entries)} flagged ({a.limits}): {counts}", file=sys.stderr)
        return
    by = {e["address"]: e for e in entries}
    rewrites = {}
    for line in Path(a.journal).read_text(encoding="utf-8").splitlines():
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        v = d.get("result") if d.get("type") == "result" else None
        if isinstance(v, dict):
            for r in v.get("entries", []) or []:
                if r.get("address") in by and r.get("tidy"):
                    rewrites[r["address"]] = r
    done = skipped = 0
    for addr, r in rewrites.items():
        e = dict(by[addr])
        labels = [x for x in e.get("evidence", []) if x.startswith("labels:")]
        ev = [x for x in r.get("evidence", []) if isinstance(x, str) and not x.startswith("labels:")]
        new = dict(e, name=r.get("name", e["name"]), summary=r.get("summary", e["summary"]), evidence=ev + labels[-1:])
        still = flags(new, V.SUMMARY_MAX, V.EVIDENCE_MAX)
        if still:
            skipped += 1
            print(f"  skipped {addr}: still {still}")
            continue
        if "tidied" not in new.get("source", ""):
            new["source"] = new.get("source", "") + "; tidied 2026-10-10"
        by[addr] = new
        done += 1
    print(f"rewrites in journal: {len(rewrites)}; applied {done}, skipped {skipped}")
    if not a.dry_run:
        DB.write_text(json.dumps(sorted(by.values(), key=lambda e: e["address"]), indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8")


if __name__ == "__main__":
    main()
