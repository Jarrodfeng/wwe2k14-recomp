"""Session 10 (M5 sweep): results of tools\\sweep\\run_sweep.ps1 batches -> table, JSON and a contact-sheet page.

  py tools\\sweep\\results.py LIST [--records FILE ...] [--no-images]
      LIST = arenas | roster | full. Reads every out\\harness\\sweep\\runs\\LIST_*.json record (or --records); when
      an item ran more than once, the newest record wins (reruns). Writes
        out\\harness\\sweep\\results\\LIST.json   every field per item
        out\\harness\\sweep\\results\\LIST.md     the table for the PLAYBOOK
        out\\harness\\review\\session10\\LIST\\index.html + JPG copies of the screenshots (local only: game images)

Per run, from the run log (out\\build\\win-amd64-release\\logs\\wwe2k14_NNN.log) and run_list's result line:
exit (clean quit / crash / killed), script failures and optional-wait timeouts, the launcher's lines, file opens
(BGnn.PAC arena archive, ENTRANCEnn, CHnnn characters, NNN.bik2 titantrons, M3.PAC match scene, INGAMELUA.PAC),
peeks (full matches: seq, judge, jmode, fin, winlose, scene), log_state values, the [frames] 5-second summaries
(fps: indicative only in shared lanes) and the screenshots. Verdict: PASS / WARN (ran clean, something to look at) /
FAIL (crash, hang, killed, script failure, or the expected archive never opened).
"""
from __future__ import annotations

import argparse
import html
import json
import re
import statistics
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXE_DIR = REPO / "out" / "build" / "win-amd64-release"
LOGS = EXE_DIR / "logs"
RUNS = REPO / "out" / "harness" / "sweep" / "runs"
RESULTS = REPO / "out" / "harness" / "sweep" / "results"
REVIEW = REPO / "out" / "harness" / "review" / "session10"
TRIAGE = REPO / "out" / "harness" / "triage"

RX_TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")
RX_FLAG = re.compile(r"\[wwe2k14\] modified: (\S+) = '(.*)' \(source: command line\)")
RX_OPEN = re.compile(r"\[wwe2k14\]\[file\] NtCreateFile '([^']*)' \S+ access=\S+ -> (0x[0-9A-F]+) .*frame=(\d+)")
RX_PEEK = re.compile(r"\[wwe2k14\]\[peek\] frame=(\d+) (\w+)=(0x[0-9A-F]+ \(-?\d+\)|unreadable)")
RX_STATE = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: state (\S+) = (.*) \(frame (\d+)\)")
RX_INPUT_LOG = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: (sweep|full): (.*) \(frame (\d+)\)")
RX_OPT = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: (.*optional: going on, frame (\d+)\))")
RX_FAIL = re.compile(r"\[wwe2k14\]\[input\] script FAILED(.*)")
RX_SHOT = re.compile(r"\[wwe2k14\]\[input\] screenshot saved: (.*\.(?:bmp|png)) \((\d+)x(\d+)\)")
RX_QUIT = re.compile(r"\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: quit \(frame (\d+)\)")
RX_FRAMES = re.compile(r"\[wwe2k14\]\[frames\] t=(\d+) s, frames (\d+)-(\d+): ([\d.]+) fps, avg ([\d.]+) ms, "
                       r"p95 ([\d.]+) ms, worst (\d+) ms, hitches (\d+)")
RX_CRASH = re.compile(r"\]\[crash\]|device lost|Failed to await|EXCEPTION|unhandled exception", re.IGNORECASE)
RX_SCEN = re.compile(r"\[wwe2k14\]\[scenario\] (launch '[^']*': .*)")
RX_TRON = re.compile(r"titantron\\(\d+)\.bik2$", re.IGNORECASE)
RX_CH = re.compile(r"\\CH\\CH(\d+)\.PAC$", re.IGNORECASE)
RX_BG = re.compile(r"\\BG\\BG(\d+)\.PAC$", re.IGNORECASE)
RX_ENT = re.compile(r"\\EVT\\(ENTRANCE\d+)\.PAC$", re.IGNORECASE)


