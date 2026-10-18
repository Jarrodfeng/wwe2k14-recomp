"""Labelling pilot (Session 10, Track B): agreement table from out/harness/labelling/results/<batch>.json
(tools/labelling/collect.py). Prints a markdown table per batch and per round plus totals, and the error notes.

  py tools/labelling/report.py [--batches b01,b02] [--notes]

Columns per labeller X (A, B = Haiku; S = Sonnet settled): ok = meaning `match` or `compatible` with the
reference (Opus), exact = `match`, correct = summary fully correct against the listing, wrong, invented,
over = overconfident. A=B = settle's ab_agree. b10 also has gold (the db's existing labels): R/A/B/S vs gold.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RES = REPO / "out" / "harness" / "labelling" / "results"


def score(funcs: list[dict], k: str) -> dict:
    f = [j[k] for j in funcs if k in j]
    return {"n": len(f),
            "ok": sum(s["meaning"] in ("match", "compatible") for s in f),
            "exact": sum(s["meaning"] == "match" for s in f),
            "correct": sum(s["correct"] == "correct" for s in f),
            "wrong": sum(s["correct"] == "wrong" for s in f),
            "invented": sum(bool(s["invented"]) for s in f),
            "over": sum(s["conf"] == "over" for s in f),
            "under": sum(s["conf"] == "under" for s in f),
            "system_ok": sum(bool(s["system_ok"]) for s in f)}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--batches", default="")
    ap.add_argument("--notes", action="store_true")
    a = ap.parse_args()
    want = set(a.batches.split(",")) if a.batches else None
    rows = []
    for p in sorted(RES.glob("b*.json")):
        d = json.loads(p.read_text(encoding="utf-8"))
        if want and d["batch"] not in want or "J" not in d or "S" not in d:
            continue
        funcs = d["J"]["functions"]
        r = {"batch": d["batch"], "round": d.get("round", ""), "n": len(funcs),
             "ab": sum(bool(x["ab_agree"]) for x in d["S"]["results"])}
        for k in "ABS":
            r[k] = score(funcs, k)
        g = [j.get("gold") for j in funcs if j.get("gold")]
        if g:
            r["gold"] = {k: sum(v.get(k) in ("match", "compatible") for v in g) for k in "RABS"}
            r["gold_n"] = len(g)
        r["ref_doubts"] = sum(bool((j.get("ref_doubt") or "").strip()) for j in funcs)
        r["issues"] = d["J"].get("systematic_issues", [])
        rows.append(r)

    def line(label: str, rs: list[dict]) -> str:
        n = sum(r["n"] for r in rs)
        ab = sum(r["ab"] for r in rs)
        cells = [label, str(n), f"{ab}"]
        for k in "ABS":
            s = {m: sum(r[k][m] for r in rs) for m in ("ok", "exact", "correct", "wrong", "invented", "over", "under")}
            cells.append(f"{s['ok']} / {s['exact']} / {s['correct']} / {s['wrong']} / {s['invented']} / "
                         f"{s['over']}-{s['under']}")
        cells.append(str(sum(r["ref_doubts"] for r in rs)))
        return "| " + " | ".join(cells) + " |"

    print("| batch | n | A=B | A: ok / exact / correct / wrong / invented / over-under | B (same) | S settled (same) "
          "| ref doubts |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        print(line(f"{r['batch']} ({r['round']})", [r]))
    rounds = sorted({r["round"] for r in rows})
    for rd in rounds:
        print(line(f"**{rd} total**", [r for r in rows if r["round"] == rd]))
    print(line("**all**", rows))
    for r in rows:
        if "gold" in r:
            print(f"\n{r['batch']} vs gold (db labels, {r['gold_n']} functions): "
                  + ", ".join(f"{k} {v}" for k, v in r["gold"].items()))
    if a.notes:
        for r in rows:
            print(f"\n### {r['batch']} systematic issues")
            for i in r["issues"]:
                print(f"- {i}")


if __name__ == "__main__":
    main()
