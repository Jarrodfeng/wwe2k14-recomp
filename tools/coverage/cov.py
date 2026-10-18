"""Scenario coverage CLI (Session 8 task 6): which guest functions ran in which scenario.

Data: the coverage build (preset win-amd64-coverage, src/coverage.h) writes one folder per run under
out/harness/coverage/<run>/ with NN_<mark text>.txt per window (guest function addresses) and segments.tsv.
A window's scenario is its mark text up to the first '.', e.g. "in_ring.first60" -> scenario "in_ring".
Two kinds of windows are kept as windows but belong to no scenario: "quit" (the last frames before a scripted quit)
and transitions "to_<scenario>..." (the press that leaves one screen and the frames until the next one is up, e.g.
"to_pause_menu": START pressed in the match until 1P PAUSED is on screen), so a scenario's sets never hold the tail
of the screen before it.
Import thunks (__imp__ entries of the generated table: kernel/XAM imports the SDK implements) have no
REX_FUNC_PROLOGUE, so coverage cannot see them: they are left out of "never" and of the total.

  py tools/coverage/cov.py build [--runs 004,005]  scan runs -> out/harness/coverage/coverage.json + summary.txt
                                                 + scenarios/<name>.txt and scenarios/unique_<name>.txt
  py tools/coverage/cov.py sets                  scenarios and windows with counts
  py tools/coverage/cov.py show SET              the functions of a set
  py tools/coverage/cov.py diff A B              in A but not in B
  py tools/coverage/cov.py intersect A B [C..]   in all of them
  py tools/coverage/cov.py union A B [C..]       in any of them
  py tools/coverage/cov.py unique SET            in SET and in no other scenario (SET: a scenario or a RUN:TEXT
                                                 window; a window is compared with every scenario but its own)
  py tools/coverage/cov.py range LO HI [SET]     functions with LO <= address < HI (in SET, default: all)
  py tools/coverage/cov.py func ADDR             scenarios and windows that ran this function
A SET is a scenario name (boot, main_menu, caw, logo, entrances, in_ring, pause_menu, replay, ...), a window
"RUN:TEXT" (e.g. 012:in_ring.first60, 012:to_pause_menu, 012:quit), "all" (ran in any window) or "never" (instrumented
generated functions that ran in no window; import thunks excluded).
Add --count to print only the size, --json for JSON. Everything derived from the game stays in out/harness/.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROOT = REPO / "out" / "harness" / "coverage"
GEN = REPO / "generated" / "default"
COVJSON = ROOT / "coverage.json"
NO_SCENARIO = {"quit"}
TRANSITION_PREFIX = "to_"
WINDOW_RX = re.compile(r"^(\d\d+)_(.+)\.txt$")
MAP_RX = re.compile(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}")
_GEN_CACHE: dict[str, int] | None = None


def generated_functions() -> dict[str, int]:
    """name -> guest address, from the generated mapping table (generated/default/wwe2k14_init.cpp)."""
    global _GEN_CACHE
    if _GEN_CACHE is None:
        init = GEN / "wwe2k14_init.cpp"
        _GEN_CACHE = {}
        if init.exists():
            for m in MAP_RX.finditer(init.read_text(encoding="utf-8", errors="replace")):
                _GEN_CACHE[m.group(2)] = int(m.group(1), 16)
    return _GEN_CACHE


def import_thunks() -> set[int]:
    """Addresses of the table's __imp__ entries (kernel/XAM import thunks): no REX_FUNC_PROLOGUE, never recorded."""
    return {a for n, a in generated_functions().items() if n.startswith("__imp__")}


def instrumented_functions() -> set[int]:
    """Every generated function that runs the (coverage) REX_FUNC_PROLOGUE: the table minus the import thunks."""
    return {a for n, a in generated_functions().items() if not n.startswith("__imp__")}


def to_addr(token: str, names: dict[str, int]) -> int | None:
    token = token.strip()
    if token.startswith("0x"):
        return int(token, 16)
    return names.get(token)


