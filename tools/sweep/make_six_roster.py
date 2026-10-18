r"""Session 12: the six-per-run roster list for tools\sweep\run_sweep.ps1 (Jarrod's full roster sweep with the fast
loads), from the Session 10 roster list (out\harness\sweep\lists\roster.json, written by make_lists.py).

  py tools\sweep\make_six_roster.py [--name roster6b] [--done 100,101,...]   -> out\harness\sweep\lists\NAME.json

Items: the roster's wrestlers in groups of six (list order; slot order = entrance order) on the sixpack scenario
(--wwe2k14_launch=sixpack --wwe2k14_launch_participants=A,...,F) with tools\input_scripts\short\
launch_six_skip_fast.txt; the last group is topped up with repeats so every run is a six-pack. Only wrestlers with
their own titantron (db\ids\characters.json kind "wrestler" and a titantron) go into six-packs: the first sweep
(2026-10-11, runs 883 / 885 / 886) found that commentators, managers, security guards, "Moveset" and Mankind (no
titantron entry) get no entrance in a six-person match, so the route's per-slot steps went out of step and it pressed
START into the match. Those get a 1v1 run against Daniel Bryan with tools\input_scripts\short\roster_pair_fast.txt
(loads them; Bryan's entrance, which goes first, is skipped 5 s after its step 5: Jarrod, 2026-10-11). --done drops
ids already covered by a finished run.
"""
import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LISTS = REPO / "out" / "harness" / "sweep" / "lists"
SIX = REPO / "tools" / "input_scripts" / "short" / "launch_six_skip_fast.txt"
PAIR = REPO / "tools" / "input_scripts" / "short" / "roster_pair_fast.txt"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="roster6")
    ap.add_argument("--done", default="")
    args = ap.parse_args()
    done = {int(x) for x in args.done.split(",") if x}
    chars = {e["id"]: e for e in
             json.loads((REPO / "db" / "ids" / "characters.json").read_text(encoding="utf-8"))["entries"]}
    roster = [it for it in json.loads((LISTS / "roster.json").read_text(encoding="utf-8"))["items"]
              if it["meta"]["id"] not in done]

    def entrance(c: int) -> bool:
        e = chars.get(c, {})
        return e.get("kind") == "wrestler" and e.get("titantron") is not None

    six = [it["meta"]["id"] for it in roster if entrance(it["meta"]["id"])]
    groups = [six[i:i + 6] for i in range(0, len(six), 6)]
    if groups and len(groups[-1]) < 6:
        fill = [c for c in six if c not in groups[-1]] or [c for c in sorted(chars) if entrance(c)]
        groups[-1] += [c for c in fill if c not in groups[-1]][: 6 - len(groups[-1])]
    items = []
    for n, g in enumerate(groups, 1):
        items.append({"name": f"six_{n:02d}", "script": str(SIX),
                      "extra": "--wwe2k14_launch=sixpack --wwe2k14_launch_participants=" + ",".join(map(str, g)),
                      "meta": {"ids": g}})
    for it in roster:
        if not entrance(it["meta"]["id"]):
            items.append(dict(it, script=str(PAIR), meta=dict(it["meta"], ids=[it["meta"]["id"]])))
    (LISTS / f"{args.name}.json").write_text(json.dumps({"list": args.name, "items": items}, indent=1),
                                             encoding="utf-8")
    covered = sorted({c for it in items for c in it["meta"]["ids"]})
    print(f"{args.name}: {len(items)} runs, {len(covered)} characters ({len(roster)} not done) -> "
          f"{LISTS / (args.name + '.json')}")
    for it in items:
        print(" ", it["name"], it["meta"]["ids"],
              "" if it["name"].startswith("six_") else f"({chars.get(it['meta']['ids'][0], {}).get('name', '?')})")


if __name__ == "__main__":
    main()
