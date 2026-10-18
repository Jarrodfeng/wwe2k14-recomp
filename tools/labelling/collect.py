"""Labelling pilot (Session 10, Track B): pull one workflow run's agent results out of its journal.jsonl into
out/harness/labelling/results/<batch>.json = {batch, A, B, R, S, J} (the newest complete result of each kind wins:
reruns and cached replays repeat results). Local only: entries describe game code.

  py tools/labelling/collect.py JOURNAL [--round r1] [--suffix _v3]

Kinds are recognised by shape: labeller entries by their `source` (labeller A / labeller B / reference), the settle
result by `results`, the judge result by `functions`; the batch by the addresses (out/harness/labelling/batches).
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BATCHES = REPO / "out" / "harness" / "labelling" / "batches"
OUT = REPO / "out" / "harness" / "labelling" / "results"


def batch_addresses() -> dict[str, list[str]]:
    return {d.name: re.findall(r"^(0x[0-9A-F]{8})\b", (d / "index.md").read_text(encoding="utf-8"), re.M)
            for d in sorted(BATCHES.glob("b*"))}


def kind_of(v: dict) -> tuple[str, list[str]] | None:
    if "entries" in v and v["entries"]:
        src = str(v["entries"][0].get("source", "")).lower()
        k = "A" if "labeller a" in src else "B" if "labeller b" in src else "R" if "reference" in src else None
        return (k, [e.get("address", "") for e in v["entries"]]) if k else None
    if "results" in v:
        return "S", [r.get("address", "") for r in v["results"]]
    if "functions" in v:
        return "J", [f.get("address", "") for f in v["functions"]]
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("journal")
    ap.add_argument("--round", default="")
    ap.add_argument("--suffix", default="", help="file name suffix, e.g. _v3 for a rerun of a batch")
    a = ap.parse_args()
    want = batch_addresses()
    by_addr = {tuple(v): k for k, v in want.items()}
    got: dict[str, dict] = {}
    for line in Path(a.journal).read_text(encoding="utf-8").splitlines():
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get("type") != "result" or not isinstance(d.get("result"), dict):
            continue
        v = d["result"]
        kd = kind_of(v)
        if not kd or not kd[0]:
            continue
        k, addrs = kd
        b = by_addr.get(tuple(a_.upper().replace("0X", "0x") for a_ in addrs))
        if not b:
            continue  # incomplete or misordered result (rerun replaced it)
        got.setdefault(b, {"batch": b})[k] = v
    OUT.mkdir(parents=True, exist_ok=True)
    for b, rec in sorted(got.items()):
        rec["round"] = a.round
        (OUT / f"{b}{a.suffix}.json").write_text(json.dumps(rec, indent=1), encoding="utf-8")
        print(f"{b}: " + " ".join(k for k in "ABRSJ" if k in rec))


if __name__ == "__main__":
    main()
