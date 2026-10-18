r"""Session 12: results of a six-per-run roster sweep (tools\sweep\make_six_roster.py list roster6, run with
tools\sweep\run_sweep.ps1). Reads the newest out\harness\sweep\runs\roster6_*.json (or the one given) and, per run,
the game log:

  exit       the run_list result line (exit code 0, PASS / failure text, triage)
  CH         each participant's CHnnn.PAC opened
  tron       each participant's titantron (db\ids\characters.json "titantron", e.g. Austin 101 -> 658.bik2) opened
  step5      "six: wrestler k segment (step 5)" logged without a wait timeout before it
  shots      s6_1..s6_6 + s6_match saved
  match      M3.PAC opened (the match scene)

Writes out\harness\review\roster6\sheet_NN.png (one row per run: the six entrance shots + the match shot, labelled
with id and name; local only) and prints a table plus a verdict per wrestler (PASS = CH + tron + shot; WARN otherwise).

  py tools\sweep\six_results.py [RECORD.json ...]   (several records are read together)
"""
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RUNS = REPO / "out" / "harness" / "sweep" / "runs"
LOGS = REPO / "out" / "build" / "win-amd64-release" / "logs"
CAPS = REPO / "out" / "build" / "win-amd64-release" / "captures"
OUT = REPO / "out" / "harness" / "review" / "roster6"



def _shot(stem):
    """Session 17: screenshots are PNG by default; older runs have BMPs."""
    png = CAPS / (stem + ".png")
    return png if png.exists() else CAPS / (stem + ".bmp")

def main() -> int:
    paths = [Path(a) for a in sys.argv[1:]] or [sorted(RUNS.glob("roster6_*.json"))[-1]]
    rec = {"items": []}
    for rec_path in paths:
        rec["items"] += json.loads(rec_path.read_text(encoding="utf-8"))["items"]
    chars = {e["id"]: e for e in json.loads((REPO / "db" / "ids" / "characters.json").read_text(encoding="utf-8"))["entries"]}
    print(f"records {', '.join(p.name for p in paths)}: {len(rec['items'])} items")
    rows, verdicts = [], {}
    for it in rec["items"]:
        ids = it["meta"]["ids"]
        line = it.get("line") or ""
        m = re.search(r"wwe2k14_(\d+)", line)
        run = int(m.group(1)) if m else None
        exit_ok = "exit code 0 " in line + " " and "KILLED" not in line and "STILL RUNNING" not in line
        text = ""
        if run is not None and (LOGS / f"wwe2k14_{run:03d}.log").exists():
            text = (LOGS / f"wwe2k14_{run:03d}.log").read_text(encoding="utf-8", errors="replace")
        crash = "[wwe2k14][crash]" in text
        opened = set(re.findall(r"NtCreateFile '([^']+)' open[^\n]*-> 0x00000000", text))
        opened_l = {p.lower() for p in opened}
        step5 = {}
        for k in range(1, 7):
            ok = f"six: wrestler {k} segment (step 5)" in text
            # an optional wait_state that runs out logs "wait_state NAME ... not reached within N frames"
            timeout = re.search(rf"wait_state entrance_step_{k - 1} [^\n]*not reached", text)
            step5[k] = ok and not timeout
        match = any(p.endswith("\\m3.pac") for p in opened_l)
        shots = []
        for k in range(1, 7):
            p = _shot(f"{run:03d}_s6_{k}") if run else None
            shots.append(p if p and p.exists() else None)
        mshot = _shot(f"{run:03d}_s6_match") if run else None
        if it["name"].startswith("ch_"):  # pair run (roster_check.txt)
            pair = [_shot(f"{run:03d}_rp_entrance"), _shot(f"{run:03d}_r_tron800")] if run else []
            shots = [next((p for p in pair if p.exists()), None)]
            mshot = _shot(f"{run:03d}_rp_match") if run else None
        row = {"name": it["name"], "run": run, "exit_ok": exit_ok, "crash": crash, "match": match, "ids": ids,
               "shots": shots, "mshot": mshot, "line": line.strip()}
        for k, c in enumerate(ids, 1):
            e = chars.get(c, {})
            tron = e.get("titantron")
            ch_ok = f"d:\\pac\\ch\\ch{c}.pac" in opened_l
            tron_ok = tron is None or any(p.endswith(f"\\titantron\\{tron}.bik2") for p in opened_l)
            shot_ok = k <= len(shots) and shots[k - 1] is not None
            s5 = step5.get(k, True) if it["name"].startswith("six_") else True
            v = "PASS" if (ch_ok and tron_ok and shot_ok and exit_ok and not crash) else "WARN"
            why = [w for w, ok in (("no CH", ch_ok), (f"no tron {tron}", tron_ok), ("no shot", shot_ok),
                                   ("exit", exit_ok), ("crash", not crash)) if not ok]
            if not s5:
                why.append("step 5 wait timed out")
            prev = verdicts.get(c)
            if prev is None or prev[0] != "PASS":
                verdicts[c] = (v, it["name"], ", ".join(why), e.get("name", "?"))
        rows.append(row)
        print(f"{it['name']:7} run {run}  exit {'ok' if exit_ok else 'FAIL'}  crash {crash}  match {match}  "
              f"step5 {sum(step5.values())}/6  ids {ids}")
    n_pass = sum(1 for v in verdicts.values() if v[0] == "PASS")
    print(f"\nwrestlers: {len(verdicts)}  PASS {n_pass}  WARN {len(verdicts) - n_pass}")
    for c, (v, name, why, cname) in sorted(verdicts.items()):
        if v != "PASS" or why:
            print(f"  {c} {cname}: {v} ({name}) {why}")
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    W, H = 320, 180
    per_sheet = 6
    for s in range(0, len(rows), per_sheet):
        chunk = rows[s:s + per_sheet]
        sheet = Image.new("RGB", (W * 7, H * len(chunk)), (20, 20, 20))
        d = ImageDraw.Draw(sheet)
        for r, row in enumerate(chunk):
            cells = list(row["shots"]) + [row["mshot"]]
            labels = [f"{c} {chars.get(c, {}).get('name', '?')}" for c in row["ids"]] + [f"{row['name']} match"]
            for k, p in enumerate(cells):
                x, y = k * W, r * H
                if p is not None and p.exists():
                    try:
                        sheet.paste(Image.open(p).convert("RGB").resize((W, H)), (x, y))
                    except OSError:
                        pass
                lab = labels[k] if k < len(labels) else ""
                d.rectangle([x, y, x + W, y + 14], fill=(0, 0, 0))
                d.text((x + 3, y + 2), lab[:44], fill=(255, 255, 0))
        out = OUT / f"sheet_{s // per_sheet + 1:02d}.png"
        sheet.save(out)
        print("sheet", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
