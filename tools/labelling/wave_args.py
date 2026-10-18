"""Sonnet bulk labelling (Session 10): the Workflow args for one or more exported waves.

  py tools/labelling/wave_args.py w01,w02 [--judge b05] > args.json

Reads out/harness/labelling/bulk/<wave>/<wave>bNN/index.md (export.py --selection ... --wave ...) and prints
{"batches": [{id, dir, ref: null, judge, n}]} for the labelling-sonnet-bulk workflow (no address lists: the
workflow checks counts, tools/labelling/wave_check.py checks addresses afterwards). judge = the batches whose id ends with --judge (default b05: 1 batch in 10).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROW = re.compile(r"^\s*\d+\s+(0x[0-9A-F]{8})\b", re.M)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("waves")
    ap.add_argument("--judge", default="b05")
    a = ap.parse_args()
    batches, addrs = [], {}
    for w in a.waves.split(","):
        for d in sorted((REPO / "out" / "harness" / "labelling" / "bulk" / w).glob(f"{w}b*")):
            found = ROW.findall((d / "index.md").read_text(encoding="utf-8"))
            if not found:
                continue
            batches.append({"id": d.name, "dir": str(d), "ref": None, "judge": d.name.endswith(a.judge),
                            "n": len(found)})
            addrs[d.name] = found
    print(json.dumps({"batches": batches}))
    print(f"{len(batches)} batches, {sum(len(v) for v in addrs.values())} functions, "
          f"{sum(b['judge'] for b in batches)} judged", file=sys.stderr)


if __name__ == "__main__":
    main()