def parse_log(num: int) -> dict:
    path = LOGS / f"wwe2k14_{num:03d}.log"
    r: dict = {"log": path.name, "exists": path.exists()}
    if not path.exists():
        return r
    flags, opens, failed_opens, peeks, states, notes, optional, fails = {}, [], [], {}, [], [], [], []
    shots, frames, crash, scen = [], [], [], []
    first_time = last_time = None
    quit_frame = None
    shutdown = False
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            mt = RX_TIME.match(line)
            if mt:
                first_time = first_time or mt.group(1)
                last_time = mt.group(1)
            if "[wwe2k14]" not in line:
                if "Title terminated" in line or "Window closing" in line:
                    shutdown = True
                if RX_CRASH.search(line):
                    crash.append(line.strip()[:240])
                continue
            m = RX_OPEN.search(line)
            if m:
                p, res, fr = m.group(1), m.group(2), int(m.group(3))
                if res == "0x00000000":
                    opens.append((fr, p))
                elif p.upper().startswith(("D:\\PAC", "GAME:\\MOVIES")):
                    failed_opens.append((fr, p, res))
                continue
            m = RX_PEEK.search(line)
            if m:
                peeks.setdefault(m.group(2), []).append((int(m.group(1)), m.group(3)))
                continue
            m = RX_FLAG.search(line)
            if m:
                flags[m.group(1)] = m.group(2)
                continue
            m = RX_STATE.search(line)
            if m:
                states.append({"line": int(m.group(1)), "name": m.group(2), "value": m.group(3),
                               "frame": int(m.group(4))})
                continue
            m = RX_INPUT_LOG.search(line)
            if m:
                notes.append((int(m.group(4)), m.group(3)))
                continue
            m = RX_OPT.search(line)
            if m:
                optional.append(m.group(2)[:200])
                continue
            m = RX_FAIL.search(line)
            if m:
                fails.append(m.group(1).strip()[:240])
                continue
            m = RX_SHOT.search(line)
            if m:
                shots.append(m.group(1))
                continue
            m = RX_QUIT.search(line)
            if m:
                quit_frame = int(m.group(1))
                continue
            m = RX_FRAMES.search(line)
            if m:
                frames.append({"t": int(m.group(1)), "a": int(m.group(2)), "b": int(m.group(3)),
                               "fps": float(m.group(4)), "avg": float(m.group(5)), "p95": float(m.group(6)),
                               "worst": int(m.group(7)), "hitches": int(m.group(8))})
                continue
            m = RX_SCEN.search(line)
            if m:
                scen.append(m.group(1)[:300])
                continue
            if RX_CRASH.search(line):
                crash.append(line.strip()[:240])

    def first(rx, after=0):
        for fr, p in opens:
            if fr >= after and rx(p):
                return fr
        return None

    ing = first(lambda p: p.upper().endswith("\\YASE\\INGAMELUA.PAC"))
    after = ing or 0
    m3 = first(lambda p: p.upper().endswith("\\M3.PAC"), after)
    bgs, chs, ents, trons = [], [], [], []
    for fr, p in opens:
        if fr < after:
            continue
        for rx, acc in ((RX_BG, bgs), (RX_CH, chs), (RX_ENT, ents), (RX_TRON, trons)):
            mm = rx.search(p)
            if mm:
                key = mm.group(1)
                if key not in [k for k, _ in acc]:
                    acc.append((key, fr))
    r.update({
        "flags": flags, "first_time": first_time, "last_time": last_time, "quit_frame": quit_frame,
        "shutdown_line": shutdown, "ingame": ing, "m3": m3,
        "bg": [(int(k), f) for k, f in bgs], "ch": [(int(k), f) for k, f in chs], "entrance": ents,
        "trons": [(int(k), f) for k, f in trons], "failed_opens": failed_opens[:20], "peeks": peeks,
        "states": states, "notes": notes, "optional_timeouts": optional, "script_failed": fails,
        "screenshots": shots, "crash_lines": crash[:10], "scenario_lines": scen[-12:],
    })
    # fps from the 5-second summaries after the in-game load (indicative only in shared lanes).
    if frames:
        ingame = [f for f in frames if ing is None or f["a"] >= ing]
        sel = ingame or frames
        r["fps"] = {"median_fps": round(statistics.median(f["fps"] for f in sel), 1),
                    "min_fps": round(min(f["fps"] for f in sel), 1),
                    "worst_ms": max(f["worst"] for f in sel), "hitches": sum(f["hitches"] for f in sel),
                    "windows": len(sel), "last_frame": frames[-1]["b"]}
    return r