# Window texts recorded by earlier versions of the probe routes -> the scenario their screenshots showed
# (Session 8 coverage runs 006 and 009; make_routes.py now writes these names directly).
ALIASES = {
    "story_probe.p1": "story",          # 006: custom story calendar (RAW / SUPERSTARS / SMACKDOWN shows)
    "story_probe.p2": "story",          # 006: RAW week 1 show page (ADD MOMENT)
    "story_probe.p3": "story",          # 006: CHOOSE A MOMENT TYPE (SCENE / MATCH)
    "story_probe.p4": "story_scene",    # 006: scene editor loads (The Miz in the RAW arena)
    "story_probe.p5": "story_scene",    # 006: animation category list over the scene editor
    "story_probe.p5_later": "story_scene",
    "story_scene.anim_chosen": "story_cutscene",   # 009: BACKSTAGE ASSAULT animation playing in the editor
    "story_scene.icons": "story_cutscene",         # 009: still playing (ADVANCED EDITOR icon bar)
    "story_scene.after_icon_a": "story_cutscene",  # 009: ADJUST LENGTH tool over the scene (+300 / +900 differ
                                                   # only slightly: the animation may have reached its end)
}


# Runs made before the routes had transition windows (Session 8 coverage runs 005 / 007 / 009 / 010): there the window
# after a screen-changing press was named after the new screen although it starts at the press. In the match route
# that window is exactly what the current route calls the transition (press + 60 frames, same waits), so it is read
# as that transition: no scenario. In the story route the old window is the transition plus the first 300 frames of
# the animation; it is read as the transition too (newer runs cover the animation). A run is "old" for a rule when it
# has the old window text but not the transition text. (old window text, transition it is read as)
LEGACY_TRANSITIONS = [
    ("pause_menu.first60", "to_pause_menu"),
    ("replay.first60", "to_replay"),
    ("story_cutscene.anim_chosen", "to_story_cutscene"),
    ("story_scene.anim_chosen", "to_story_cutscene"),  # run 009's name for the same window
]


def scenario_of(text: str) -> str | None:
    if text in ALIASES:
        return ALIASES[text]
    s = text.split(".", 1)[0]
    return None if s in NO_SCENARIO or s.startswith(TRANSITION_PREFIX) else s


def read_runs(only: set[str] | None, names: dict[str, int]):
    runs = {}
    for d in sorted(p for p in ROOT.iterdir() if p.is_dir() and p.name.isdigit()) if ROOT.exists() else []:
        if only and d.name not in only:
            continue
        meta = {}
        if (d / "run.txt").exists():
            for line in (d / "run.txt").read_text(encoding="utf-8", errors="replace").splitlines():
                k, _, v = line.partition(" ")
                meta[k] = v
        seg = {}
        if (d / "segments.tsv").exists():
            rows = (d / "segments.tsv").read_text(encoding="utf-8", errors="replace").splitlines()[1:]
            for row in rows:
                f = row.split("\t")
                if len(f) >= 7:
                    seg[int(f[0])] = {"mark_id": int(f[1]), "text": f[2], "start_frame": int(f[3]),
                                      "end_frame": int(f[4]), "new_in_run": int(f[6])}
        windows = []
        for p in sorted(d.iterdir()):
            m = WINDOW_RX.match(p.name)
            if not m:
                continue
            idx = int(m.group(1))
            if idx not in seg:
                # Not written by this run (src/coverage.h appends a segments.tsv row for every window it writes):
                # a leftover of an older run in the same folder. Never merge it.
                print(f"warning: {d.name}/{p.name} has no segments.tsv row: skipped (leftover of an older run?)",
                      file=sys.stderr)
                continue
            info = seg[idx]
            funcs, unresolved = set(), []
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                if not line or line.startswith("#"):
                    continue
                a = to_addr(line, names)
                if a is None:
                    unresolved.append(line)
                else:
                    funcs.add(a)
            windows.append({"window": idx, "file": p.name, "text": info["text"], "scenario": scenario_of(info["text"]),
                            "start_frame": info.get("start_frame"), "end_frame": info.get("end_frame"),
                            "new_in_run": info.get("new_in_run"), "funcs": funcs, "unresolved": unresolved})
        texts = {w["text"] for w in windows}
        for w in windows:
            for old, trans in LEGACY_TRANSITIONS:
                if w["text"] == old and trans not in texts:
                    w["scenario"] = None
                    w["read_as"] = trans
        if windows:
            runs[d.name] = {"meta": meta, "windows": windows}
    return runs


def hx(a: int) -> str:
    return f"0x{a:08X}"


