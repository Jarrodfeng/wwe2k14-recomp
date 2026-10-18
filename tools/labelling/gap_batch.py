"""Sonnet bulk labelling (Session 10): build batches from gaps (functions a wave left unlabelled) for a re-run.

  py tools/labelling/gap_batch.py GAPS.json [GAPS.json ...] --name g01

GAPS.json = tools/labelling/wave_check.py --write output ({batch: [addresses]}). Copies each missing function's
context file from its wave batch folder into out/harness/labelling/bulk/gaps/<name>bNN/ (20 per batch) with an
index.md in the bulk format, skipping addresses that have a db entry by now, and prints the workflow batch list
({"batches": [{id, dir, ref: null, judge: false, n}]}) for the labelling-sonnet-bulk workflow.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BULK = REPO / "out" / "harness" / "labelling" / "bulk"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from wave_check import db_addresses  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("gaps", nargs="+")
    ap.add_argument("--name", required=True)
    a = ap.parse_args()
    have = db_addresses()
    todo = []
    for g in a.gaps:
        for batch, addrs in json.loads(Path(g).read_text(encoding="utf-8")).items():
            wave = batch[:3]
            for addr in addrs:
                if addr not in have:
                    todo.append((addr, BULK / wave / batch / f"{addr}.md"))
    batches = []
    for k in range(0, len(todo), 20):
        chunk = todo[k:k + 20]
        bid = f"{a.name}b{k // 20 + 1:02d}"
        d = BULK / "gaps" / bid
        d.mkdir(parents=True, exist_ok=True)
        rows = []
        for i, (addr, src) in enumerate(chunk, 1):
            shutil.copyfile(src, d / f"{addr}.md")
            rows.append(f"{i:5d}  {addr}")
        (d / "index.md").write_text(f"# {bid}: {len(chunk)} functions (gap re-run), label in this order\n\n"
                                    "order  address\n" + "\n".join(rows) + "\n", encoding="utf-8")
        batches.append({"id": bid, "dir": str(d), "ref": None, "judge": False, "n": len(chunk)})
    print(json.dumps({"batches": batches}))
    print(f"{len(todo)} gap functions in {len(batches)} batches", file=sys.stderr)


if __name__ == "__main__":
    main()