def triage_for(num: int) -> dict | None:
    p = TRIAGE / f"run_{num:03d}.json"
    if p.exists():
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
            return {"class": d.get("class"), "confidence": d.get("confidence"), "culprit": d.get("culprit")}
        except Exception as e:  # noqa: BLE001
            return {"class": f"unreadable ({e})"}
    return None


def live_lines(rec: dict) -> list[str]:
    """Result lines of a batch still running: run_list -Parallel writes out/harness/run_list/par_<stamp>_<pid>/
    line_<i>.txt as each run ends (i = input order = the record's item order). Takes the newest par_ folder whose
    job.json lists the same scripts and extras as the record."""
    want = [(it.get("extra") or "") for it in rec.get("items", [])]
    for d in sorted((REPO / "out" / "harness" / "run_list").glob("par_*"), reverse=True):
        try:
            job = json.loads((d / "job.json").read_text(encoding="utf-8-sig"))
        except Exception:  # noqa: BLE001
            continue
        if [str(x.get("extra") or "") for x in job.get("items", [])] != want:
            continue
        lines = []
        for i in range(len(want)):
            f = d / f"line_{i}.txt"
            lines.append(f.read_text(encoding="utf-8-sig").strip() if f.exists() else "")
        return lines
    return []


def load_records(list_name: str, files: list[str] | None, live: bool = False) -> dict[str, dict]:
    paths = [Path(f) for f in files] if files else sorted(RUNS.glob(f"{list_name}_*.json"))
    items: dict[str, dict] = {}
    for p in paths:  # oldest first (file names carry the stamp): later records win
        try:
            rec = json.loads(p.read_text(encoding="utf-8-sig"))
        except Exception as e:  # noqa: BLE001
            print(f"skip {p.name}: {e}", file=sys.stderr)
            continue
        if live and not rec.get("ended"):
            lines = live_lines(rec)
            for it, ln in zip(rec.get("items", []), lines):
                m = re.search(r"-> (wwe2k14_\d+)", ln)
                if m:
                    it["line"], it["log"] = ln, m.group(1)
        for it in rec.get("items", []):
            if not it.get("log"):
                if it["name"] not in items:
                    items[it["name"]] = dict(it, record=p.name)
                continue
            items[it["name"]] = dict(it, record=p.name)
    return items


def exit_kind(line: str) -> str:
    if "KILLED after" in line:
        return "killed"
    if "STILL RUNNING" in line:
        return "still running"
    if "ABORT" in line:
        return "abort"
    m = re.search(r"EXITED after [\d.,]+s, exit code (-?\d+)", line)
    if m:
        return "clean" if m.group(1) == "0" else f"exit code {m.group(1)}"
    return "unknown"


def judge(list_name: str, it: dict, lg: dict) -> tuple[str, list[str]]:
    why: list[str] = []
    meta = it.get("meta") or {}
    ek = exit_kind(it.get("line", ""))
    if not lg.get("exists"):
        return "FAIL", ["no run log"]
    if ek != "clean":
        why.append(f"exit: {ek}")
    if lg.get("crash_lines"):
        why.append(f"crash lines: {len(lg['crash_lines'])}")
    if lg.get("script_failed"):
        why.append("script failed: " + lg["script_failed"][0])
    if lg.get("quit_frame") is None:
        why.append("no scripted quit")
    fail = bool(why)
    if lg.get("ingame") is None:
        why.append("no in-game load (INGAMELUA.PAC)")
        fail = True
    if list_name == "arenas":
        want = int(meta.get("arena", -1))
        if want not in [b for b, _ in lg.get("bg", [])]:
            why.append(f"BG{want:02d}.PAC never opened")
            fail = True
    if list_name == "roster":
        want = int(meta.get("id", -1))
        if want not in [c for c, _ in lg.get("ch", [])]:
            why.append(f"CH{want}.PAC never opened")
            fail = True
    if fail:
        return "FAIL", why
    if lg.get("m3") is None and list_name != "roster":
        why.append("no match scene (M3.PAC)")
    if not lg.get("trons") and list_name != "full":
        why.append("no titantron opened")
    if lg.get("optional_timeouts"):
        why.append(f"{len(lg['optional_timeouts'])} optional wait(s) timed out")
    if lg.get("failed_opens"):
        why.append(f"{len(lg['failed_opens'])} failed game-file opens")
    if list_name == "full":
        fin = lg.get("peeks", {}).get("fin", [])
        if not any(v.split()[0] != "0x00000000" for _, v in fin if v != "unreadable"):
            why.append("finish type G+472 never set")
    return ("WARN" if why else "PASS"), why