def cmd_build(args) -> int:
    names = generated_functions()
    only = set(args.runs.split(",")) if args.runs else None
    runs = read_runs(only, names)
    if not runs:
        print(f"no coverage runs under {ROOT}")
        return 1
    scen: dict[str, set[int]] = {}
    scen_windows: dict[str, list[str]] = {}
    scen_runs: dict[str, set[str]] = {}
    for r, run in runs.items():
        for w in run["windows"]:
            if w["scenario"]:
                scen.setdefault(w["scenario"], set()).update(w["funcs"])
                scen_windows.setdefault(w["scenario"], []).append(f"{r}:{w['text']}")
                scen_runs.setdefault(w["scenario"], set()).add(r)
    in_scen = set().union(*scen.values()) if scen else set()
    owners: dict[int, list[str]] = {}
    for s, fs in scen.items():
        for a in fs:
            owners.setdefault(a, []).append(s)
    unique = {s: {a for a in fs if len(owners[a]) == 1} for s, fs in scen.items()}
    # Functions that ran only in windows of no scenario (transitions to_*, quit): tagged with those window names, so
    # "all" / "never" / tools/q still see that they ran.
    outside: dict[int, set[str]] = {}
    for run in runs.values():
        for w in run["windows"]:
            if not w["scenario"]:
                tag = w.get("read_as") or w["text"].split(".", 1)[0]
                for a in w["funcs"] - in_scen:
                    outside.setdefault(a, set()).add(tag)
    allf = in_scen | set(outside)
    instrumented = instrumented_functions()
    thunks = import_thunks()
    total = len(instrumented) or None
    stray = allf - instrumented if instrumented else set()
    order = sorted(scen, key=lambda s: min(int(x.split(":")[0]) * 1000 + i for i, x in enumerate(scen_windows[s])))
    func_tags = {a: sorted(owners[a], key=order.index) for a in owners}
    for a, tags in outside.items():
        func_tags[a] = sorted(tags)

    doc = {
        "format": "wwe2k14-coverage/1",
        "built": dt.datetime.now().isoformat(timespec="seconds"),
        "note": "Guest functions (start addresses) that ran at least once per window/scenario. Coverage build "
                "(preset win-amd64-coverage, src/coverage.h). Derived from the game: out/harness only.",
        "total_generated_functions": total,
        "total_note": "instrumented generated functions (those with REX_FUNC_PROLOGUE): the generated table "
                      "minus its import thunks",
        "import_thunks_not_instrumented": len(thunks),
        "runs": {r: {"script": run["meta"].get("script", ""), "log": run["meta"].get("log", r),
                     "windows": [{k: (len(v) if k == "funcs" else v) for k, v in w.items() if k != "unresolved"}
                                 | {"unresolved": len(w["unresolved"])} for w in run["windows"]]}
                 for r, run in runs.items()},
        "scenarios": {s: {"runs": sorted(scen_runs[s]), "windows": scen_windows[s], "count": len(scen[s]),
                          "unique_count": len(unique[s]), "functions": [hx(a) for a in sorted(scen[s])],
                          "unique": [hx(a) for a in sorted(unique[s])]} for s in order},
        "functions_note": "every function that ran in any window: its scenarios, or for one that ran only in windows "
                          "of no scenario, those window names (to_* transitions, quit)",
        "functions": {hx(a): func_tags[a] for a in sorted(func_tags)},
    }
    ROOT.mkdir(parents=True, exist_ok=True)
    COVJSON.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    sdir = ROOT / "scenarios"
    sdir.mkdir(exist_ok=True)
    wanted = {f"{s}.txt" for s in order} | {f"unique_{s}.txt" for s in order}
    for old in sdir.glob("*.txt"):  # lists of scenarios no longer in the tables (this folder is build output only)
        if old.name not in wanted:
            old.unlink()
    for s in order:
        (sdir / f"{s}.txt").write_text("".join(hx(a) + "\n" for a in sorted(scen[s])), encoding="utf-8")
        (sdir / f"unique_{s}.txt").write_text("".join(hx(a) + "\n" for a in sorted(unique[s])), encoding="utf-8")

    lines = [f"Scenario coverage (Session 8 task 6), built {doc['built']} from runs {', '.join(runs)}",
             f"instrumented guest functions (generated/default/wwe2k14_init.cpp table minus its {len(thunks)} "
             f"import thunks, which have no prologue): {total}; ran in any window: {len(allf)}"
             + (f" ({100.0 * len(allf) / total:.1f}%)" if total else "")
             + f"; in a scenario: {len(in_scen)}; only in transition / quit windows: {len(outside)}",
             "Transition windows (to_*: from the press that leaves a screen until the next one is up) and quit "
             "windows belong to no scenario.",
             "", f"{'scenario':<26}{'functions':>10}{'% of all':>9}{'unique':>8}  runs / windows"]
    if stray:
        lines.insert(2, f"warning: {len(stray)} recorded addresses are not instrumented table entries "
                        f"(codegen changed since these runs?), e.g. {', '.join(hx(a) for a in sorted(stray)[:5])}")
    for s in order:
        pct = f"{100.0 * len(scen[s]) / total:.1f}" if total else "?"
        lines.append(f"{s:<26}{len(scen[s]):>10}{pct:>9}{len(unique[s]):>8}  {', '.join(scen_windows[s])}")
    # Windows around marks: the first 60 frames after a scenario starts (TEXT.first60) against the rest of it.
    trans = []
    for r, run in runs.items():
        for w in run["windows"]:
            if w["text"].endswith(".first60") and w["scenario"]:
                rest = set().union(*([x["funcs"] for x in run["windows"]
                                      if x["scenario"] == w["scenario"] and x is not w] or [set()]))
                only = w["funcs"] - rest
                trans.append(f"{r + ':' + w['text']:<36}{len(w['funcs']):>10}{len(only):>14}"
                             f"{len(only - set().union(*(scen[s] for s in scen if s != w['scenario']))):>16}")
    if trans:
        lines += ["", "Windows around marks (first 60 frames of a scenario): functions, only in these 60 frames "
                  "(not later in the scenario, same run), and of those not in any other scenario:",
                  f"{'run:window':<36}{'functions':>10}{'first60 only':>14}{'+ no other scen':>16}"] + trans
    lines += ["", "Windows (frames are guest frames; 'new' = first seen in that run):",
              f"{'run:window':<36}{'frames':>15}{'functions':>10}{'new':>8}"]
    for r, run in runs.items():
        lines.append(f"-- run {r}: {run['meta'].get('script', '')}")
        for w in run["windows"]:
            fr = f"{w['start_frame']}..{w['end_frame']}" if w["start_frame"] is not None else "?"
            unres = f"  ({len(w['unresolved'])} unresolved names)" if w["unresolved"] else ""
            if w.get("read_as"):
                unres += f"  (older route: read as transition {w['read_as']}, no scenario)"
            lines.append(f"{r + ':' + w['text']:<36}{fr:>15}{len(w['funcs']):>10}{w['new_in_run'] or 0:>8}{unres}")
    (ROOT / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    head = next(i for i, line in enumerate(lines) if line.startswith("scenario")) + 1
    print("\n".join(lines[:head + len(order)]))
    print(f"wrote {COVJSON}, {ROOT / 'summary.txt'}, {sdir}")
    return 0


def load() -> dict:
    if not COVJSON.exists():
        raise SystemExit(f"{COVJSON} missing: run  py tools/coverage/cov.py build")
    return json.loads(COVJSON.read_text(encoding="utf-8"))


def resolve(doc: dict, name: str) -> set[int]:
    if name == "all":
        return {int(a, 16) for a in doc["functions"]}
    if name == "never":
        return instrumented_functions() - {int(a, 16) for a in doc["functions"]}
    if name in doc["scenarios"]:
        return {int(a, 16) for a in doc["scenarios"][name]["functions"]}
    if ":" in name:
        run, text = name.split(":", 1)
        rdir = ROOT / run
        for p in sorted(rdir.glob("*.txt")) if rdir.exists() else []:
            m = WINDOW_RX.match(p.name)
            safe = re.sub(r"[^A-Za-z0-9_-]", "_", text)[:60]  # src/coverage.h FileSafe()
            if m and (m.group(2) in (text, safe) or p.name == text or p.stem == text):
                names = generated_functions()
                out = set()
                for line in p.read_text(encoding="utf-8").splitlines():
                    if line and not line.startswith("#"):
                        a = to_addr(line, names)
                        if a is not None:
                            out.add(a)
                return out
        raise SystemExit(f"no window {text!r} in run {run} ({rdir})")
    raise SystemExit(f"unknown set {name!r}; scenarios: {', '.join(doc['scenarios'])}")


def emit(funcs: set[int], args, doc: dict | None = None) -> int:
    if args.count:
        print(len(funcs))
    elif args.json:
        print(json.dumps([hx(a) for a in sorted(funcs)]))
    else:
        for a in sorted(funcs):
            extra = ""
            if doc is not None:
                extra = "  " + ",".join(doc["functions"].get(hx(a), []))
            print(hx(a) + extra)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["build", "sets", "show", "diff", "intersect", "union", "unique", "range", "func"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--runs", help="build: comma-separated run folders (default: all)")
    ap.add_argument("--count", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--tags", action="store_true", help="show/diff/...: append each function's scenarios")
    a = ap.parse_args()
    if a.cmd == "build":
        return cmd_build(a)
    doc = load()
    tag = doc if a.tags else None
    if a.cmd == "sets":
        total = doc.get("total_generated_functions")
        for s, v in doc["scenarios"].items():
            print(f"{s:<26}{v['count']:>8}  unique {v['unique_count']:>6}  windows: {', '.join(v['windows'])}")
        print(f"{'all':<26}{len(doc['functions']):>8}  of {total} instrumented generated functions "
              f"({doc.get('import_thunks_not_instrumented', '?')} import thunks not instrumented)")
        for r, run in doc["runs"].items():
            print(f"run {r} ({run['script']}): " + ", ".join(f"{w['text']}={w['funcs']}" for w in run["windows"]))
        return 0
    need = {"show": 1, "diff": 2, "unique": 1, "func": 1}
    if a.cmd in need and len(a.args) != need[a.cmd]:
        raise SystemExit(f"{a.cmd} takes {need[a.cmd]} argument(s)")
    if a.cmd == "show":
        return emit(resolve(doc, a.args[0]), a, tag)
    if a.cmd == "diff":
        return emit(resolve(doc, a.args[0]) - resolve(doc, a.args[1]), a, tag)
    if a.cmd in ("intersect", "union"):
        if len(a.args) < 2:
            raise SystemExit(f"{a.cmd} takes two or more sets")
        sets = [resolve(doc, n) for n in a.args]
        out = set.intersection(*sets) if a.cmd == "intersect" else set.union(*sets)
        return emit(out, a, tag)
    if a.cmd == "unique":
        s = a.args[0]
        if s in doc["scenarios"]:
            return emit({int(x, 16) for x in doc["scenarios"][s]["unique"]}, a, tag)
        if ":" not in s:
            raise SystemExit("unique takes a scenario or a RUN:TEXT window ('all' / 'never' have no 'other' sets)")
        # A window: compare with every scenario except the one the window belongs to (none for to_* / quit).
        run, text = s.split(":", 1)
        own = scenario_of(text)
        for w in doc["runs"].get(run, {}).get("windows", []):  # the recorded text, if TEXT was a file name form
            if text in (w["text"], w["file"], w["file"][:-4], w["file"][3:-4]):
                own = w["scenario"]
                break
        others = set().union(*(resolve(doc, n) for n in doc["scenarios"] if n != own))
        return emit(resolve(doc, s) - others, a, tag)
    if a.cmd == "range":
        if len(a.args) not in (2, 3):
            raise SystemExit("range LO HI [SET]")
        lo, hi = int(a.args[0], 16), int(a.args[1], 16)
        base = resolve(doc, a.args[2] if len(a.args) == 3 else "all")
        return emit({x for x in base if lo <= x < hi}, a, tag)
    if a.cmd == "func":
        x = int(a.args[0], 16)
        key = hx(x)
        names = generated_functions()
        # Window files list the few named helpers (xstart, __savegprlr_28, ...) by name, everything else by address.
        tokens = {key} | {n for n, v in names.items() if v == x and not n.startswith("sub_")}
        scen = doc["functions"].get(key)
        print(f"{key}: scenarios: {', '.join(scen) if scen else '(none recorded)'}")
        for r in doc["runs"]:
            rdir = ROOT / r
            hits = []
            for p in sorted(rdir.glob("*.txt")):
                if WINDOW_RX.match(p.name) and tokens & set(p.read_text(encoding="utf-8").split("\n")):
                    hits.append(p.stem)
            if hits:
                print(f"  run {r}: {', '.join(hits)}")
        if x in import_thunks():
            name = next(n for n, v in names.items() if v == x and n.startswith("__imp__"))
            print(f"  import thunk {name}: not instrumented (no REX_FUNC_PROLOGUE), so coverage cannot say whether "
                  f"it ran")
        elif not scen and x not in set(names.values()):
            print("  (not a generated function start: coverage records function starts only)")
        return 0
    return 1


if __name__ == "__main__":
    try:
        rc = main()
        sys.stdout.flush()
    except (BrokenPipeError, OSError) as e:  # output piped into head / Select -First: stop quietly
        if not (isinstance(e, BrokenPipeError) or getattr(e, "errno", None) == 22):
            raise
        import os
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())  # no second error at exit flush
        rc = 0
    sys.exit(rc)
