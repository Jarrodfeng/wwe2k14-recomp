"""Labelling pilot (Session 10, Track B): tokens and time per stage from the workflow agent transcripts.

  py tools/labelling/cost.py WORKFLOW_DIR [WORKFLOW_DIR ...]

WORKFLOW_DIR = a run folder under ~/.claude/projects/<project>/<session>/subagents/workflows/wf_*. Each agent there has
agent-<id>.meta.json (its label, e.g. "A:b03", "ref:b03", "settle:b03", "judge:b03", and model) and agent-<id>.jsonl
(one line per message with `usage`). Prints, per stage (A, B, ref, settle, judge): agents, input tokens (incl. cache
reads / writes), output tokens, and the median wall time per agent (first to last message), plus per-function
figures (20 functions per batch). Retries (label ":retry") count with their stage.
"""
from __future__ import annotations

import json
import statistics
import sys
from datetime import datetime
from pathlib import Path


def stage_of(label: str) -> str:
    head = label.split(":")[0]
    return {"A": "labeller A", "B": "labeller B", "ref": "reference", "settle": "settle", "judge": "judge",
            "tidy": "tidy"}.get(head, head)  # the models column shows which model each stage really ran on


def main() -> None:
    stats: dict[str, dict] = {}
    batches = set()
    for d in sys.argv[1:]:
        for meta in Path(d).glob("agent-*.meta.json"):
            m = json.loads(meta.read_text(encoding="utf-8"))
            label = m.get("description", "")
            st = stats.setdefault(stage_of(label), {"agents": 0, "in": 0, "out": 0, "fresh": 0, "cw": 0, "cr": 0, "secs": [], "models": set()})
            if ":" in label:
                batches.add((d, label.split(":")[1]))
            st["agents"] += 1
            t0 = t1 = None
            for line in meta.with_name(meta.name.replace(".meta.json", ".jsonl")).read_text(encoding="utf-8").splitlines():
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ts = e.get("timestamp")
                if ts:
                    t = datetime.fromisoformat(ts.replace("Z", "+00:00"))
                    t0 = t0 or t
                    t1 = t
                msg = e.get("message")
                if isinstance(msg, dict) and msg.get("usage"):
                    u = msg["usage"]
                    st["in"] += u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0) + \
                        u.get("cache_read_input_tokens", 0)
                    st["out"] += u.get("output_tokens", 0)
                    st["fresh"] += u.get("input_tokens", 0)
                    st["cw"] += u.get("cache_creation_input_tokens", 0)
                    st["cr"] += u.get("cache_read_input_tokens", 0)
                    if msg.get("model"):
                        st["models"].add(msg["model"])
            if t0 and t1:
                st["secs"].append((t1 - t0).total_seconds())
    nfun = 20 * len(batches)
    print(f"{len(batches)} batch runs = {nfun} function labels per stage")
    print("| stage | agents | input tokens | of which cache reads | cache writes | output tokens | per function in / out "
          "| median min per agent | models |")
    print("|---|---|---|---|---|---|---|---|---|")
    for k in sorted(stats):
        s = stats[k]
        med = statistics.median(s["secs"]) / 60 if s["secs"] else 0
        print(f"| {k} | {s['agents']} | {s['in']:,} | {100 * s['cr'] // max(s['in'], 1)}% | {s['cw']:,} | {s['out']:,} | "
              f"{s['in'] // max(nfun, 1):,} / "
              f"{s['out'] // max(nfun, 1):,} | {med:.1f} | {', '.join(sorted(s['models']))} |")


if __name__ == "__main__":
    main()
