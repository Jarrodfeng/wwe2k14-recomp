#!/usr/bin/env python3
"""Where ONE agent's wall time went (S110, Jarrod: "a full breakdown of a review lane, I'd like to know what it spent time doing").

    py -I tools\\harness\\agent_timeline.py --session <session id> --agent <agent id> [--slow 12]

Reads the agent's own transcript (projects\\<project>\\<session>\\subagents\\agent-<id>.jsonl) and splits its wall time into:
  tool time   = from the reply that issued a tool call to the tool's result (the tool running: a compile, a test, a read)
  model time  = from a result (or a message to the agent) to the agent's next reply (it reading the result and writing the next step)
  parked      = gaps over PARK seconds with no tool pending (the agent had ended a turn: waiting for a message or a background job)
Each tool call is put in ONE kind (read game code, read our code, edit, compile, unit / test run, build, git, wait / poll, other).
Rounds are cut at each message delivered to the agent (its brief, then every follow-up from the main session).
Prints totals only: no transcript text beyond the first words of each message that opened a round and of the slowest commands.
"""
import argparse, json, os, re, sys
from datetime import datetime
from pathlib import Path

PARK = 90  # s: a model gap longer than this with no tool pending is the agent parked, not thinking


def ts(s):
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


def kind_of(name, inp):
    cmd = ""
    if isinstance(inp, dict):
        cmd = str(inp.get("command") or "")
    low = cmd.lower()
    if name in ("Edit", "Write", "NotebookEdit"):
        return "edit"
    if name == "Read":
        return "read our files"
    if name in ("Grep", "Glob"):
        return "search our files"
    if name in ("Bash", "PowerShell"):
        if re.search(r"q\.py\s+(multi\s+)?[\"']?(decomp|field|fn|callers|callees|str|imm|addr|vtable)", low) or "decomp sub_" in low:
            return "read game code (q)"
        if "q.py" in low:
            return "search our code (q)"
        if "game_syntax" in low or "-fsyntax-only" in low:
            return "compile check"
        if "build_test.ps1" in low or "build_viewer" in low or "build_imp_test" in low or "clang++" in low or "build.ps1" in low:
            return "build"
        if "selftest" in low or "_unit" in low or "importer_test" in low or "imp_test" in low or "check.py" in low or "validate.py" in low:
            return "unit / test run"
        if re.search(r"\bgit\s+(diff|log|show|status|worktree|merge-base|rev-parse|branch)", low):
            return "git read"
        if re.search(r"\bgit\s+(add|commit|stash|checkout|archive)", low):
            return "git write"
        if "start-sleep" in low or re.search(r"\bsleep\b", low) or "wait-" in low:
            return "wait / poll"
        if re.search(r"\b(grep|rg|findstr|select-string)\b", low):
            return "search our files"
        return "shell other"
    if name in ("TaskOutput", "BashOutput", "Monitor"):
        return "wait / poll"
    if name == "SubagentHandback":
        return "hand-back"
    return name


