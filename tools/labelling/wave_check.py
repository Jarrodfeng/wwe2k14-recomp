"""Sonnet bulk labelling (Session 10): which functions of the given waves have no db entry yet (gaps to re-run).

  py tools/labelling/wave_check.py w01,w02 [--write GAPS.json]

Compares the batch index files of out/harness/labelling/bulk/<wave>/ with every db/*.json file (read live) and prints,
per wave, how many of its functions are labelled, plus the missing ones with their batch. --write saves the missing
addresses as a JSON list {batch: [addresses]} (for a re-run).
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROW = re.compile(r"^\s*\d+\s+(0x[0-9A-F]{8})\b", re.M)


def db_addresses() -> set[str]:
    out = set()
    for p in (REPO / "db").glob("*.json"):
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
    ap.add_argument("waves")
    ap.add_argument("--write")
    a = ap.parse_args()
    have = db_addresses()
    gaps: dict[str, list[str]] = {}
    for w in a.waves.split(","):
        total = done = 0
        for d in sorted((REPO / "out" / "harness" / "labelling" / "bulk" / w).glob(f"{w}b*")):
            for addr in ROW.findall((d / "index.md").read_text(encoding="utf-8")):
                total += 1
                if addr in have:
                    done += 1
                else:
                    gaps.setdefault(d.name, []).append(addr)
        print(f"{w}: {done} / {total} labelled")
    for b, v in gaps.items():
        print(f"  missing in {b}: {', '.join(v)}")
    if a.write:
        Path(a.write).write_text(json.dumps(gaps, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