def jpg_copies(list_name: str, num: int, shots: list[str], do_images: bool) -> list[dict]:
    out = []
    dest = REVIEW / list_name
    dest.mkdir(parents=True, exist_ok=True)
    for s in shots:
        src = Path(s)
        label = src.stem.split("_", 1)[1] if "_" in src.stem else src.stem
        big, small = dest / f"{src.stem}.jpg", dest / f"{src.stem}_t.jpg"
        if do_images and src.exists() and not (big.exists() and small.exists()):
            try:
                from PIL import Image
                with Image.open(src) as im:
                    im = im.convert("RGB")
                    im.thumbnail((1280, 720))
                    im.save(big, quality=85)
                    im.thumbnail((400, 225))
                    im.save(small, quality=80)
            except Exception as e:  # noqa: BLE001
                print(f"image {src.name}: {e}", file=sys.stderr)
        out.append({"label": label, "bmp": str(src), "jpg": big.name if big.exists() else None,
                    "thumb": small.name if small.exists() else None})
    return out


_NAMES: dict[int, tuple[str, str]] | None = None


def character_name(cid: int) -> tuple[str, str]:
    """(name, where from): 'table' = db/ids/characters.json, 'wiki' = out/harness/sweep/wiki_names.json (the
    community list cited in characters.json, kept local), '' = unknown."""
    global _NAMES
    if _NAMES is None:
        _NAMES = {}
        wiki = REPO / "out" / "harness" / "sweep" / "wiki_names.json"
        if wiki.exists():
            try:
                for k, v in (json.loads(wiki.read_text(encoding="utf-8")).get("names") or {}).items():
                    if v and v.get("name"):
                        note = f" ({v['note']})" if v.get("note") else ""
                        _NAMES[int(k)] = (v["name"] + note, "wiki")
            except Exception as e:  # noqa: BLE001
                print(f"wiki_names.json: {e}", file=sys.stderr)
        try:
            for e in json.loads((REPO / "db" / "ids" / "characters.json").read_text(encoding="utf-8"))["entries"]:
                _NAMES[int(e["id"])] = (str(e.get("name", "")), "table")
        except Exception as e:  # noqa: BLE001
            print(f"characters.json: {e}", file=sys.stderr)
    return _NAMES.get(cid, ("", ""))


def describe(list_name: str, it: dict, lg: dict) -> str:
    meta = it.get("meta") or {}
    parts = []
    if list_name == "arenas":
        parts.append(f"{meta.get('arena')}: {meta.get('arena_name', '')}")
    elif list_name == "roster":
        name, src = character_name(int(meta.get("id", -1)))
        parts.append(f"{meta.get('id')} {name}{' (wiki)' if src == 'wiki' else ''} (vs {meta.get('opponent')})")
    else:
        parts.append(f"{meta.get('scenario')} ({meta.get('pace')})")
    return " ".join(parts)