def first_words(x, n=90):
    if isinstance(x, list):
        x = " ".join(p.get("text", "") for p in x if isinstance(p, dict) and p.get("type") == "text")
    x = re.sub(r"\s+", " ", str(x or "")).strip()
    return x[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True)
    ap.add_argument("--agent", required=True)
    ap.add_argument("--project", default="C--Users-Jarrod-wwe2k14-recomp")
    ap.add_argument("--slow", type=int, default=12, help="how many slowest tool calls to list")
    a = ap.parse_args()
    p = Path(os.path.expanduser("~")) / ".claude" / "projects" / a.project / a.session / "subagents" / f"agent-{a.agent}.jsonl"
    if not p.exists():
        print(f"no transcript at {p}")
        return 2
    recs = []
    for line in p.open(encoding="utf-8", errors="replace"):
        try:
            r = json.loads(line)
        except Exception:
            continue
        t = ts(r.get("timestamp", ""))
        if t is None:
            continue
        recs.append((t, r))
    recs.sort(key=lambda x: x[0])
    if not recs:
        print("empty transcript")
        return 2

    pending = {}  # tool_use id -> (t issued, kind, label)
    rounds = []   # each: dict(start, title, tool{kind: s}, calls{kind: n}, model, parked)
    slow = []
    cur = None
    last_t = None
    last_was_result = False

    def new_round(t, title):
        nonlocal cur
        cur = {"start": t, "end": t, "title": title, "tool": {}, "calls": {}, "model": 0.0, "parked": 0.0, "edits": 0}
        rounds.append(cur)

    for t, r in recs:
        typ = r.get("type")
        msg = r.get("message") or {}
        content = msg.get("content")
        if typ == "user":
            is_result = isinstance(content, list) and any(isinstance(c, dict) and c.get("type") == "tool_result" for c in content)
            if is_result:
                for c in content:
                    if isinstance(c, dict) and c.get("type") == "tool_result":
                        iss = pending.pop(c.get("tool_use_id"), None)
                        if iss and cur:
                            d = max(0.0, t - iss[0])
                            cur["tool"][iss[1]] = cur["tool"].get(iss[1], 0.0) + d
                            slow.append((d, iss[1], iss[2], len(rounds)))
                last_was_result = True
            else:
                new_round(t, first_words(content))  # a message delivered to the agent: a new round
                last_was_result = True
            last_t = t
            if cur:
                cur["end"] = t
        elif typ == "assistant":
            if cur is None:
                new_round(t, "(start)")
            if last_t is not None and not pending:
                gap = max(0.0, t - last_t)
                if gap > PARK and not last_was_result:
                    cur["parked"] += gap
                elif gap > PARK * 4:
                    cur["parked"] += gap
                else:
                    cur["model"] += gap
            if isinstance(content, list):
                for c in content:
                    if isinstance(c, dict) and c.get("type") == "tool_use":
                        k = kind_of(c.get("name", ""), c.get("input"))
                        inp = c.get("input") or {}
                        label = str(inp.get("description") or inp.get("command") or inp.get("file_path") or inp.get("pattern") or c.get("name"))
                        pending[c.get("id")] = (t, k, re.sub(r"\s+", " ", label)[:100])
                        cur["calls"][k] = cur["calls"].get(k, 0) + 1
                        if k == "edit":
                            cur["edits"] += 1
            last_t = t
            last_was_result = False
            cur["end"] = t

    def fmt(s):
        return f"{s / 60:6.1f}"

    total_wall = recs[-1][0] - recs[0][0]
    print(f"agent {a.agent}: {datetime.fromtimestamp(recs[0][0]).strftime('%H:%M')} -> {datetime.fromtimestamp(recs[-1][0]).strftime('%H:%M')}, "
          f"{total_wall / 60:.1f} min wall, {len(rounds)} round(s) (a round starts at each message delivered to it)")
    kinds = sorted({k for r in rounds for k in r["tool"]} | {k for r in rounds for k in r["calls"]})
    tot_tool = {k: sum(r["tool"].get(k, 0.0) for r in rounds) for k in kinds}
    tot_calls = {k: sum(r["calls"].get(k, 0) for r in rounds) for k in kinds}
    tot_model = sum(r["model"] for r in rounds)
    tot_parked = sum(r["parked"] for r in rounds)
    print("\n== whole agent (minutes) ==")
    print(f"  model time (reading results, deciding, writing the next step) {fmt(tot_model)}")
    print(f"  parked (turn ended: waiting for a message or its own background job) {fmt(tot_parked)}")
    print(f"  tool time, by kind (calls):")
    for k in sorted(kinds, key=lambda k: -tot_tool[k]):
        print(f"    {k:<24}{fmt(tot_tool[k])}  ({tot_calls[k]})")
    print(f"  tool time total {fmt(sum(tot_tool.values()))}; calls {sum(tot_calls.values())}; edits {sum(r['edits'] for r in rounds)}")
    print("\n== per round (minutes) ==")
    print("  #  start  wall  model parked  tool | calls edits | top tool kinds | opened by")
    for i, r in enumerate(rounds, 1):
        wall = r["end"] - r["start"]
        tt = sum(r["tool"].values())
        top = ", ".join(f"{k} {v / 60:.1f}" for k, v in sorted(r["tool"].items(), key=lambda kv: -kv[1])[:3] if v >= 6)
        print(f"  {i:<2} {datetime.fromtimestamp(r['start']).strftime('%H:%M')} {fmt(wall)} {fmt(r['model'])} {fmt(r['parked'])} {fmt(tt)} |"
              f" {sum(r['calls'].values()):5d} {r['edits']:5d} | {top} | {r['title'][:70]}")
    print(f"\n== the {a.slow} slowest tool calls ==")
    for d, k, label, rn in sorted(slow, key=lambda x: -x[0])[: a.slow]:
        print(f"  {d:6.0f} s  round {rn}  {k:<18} {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
