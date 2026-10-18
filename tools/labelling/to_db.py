"""Sonnet bulk labelling (Session 10): settled entries from workflow journals -> db/bulk_sonnet.json.

  py tools/labelling/to_db.py JOURNAL [JOURNAL ...] [--dry-run] [--exclude-file LIST.json]

Reads every settle result ({"results": [{address, ab_agree, why, settled}]}) and every labeller result ({"entries":
[...]}, source "labeller A/B (sonnet)") in the journals (newest wins), and merges the settled entries into
db/bulk_sonnet.json (a JSON list in the db schema, sorted by address; CLAUDE.md "Function database"):
- the last evidence item must be "labels: A <name>; B <name>..." (CLAUDE.md two-label rule); when the settle pass left
  it out, it is rebuilt from the labeller results;
- addresses already in another db/*.json file are skipped (that entry wins; bulk never overwrites curated entries);
- an address listed in --exclude-file (a JSON list of addresses, e.g. judged wrong) is skipped;
- checked: 8-digit upper-case hex address, the schema fields, confidence / system values, evidence non-empty and
  required above low.
Prints what changed. Entries describe game code in our own words (no listings, no game strings): they are ours.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DB = REPO / "db"
OUT = DB / "bulk_sonnet.json"
CONF = {"high", "medium", "low", "unknown"}
SYSTEMS = {"camera", "render", "match", "assets", "audio", "io", "ui", "other"}
ADDR = re.compile(r"^0x8[0-9A-F]{7}$")


def other_db_addresses() -> set[str]:
    out = set()
    for p in DB.glob("*.json"):
        if p.name == OUT.name:
            continue
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            continue
        for e in d if isinstance(d, list) else d.get("entries", []):
            if isinstance(e, dict) and isinstance(e.get("address"), str):
                out.add(e["address"].upper().replace("0X", "0x"))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("journals", nargs="+")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--exclude-file")
    a = ap.parse_args()
    settled: dict[str, dict] = {}
    names: dict[str, dict[str, str]] = {}
    for j in a.journals:
        for line in Path(j).read_text(encoding="utf-8").splitlines():
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            v = d.get("result") if d.get("type") == "result" else None
            if not isinstance(v, dict):
                continue
            for e in v.get("entries", []) or []:
                src = str(e.get("source", "")).lower()
                k = "A" if "labeller a" in src else "B" if "labeller b" in src else None
                if k and "sonnet" in src:
                    names.setdefault(str(e.get("address", "")).upper().replace("0X", "0x"), {})[k] = e.get("name", "")
            for r in v.get("results", []) or []:
                s = r.get("settled")
                if isinstance(s, dict) and s.get("address"):
                    settled[s["address"].upper().replace("0X", "0x")] = s
    exclude = set(json.loads(Path(a.exclude_file).read_text(encoding="utf-8"))) if a.exclude_file else set()
    taken = other_db_addresses()
    current = json.loads(OUT.read_text(encoding="utf-8")) if OUT.exists() else []
    by = {e["address"]: e for e in current}
    added = replaced = skipped_db = skipped_ex = bad = 0
    problems = []
    for addr, s in sorted(settled.items()):
        if addr in taken:
            skipped_db += 1
            continue
        if addr in exclude:
            skipped_ex += 1
            continue
        e = {k: s.get(k) for k in ("address", "name", "summary", "evidence", "confidence", "system", "source")}
        e["address"] = addr
        ev = [str(x) for x in (e.get("evidence") or [])]
        if not ev or not ev[-1].startswith("labels:"):
            n = names.get(addr, {})
            ev = [x for x in ev if not x.startswith("labels:")]
            ev.append(f"labels: A {n.get('A', '?')}; B {n.get('B', '?')}")
        e["evidence"] = ev
        e["source"] = e.get("source") or "sonnet A/B + settle, 2026-10-10"
        why = []
        if not ADDR.match(addr):
            why.append("address")
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(e.get("name", ""))):
            why.append(f"name '{e.get('name')}'")
        if e.get("confidence") not in CONF:
            why.append("confidence")
        if e.get("system") not in SYSTEMS:
            why.append("system")
        if not e.get("summary"):
            why.append("summary")
        if e.get("confidence") in ("high", "medium") and len(ev) < 2:
            why.append("evidence above low")
        if why:
            bad += 1
            problems.append(f"{addr}: {', '.join(why)}")
            continue
        if addr in by:
            replaced += 1
        else:
            added += 1
        by[addr] = e
    print(f"settled entries in journals: {len(settled)}; added {added}, replaced {replaced}, skipped (other db file) "
          f"{skipped_db}, skipped (excluded) {skipped_ex}, rejected {bad}; total in {OUT.name}: {len(by)}")
    for p in problems[:20]:
        print("  rejected", p)
    if not a.dry_run:
        out = sorted(by.values(), key=lambda e: e["address"])
        OUT.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