def fmt_frames(v):
    return "-" if v is None else str(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("list", choices=["arenas", "roster", "full"])
    ap.add_argument("--records", nargs="*")
    ap.add_argument("--no-images", action="store_true")
    ap.add_argument("--live", action="store_true", help="include runs of a batch that is still going")
    ap.add_argument("--out", default="", help="Session 11: write results\\OUT\\ and review\\OUT\\ instead of the "
                    "Session 10 folders (so a later sweep never overwrites the Session 10 review pages)")
    a = ap.parse_args()
    if a.out:
        global RESULTS, REVIEW
        RESULTS = RESULTS / a.out
        REVIEW = REVIEW.parent / a.out
    items = load_records(a.list, a.records, a.live)
    order = json.loads((REPO / "out" / "harness" / "sweep" / "lists" / f"{a.list}.json").read_text(
        encoding="utf-8"))["items"]
    rows = []
    for spec in order:
        it = items.get(spec["name"])
        if not it or not it.get("log"):
            rows.append({"name": spec["name"], "meta": spec.get("meta"), "verdict": "NOT RUN", "why": [],
                         "line": (it or {}).get("line", "")})
            continue
        num = int(it["log"].split("_")[1])
        lg = parse_log(num)
        verdict, why = judge(a.list, it, lg)
        row = {"name": it["name"], "meta": it.get("meta"), "run": num, "record": it.get("record"),
               "line": it.get("line", ""), "exit": exit_kind(it.get("line", "")), "verdict": verdict, "why": why,
               "triage": triage_for(num), "desc": describe(a.list, it, lg),
               "images": jpg_copies(a.list, num, lg.get("screenshots", []), not a.no_images)}
        row.update({k: lg.get(k) for k in ("ingame", "m3", "bg", "ch", "entrance", "trons", "fps", "peeks",
                                           "states", "notes", "optional_timeouts", "script_failed",
                                           "crash_lines", "failed_opens", "scenario_lines", "first_time",
                                           "last_time", "quit_frame", "flags")})
        rows.append(row)
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / f"{a.list}.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
    write_md(a.list, rows)
    write_html(a.list, rows)
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print(f"{a.list}: {len(rows)} items: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"  {RESULTS / (a.list + '.md')}")
    print(f"  {REVIEW / a.list / 'index.html'}")
    return 0


def rel(fr, base):
    return "-" if fr is None or base is None else f"+{fr - base}"


def write_md(list_name: str, rows: list[dict]) -> None:
    L = [f"# Session 10 sweep: {list_name}", "",
         "Generated by `py tools\\sweep\\results.py " + list_name + "` (frames are guest frames; `+N` = after the "
         "in-game load INGAMELUA.PAC; fps from the 5-s summaries after it, indicative only in shared lanes).", ""]
    if list_name == "arenas":
        L += ["| arena | name | run | verdict | BG | entrance | 1st tron | M3 | fps med / min | notes |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    elif list_name == "roster":
        L += ["| id | name | run | verdict | CH | 1st tron (frame) | trons | fps med / min | notes |",
              "|---|---|---|---|---|---|---|---|---|"]
    else:
        L += ["| match | run | verdict | M3 | decided (fin) | winner / loser | quit | fps med / min | notes |",
              "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        meta = r.get("meta") or {}
        if r["verdict"] == "NOT RUN":
            L.append(f"| {r['name']} | | NOT RUN | | | | | | | |")
            continue
        fps = r.get("fps") or {}
        fps_s = f"{fps.get('median_fps', '-')} / {fps.get('min_fps', '-')}" if fps else "-"
        notes = "; ".join(r["why"]) + (f" [triage {r['triage']['class']}]" if r.get("triage") else "")
        ing = r.get("ingame")
        trons = r.get("trons") or []
        if list_name == "arenas":
            bg = ",".join(f"BG{b:02d}" for b, _ in (r.get("bg") or []))
            ent = ",".join(e for e, _ in (r.get("entrance") or []))
            t1 = f"{trons[0][0]} ({rel(trons[0][1], ing)})" if trons else "-"
            L.append(f"| {meta.get('arena')} | {meta.get('arena_name', '')} | {r['run']} | {r['verdict']} | {bg} | "
                     f"{ent} | {t1} | {rel(r.get('m3'), ing)} | {fps_s} | {notes} |")
        elif list_name == "roster":
            want = meta.get("id")
            ch = "yes" if want in [c for c, _ in (r.get("ch") or [])] else "NO"
            t1 = f"{trons[0][0]} ({rel(trons[0][1], ing)})" if trons else "-"
            nm, src = character_name(int(want))
            nm = f"{nm} (wiki)" if src == "wiki" else nm
            L.append(f"| {want} | {nm} | {r['run']} | {r['verdict']} | {ch} | {t1} | "
                     f"{','.join(str(t) for t, _ in trons)} | {fps_s} | {notes} |")
        else:
            fin = [(fr, v) for fr, v in (r.get("peeks") or {}).get("fin", []) if not v.startswith("0x00000000")]
            dec = f"{fin[0][1].split()[0]} @ {rel(fin[0][0], r.get('m3'))} after M3" if fin else "-"
            wl = (r.get("peeks") or {}).get("winlose", [])
            wl_s = wl[-1][1].split()[0] if wl else "-"
            L.append(f"| {r['name']} | {r['run']} | {r['verdict']} | {rel(r.get('m3'), ing)} | {dec} | {wl_s} | "
                     f"{fmt_frames(r.get('quit_frame'))} | {fps_s} | {notes} |")
    (RESULTS / f"{list_name}.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def write_html(list_name: str, rows: list[dict]) -> None:
    dest = REVIEW / list_name
    dest.mkdir(parents=True, exist_ok=True)
    e = html.escape
    cards = []
    for r in rows:
        meta = r.get("meta") or {}
        title = r.get("desc") or r["name"]
        cls = {"PASS": "pass", "WARN": "warn", "FAIL": "fail"}.get(r["verdict"], "none")
        facts = []
        if r["verdict"] != "NOT RUN":
            trons = r.get("trons") or []
            facts.append(f"run {r['run']} &middot; {e(r.get('exit', ''))}")
            if list_name == "arenas":
                facts.append("BG: " + e(",".join(f"{b:02d}" for b, _ in (r.get("bg") or [])) or "none"))
            if list_name == "roster":
                facts.append("CH: " + e(",".join(str(c) for c, _ in (r.get("ch") or [])[:6])))
            facts.append("trons: " + e(",".join(str(t) for t, _ in trons) or "none"))
            if r.get("fps"):
                facts.append(f"fps {r['fps']['median_fps']} (min {r['fps']['min_fps']})")
            for w in r["why"]:
                facts.append("<b>" + e(w) + "</b>")
        imgs = "".join(
            f'<a href="{e(i["jpg"])}"><img loading="lazy" src="{e(i["thumb"])}" alt="{e(i["label"])}" '
            f'title="{e(i["label"])}"></a>' for i in (r.get("images") or []) if i.get("thumb"))
        cards.append(f'<div class="card {cls}"><div class="hd"><span class="v">{e(r["verdict"])}</span> '
                     f'{e(title)}</div><div class="imgs">{imgs or "<i>no screenshots</i>"}</div>'
                     f'<div class="facts">{"<br>".join(facts)}</div></div>')
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    summary = " &middot; ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    page = f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sweep: {e(list_name)}</title><style>
:root{{--bg:#fafaf9;--fg:#1c1917;--card:#fff;--line:#e7e5e4;--pass:#15803d;--warn:#b45309;--fail:#b91c1c}}
@media (prefers-color-scheme:dark){{:root{{--bg:#1c1917;--fg:#f5f5f4;--card:#292524;--line:#44403c;--pass:#4ade80;--warn:#fbbf24;--fail:#f87171}}}}
body{{background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,sans-serif;margin:16px}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(420px,1fr));gap:12px}}
.card{{background:var(--card);border:1px solid var(--line);border-left:5px solid var(--line);border-radius:6px;padding:8px}}
.card.pass{{border-left-color:var(--pass)}}.card.warn{{border-left-color:var(--warn)}}.card.fail{{border-left-color:var(--fail)}}
.hd{{font-weight:600;margin-bottom:6px}}.v{{font-size:12px;padding:1px 6px;border-radius:4px;border:1px solid var(--line)}}
.imgs img{{width:200px;height:auto;margin:0 4px 4px 0;border-radius:3px}}.facts{{font-size:12px;opacity:.9}}
</style></head><body><h1>Session 10 sweep: {e(list_name)}</h1><p>{summary}. Local only (game screenshots). Click a
picture for 1280x720; the original BMPs are in out\\build\\win-amd64-release\\captures.</p>
<div class="grid">{"".join(cards)}</div></body></html>"""
    (dest / "index.html").write_text(page, encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
