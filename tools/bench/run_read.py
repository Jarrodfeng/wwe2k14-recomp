#!/usr/bin/env python3
"""run_read.py - read ONE game check run's log and print its facts (Session 108, lane RR).

Why: a run must never be judged from a hand-written search of its log cut at N lines (S108: the line `event prop id 2591 -> PACH 400000 (entry 4)`
sat past the cut and the run was misjudged; the census counter `capped 4374` read above zero for sessions and nobody saw it). This tool reads the
WHOLE log, line by line (a log of hundreds of MB is never held in memory), and prints sections in a fixed order. It writes no file.

  py -I tools\\bench\\run_read.py LOG [--json] [--full] [--expect "TEXT"]... [--zero NAME]... [--zero-or-absent NAME]...
                                       [--allow-unfinished] [--strict-errors [--allow-error REGEX]...]
  LOG = a run log (...\\logs\\wwe2k14_NNN.log), or an exe / run folder or a logs\\ folder (then the run with the HIGHEST NUMBER there, said in RUN).
  A long run's log rotates: wwe2k14_NNN.K.log are its older parts (the highest K is the oldest and holds the run's start), wwe2k14_NNN.log the
  newest (src\\launcher_mobile\\mobile_main.cpp, part 8). Every part of the run is read, oldest first, and RUN says how many.

Sections, in order: RUN, FLAGS, RENDERER, PROBLEMS, ERROR-LEVEL LINES, LOADED, SHORTFALLS, SHOTS, BLACK FRAMES, then EXPECTED / ZERO / UNSORTED.
BLACK FRAMES (S111 BLACKFRAME): the host's `[host] black frames` lines (src\\renderer_host\\black_spans.h), one per run of black frames: the branch that chose
black <- the input that asked for it, the inputs and the why, timed from frames_NNN.csv beside the log when it is there. 2d_only runs (black under the game's
2D rows: menus / loads) are counted, --full lists them. Never decides the exit code.
The default output fits a screen (lists are wrapped, runtime / default flags are counted, shots are first / last); --full prints every item;
--json always carries everything.
Exit: 0 clean; 1 a PROBLEM, a route that did not reach `bench: done`, an unmet --expect / --zero, or (--strict-errors) an error-level group not
allowed; 2 the log could not be read or the arguments are wrong.
  PROBLEMS        `[rdr][PROBLEM] name:` lines (src\\renderer\\problem.cpp), `ROWS REFUSED` (src\\scene_live\\weapon_rows_glue.h), and the crash /
                  device-lost / fatal-exit markers: `[wwe2k14][crash]` (crash_handler.h, critical), `DEVICE LOST at guest frame` (frame_stats.h),
                  `script FAILED` (input_script.h: the route quit), `[FATAL]` (the SDK's unregistered-function stop). At any level. Nothing else.
  ERROR-LEVEL     every other `[error]` / `[critical]` line, grouped by message with addresses, numbers and paths normalised; a burst (consecutive
                  error lines of one thread with different messages: a heap dump) is one group. Every real run has some (BaseHeap::Alloc,
                  NtQueryInformationFile ... unimplemented, ResolvePath ... device not found, guest access violations): they do not decide the
                  exit unless --strict-errors, and then --allow-error REGEX (matched on a group's example or key) lets a group through.
  --expect TEXT   TEXT must appear in a line of the game's own (plain substring of one line). Setting echoes (`modified:` / `(source: ...)`)
                  and the route's own lines (`[input] line N: ...`) never count: they are the harness's words. EXPECTED names the line found.
  --zero NAME     every census counter called NAME (`capped`, or `weapon_rows.capped`) must print 0 EVERY time (many censuses are per window:
                  the last value alone can be 0 after a lossy window). A NAME no census printed FAILS (the path never ran): --zero-or-absent
                  NAME accepts absence.
  --allow-unfinished  a log with no `bench: done` line (crashed, killed, a route without that line) does not fail the read.

Judgement of findings is NOT done here: which findings fail a player build is tools\\report\\triage.py; which lines refuse a bench step is
tools\\surface\\bench_line.py. This tool only states what the log contains.
Counter shape: census lines are free text with several `[tag][tag]` blocks on one physical line, some spanning continuation lines. A block is
cut at every `[scene|rdr|host|native|wwe2k14][name]` marker; inside it a counter is `WORDS N`, `WORDS=N` or `WORDS: N` (WORDS = the run of words
right before the number; CamelCase is split into words). A name seen twice in one block is `name#2` (the second `capped` of the weapon_rows line is
the stage one). The counted thing is the LAST word(s) of WORDS: `first refused row at record 4454` counts a record position, not refusals.
"""
# entry: main
import argparse
import json
import os
import re
import sys

# --------------------------------------------------------------------------------------------------------------------
# THE COUNTER TABLE: name -> meaning. A counter is a SHORTFALL when one of its counted phrases (counted_phrases: the normalized name - lower case,
# CamelCase / spaces / dashes -> `_` - or, before a plural last word, the words that qualify it) IS the entry or ENDS with it as whole `_` words
# (`palette_unreadable`, `latch_proxy_rows_dropped`, `wrinkle_map_fallback_binds`), and any value it printed is not 0. A new meaning is one row. Reviewed against the S108 FAN runs (fixed and base): `late_frame_threshold_set_to 100`
# (a setting echo) and `first_refused_row_at_record 4454` (a record index) are not counts and do not match. Names this table does not know are
# NOT guessed at: those whose counted word LOOKS lossy (HINT) and print non-zero are listed under UNSORTED so the table can grow.
# Some are legitimately non-zero in normal play (docs\research\s108\ALM_alarms.md): the tool does not judge that.
COUNTERS = [
    ("capped", "rows / nodes refused at a constant (no row)"),
    ("overflow", "a pool was full: the call / item was dropped"),
    ("unreadable", "the game's bytes could not be read: not drawn"),
    ("refused", "a read or a draw was refused"),
    ("bad_rows", "rows the plan dropped as bad"),
    ("holes", "something the game drew was not drawn"),
    ("hole", "something the game drew was not drawn"),
    ("missing", "data the game named was not there"),
    # (S108 PRT: `not_in_index` is no longer a loss: an index miss is now read from the game's own texture; the losses are these two)
    ("guest_failed", "the game's own texture could not be read: the batch is skipped"),
    ("guest_missing", "a guest sprite is not in this process (an offline replay): not drawn"),
    ("no_texture", "no texture: drawn white / not drawn"),
    ("unnamed_skipped", "an unnamed object skipped"),
    ("fallback", "a substitute was drawn"),
    ("stale", "data older than the frame was used"),
    ("stale_node", "a node older than the frame was used"),
    ("dropped", "dropped"),
    ("late", "arrived too late for its frame"),
    ("count_unknown", "the game's own count was unknown: 0 rows"),
    ("no_handle", "no handle"),
    ("no_header", "no header"),
    ("no_key", "no key"),
]
# UNSORTED: the counted (last) word starts with one of these stems, or the name's first word is `bad`. `clamp` is not here: in the logs it is a
# sampler address mode count, not lost data; `whole` is not `hole`.
HINT_LAST = re.compile(r"^(?:skip|drop|miss|fail|refus|unknown|unread|overflow|capped|cap$|holes?$|stale|lost|fallback|invalid|reject|unmapped|"
                       r"unmatched|trunc|assum|unnamed|unresolved)")
CENSUS_TAG = re.compile(r"\[(?:scene|rdr|host|native|wwe2k14)\]\[\w+\](?:\[\w+\])?")
NUM = re.compile(r"((?:[A-Za-z_][\w\-]*)(?: [A-Za-z_][\w\-]*)*)(?:=|: ?| )(-?\d+(?:\.\d+)?)(?![\w.])")
HEAD = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[(\w+)\] \[[^\]]*\] \[(t\d+)\] ?(.*)$")
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[(\w+)\]")
FRAME = re.compile(r"\bframe[= ](\d+)")
FLAG = re.compile(r"modified: (\S+) = '(.*)' \(source: ([^)]*)\)")
PROBLEM = re.compile(r"\[rdr\]\[PROBLEM\]\s*([^\s:]*)")   # every occurrence counts, whatever follows the marker
MARKERS = [("crash", re.compile(r"\[wwe2k14\]\[crash\]")), ("device lost", re.compile(r"DEVICE LOST at guest frame")),
           ("script FAILED", re.compile(r"\]\[input\] script FAILED")), ("FATAL", re.compile(r"\[FATAL\]"))]
ECHO = re.compile(r"modified: |\(source: [^)]*\)|\]\[input\] line \d+: ")
LOGNAME = re.compile(r"^wwe2k14_(\d+)(?:\.(\d+))?\.log$")
# S111 BLACKFRAME (src\renderer_host\black_spans.h): one line per run of black frames, and the exit total
BLACK = re.compile(r"\[host\] black frames (\d+)-(\d+) \(n (\d+), held (\d+)\): (.*?) \| inputs: (.*?) \| why: (.*)$")
BLACK_SUM = re.compile(r"\[host\] black spans (\d+) \(frames black (\d+), held (\d+)\), lines printed (\d+)")
BURST_MIN = 3


def norm_name(n):
    n = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", n.strip())
    return re.sub(r"[ \-]+", "_", n.lower())


def counted_phrases(n):
    """The phrases of normalized name n that say what is counted: the name's ending (`palette_unreadable` -> `unreadable`), and when its last
    word is a plural noun, the words that qualify it (`chain_failed_frames` -> `chain_failed`, `lost_bits` -> `lost`). A name whose last word is
    a single position / setting (`first_refused_row_at_record`, `late_frame_threshold_set_to`, `skip_frame`) offers only that ending."""
    ws = [w for w in n.split("_") if w]
    out = ["_".join(ws)]
    if len(ws) >= 2 and ws[-1].endswith("s"):
        out.append("_".join(ws[:-1]))
    return out


def table_hit(n):
    """-> the COUNTERS row (word, meaning) that one of n's counted phrases IS or ends with (whole `_` words), else None."""
    for ph in counted_phrases(n):
        for w, why in COUNTERS:
            if ph == w or ph.endswith("_" + w):
                return w, why
    return None


def hint_hit(n):
    """A lossy-looking name: the last word of one of its counted phrases starts with a HINT stem, or its first word is `bad`."""
    if n.split("_")[0] == "bad":
        return True
    return any(HINT_LAST.match(ph.split("_")[-1]) or ph.endswith("over_cap") for ph in counted_phrases(n))


def err_norm(msg):
    """An error-level message with its varying parts (paths, addresses, numbers) replaced: the group key."""
    m = re.sub(r"(?:\b\w+:)?[\\/][^\s'\"()]*", "<path>", msg)
    m = re.sub(r"0x[0-9A-Fa-f]+", "#", m)
    m = re.sub(r"\b[0-9A-Fa-f]*\d[0-9A-Fa-f]*\b", "#", m)
    m = re.sub(r"\d+", "#", m)
    return re.sub(r"\s+", " ", m).strip()[:120]


def run_parts(folder, num):
    """-> the run's log files, oldest first: wwe2k14_NNN.K.log by K descending, then wwe2k14_NNN.log."""
    parts = []
    for n in os.listdir(folder):
        m = LOGNAME.match(n)
        if m and m.group(1) == num:
            parts.append((-(int(m.group(2)) if m.group(2) else 0), os.path.join(folder, n)))
    return [p for _k, p in sorted(parts)]


def locate(path):
    """-> (parts oldest first, run number or None, note) for PATH (a log file, a logs folder, or an exe / run folder). ([], ...) when none."""
    if os.path.isfile(path):
        m = LOGNAME.match(os.path.basename(path))
        if not m:
            return [path], None, ""
        return run_parts(os.path.dirname(os.path.abspath(path)), m.group(1)), m.group(1), ""
    for d in (path, os.path.join(path, "logs")):
        if not os.path.isdir(d):
            continue
        nums = sorted({m.group(1) for m in (LOGNAME.match(n) for n in os.listdir(d)) if m}, key=int)
        if nums:
            note = "the folder holds %d run logs (%s .. %s): read the newest by number, %s" % (len(nums), nums[0], nums[-1], nums[-1]) \
                if len(nums) > 1 else ""
            return run_parts(d, nums[-1]), nums[-1], note
    return [], None, ""


class Reader:
    """One pass over the run's lines; holds only counts, first lines and counters (never the text)."""

    def __init__(self, expects):
        self.F = {"lines": 0, "ts_first": None, "ts_last": None, "version": [], "route": [], "done": None, "quit": None, "start_seen": False,
                  "last_scene": None, "flags": [], "renderer": {}, "problems": {}, "errors": {}, "props": {}, "made": {}, "chars": {},
                  "arenas": {}, "banks": {}, "dlc": {}, "counters": {}, "expect": {e: None for e in expects}, "black": [], "black_sum": None}
        self.last_tag = None
        self.cur_ts = None     # the last timestamp seen: a continuation line (no timestamp) belongs to it
        self.run = []          # the current run of consecutive error-level lines: [(ln, ts, thread, msg)]

    def flush_run(self):
        run, self.run = self.run, []
        if not run:
            return
        keys = [err_norm(m) for _ln, _ts, _t, m in run]
        if len(run) >= BURST_MIN and len(set(keys)) > 1:
            first = next((i for i, m in enumerate(run) if re.search(r"[A-Za-z]{3}", m[3])), 0)
            items = [("burst: " + keys[first], run[first], len(run))]
        else:
            items = [(k, r, 1) for k, r in zip(keys, run)]
        for key, (ln, ts, _t, msg), size in items:
            g = self.F["errors"].setdefault(key, {"key": key, "count": 0, "first_ts": ts, "line_no": ln, "example": msg, "burst_lines": 0})
            g["count"] += 1
            g["burst_lines"] = max(g["burst_lines"], size if size > 1 else 0)

    def problem(self, kind, key, line, ts, ln):
        ts = ts or self.cur_ts
        fr = FRAME.search(line)
        b = self.F["problems"].setdefault((kind, key), {"n": 0, "first_ts": ts, "first_frame": fr.group(1) if fr else None, "line": line.strip(), "ln": ln})
        b["n"] += 1
        if b["first_frame"] is None and fr:
            b["first_frame"] = fr.group(1)

    def feed(self, line, ln):
        F = self.F
        F["lines"] += 1
        h = HEAD.match(line)
        m = h or TS.match(line)
        ts = level = thread = msg = None
        if m:
            ts, level = m.group(1), m.group(2)
            thread, msg = (h.group(3), h.group(4)) if h else ("?", line[m.end():].strip())
            F["ts_first"] = F["ts_first"] or ts
            F["ts_last"] = self.cur_ts = ts
            self.last_tag = None
        echo = "modified:" in line
        if echo:
            fm = FLAG.search(line)
            if fm:
                F["flags"].append((fm.group(1), fm.group(2), fm.group(3)))
        if "[wwe2k14] run log number" in line:
            F["start_seen"] = True
        if "[wwe2k14] version " in line or "exe built (file time" in line or "[wwe2k14] run log number" in line or "built with WWE2K14_KEEP_STREAM" in line:
            F["version"].append(line[line.index("[wwe2k14]"):] if "[wwe2k14]" in line else line)
        if "bench: start scene" in line:
            F["route"].append(line.split("bench: ", 1)[1].strip())
        if "bench: done" in line:
            F["done"] = ln
        if re.search(r"\[input\] line \d+: quit\b", line):
            F["quit"] = ln
        if "[wwe2k14][scene] " in line and ("sequence" in line or "scene" in line):
            F["last_scene"] = line
        # RENDERER marks, in the order they are named: the stream-free build's own line (src\wwe2k14_app.h, `#if !WWE2K14_STREAM()`: a player
        # build has no periodic [rdr][live] census, that is printed from the stream-only src\renderer_adapter\live.cpp), the owner-mode line
        # (wwe2k14_app.h), the `[rdr][live] started on` lines (live.cpp); every other [rdr][live] line (the census) is only counted.
        if "built with WWE2K14_KEEP_STREAM=OFF" in line:
            F["renderer"].setdefault("stream_free", (ln, "stream-free build (WWE2K14_KEEP_STREAM=OFF): the scene host is the renderer"))
        if "[rdr][live] owner mode:" in line:
            F["renderer"].setdefault("owner", (ln, line[line.index("[rdr][live]"):]))
        elif "[rdr][live] started" in line:
            F["renderer"].setdefault("started", []).append((ln, line[line.index("[rdr][live]"):]))
        elif "[rdr][live]" in line:
            F["renderer"]["census"] = F["renderer"].get("census", 0) + 1
        if F["expect"] and not ECHO.search(line):
            for e, at in F["expect"].items():
                if at is None and e in line:
                    F["expect"][e] = ln
        # --- PROBLEMS (any level), then the error-level groups (what is a PROBLEM is not also an error-level line)
        is_problem = False
        if not echo:
            for pm in PROBLEM.finditer(line):
                self.problem("PROBLEM", pm.group(1), line[pm.start():], ts, ln)
                is_problem = True
            if "ROWS REFUSED" in line:
                self.problem("ROWS REFUSED", re.sub(r"\d+", "#", line[line.index("ROWS REFUSED"):][:200]), line, ts, ln)
                is_problem = True
            for kind, rx in MARKERS:
                if rx.search(line):
                    self.problem(kind, kind, line, ts, ln)
                    is_problem = True
                    break
        if m:   # a new log line ends a run of error lines unless it continues it (same thread, error level)
            if level in ("error", "critical") and not is_problem:
                if self.run and self.run[-1][2] != thread:
                    self.flush_run()
                self.run.append((ln, ts, thread, msg))
            else:
                self.flush_run()
        # --- LOADED
        pm = re.search(r"event prop id (\d+) -> PACH (\S+) \(entry (\d+)\)", line)
        if pm:
            k = "event prop id %s -> PACH %s (entry %s)" % pm.groups()
            F["props"][k] = F["props"].get(k, 0) + 1
        pm = re.search(r"event prop made: id (\d+) k (\d+)", line)
        if pm:
            k = "event prop made: id %s k %s" % pm.groups()
            F["made"][k] = F["made"].get(k, 0) + 1
        if "NtCreateFile '" in line and "-> 0x00000000" in line:
            om = re.search(r"NtCreateFile '([^']*)'", line)
            if om:
                base = re.split(r"[\\/]", om.group(1))[-1]
                if re.match(r"(?i)CH\d+.*\.PAC$", base):
                    F["chars"][base.upper()] = F["chars"].get(base.upper(), 0) + 1
                elif re.match(r"(?i)BG\d+.*\.PAC$", base):
                    F["arenas"][base.upper()] = F["arenas"].get(base.upper(), 0) + 1
                elif re.search(r"(?i)\.bnk$", base):
                    F["banks"][base] = F["banks"].get(base, 0) + 1
        if "[wwe2k14][dlc] pack " in line:
            dm = re.search(r"\[dlc\] (pack \d+ .*)", line)
            if dm:
                F["dlc"][dm.group(1)] = F["dlc"].get(dm.group(1), 0) + 1
        # --- BLACK FRAMES (S111 BLACKFRAME: one line per run of black frames, written when the run ended; the exit total)
        if "[host] black " in line:
            bm = BLACK.search(line)
            if bm:
                F["black"].append({"first": int(bm.group(1)), "last": int(bm.group(2)), "n": int(bm.group(3)), "held": int(bm.group(4)), "what": bm.group(5).strip(),
                                   "inputs": bm.group(6).strip(), "why": bm.group(7).strip(), "ts": ts or self.cur_ts, "line_no": ln})
            sm = BLACK_SUM.search(line)
            if sm:
                F["black_sum"] = {"spans": int(sm.group(1)), "black": int(sm.group(2)), "held": int(sm.group(3)), "printed": int(sm.group(4))}
        # --- census blocks (every physical line, whole; continuation lines carry the previous line's last tag)
        marks = list(CENSUS_TAG.finditer(line))
        segs = []
        if marks:
            if marks[0].start() > 0 and self.last_tag is not None and not m:
                segs.append((self.last_tag, line[:marks[0].start()]))
            for i, mk in enumerate(marks):
                end = marks[i + 1].start() if i + 1 < len(marks) else len(line)
                segs.append((mk.group(0), line[mk.end():end]))
            self.last_tag = marks[-1].group(0)
        elif self.last_tag is not None and not m:
            segs.append((self.last_tag, line))
        cnt = F["counters"]
        for tag, body in segs:
            seen = {}
            for nm in NUM.finditer(body):
                name = norm_name(nm.group(1))
                seen[name] = seen.get(name, 0) + 1
                key = (tag, name if seen[name] == 1 else "%s#%d" % (name, seen[name]))
                v = float(nm.group(2))
                v = int(v) if v == int(v) else v
                c = cnt.get(key)
                if c is None:
                    c = cnt[key] = {"first": v, "nz": None, "nz_ts": None, "max": v, "last": v, "n": 0}
                c["last"] = v
                c["n"] += 1
                if abs(v) > abs(c["max"]):
                    c["max"] = v
                if v != 0 and c["nz"] is None:
                    c["nz"], c["nz_ts"] = v, self.cur_ts

    def finish(self):
        self.flush_run()
        return self.F


def read_run(parts, expects=()):
    """Read every part of the run, oldest first, line by line -> the facts dict."""
    r = Reader(list(expects))
    ln = 0
    for p in parts:
        with open(p, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                ln += 1
                r.feed(line.rstrip("\r\n"), ln)
    return r.finish()


def save_row(parts, num):
    """S108 OPT: the save-options line run_list.ps1 printed when it launched this run (out\\harness\\run_save\\wwe2k14_NNN.txt: line 1 = the line, line 2 = the exe
    folder the run used). The text, or why there is none. A run NOT started by run_list has no record: said so, never silently absent."""
    d = os.environ.get("WWE_RUN_SAVE_DIR")
    if not d:
        try:
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "save"))
            import save_read
            d = os.path.join(save_read.MAIN, "out", "harness", "run_save")
        except Exception as e:   # noqa: BLE001
            return "not shown (cannot find out\\harness\\run_save: %s)" % e
    p = os.path.join(d, "wwe2k14_%s.txt" % num)
    if not num or not os.path.isfile(p):
        return "NOT RECORDED (no %s: the run did not start through tools\\bench\\run_list.ps1, or it is older than the record)" % os.path.join("out", "harness", "run_save", "wwe2k14_%s.txt" % (num or "NNN"))
    try:
        lines = open(p, encoding="utf-8", errors="replace").read().split("\n")
    except OSError as e:
        return "NOT READABLE (%s)" % e
    exe = lines[1].strip() if len(lines) > 1 else ""
    mine = os.path.dirname(os.path.dirname(os.path.abspath(parts[-1])))
    if exe and os.path.normcase(os.path.normpath(exe)) != os.path.normcase(mine):
        return "NOT SHOWN (the record is for a run of %s; this log is under %s: two builds number runs alike)" % (exe, mine)
    return lines[0].strip().replace("save options: ", "", 1)


def shots(folder, num):
    """-> (captures dir or None, [(name, bytes)]) of this run's pictures in the captures folder beside logs\\ (named with the run number)."""
    cap = os.path.join(os.path.dirname(os.path.abspath(folder)), "captures")
    if not os.path.isdir(cap):
        return None, []
    pref = (num + "_") if num else ""
    out = []
    for n in sorted(os.listdir(cap)):
        if os.path.isfile(os.path.join(cap, n)) and (not pref or n.startswith(pref)) and n.lower().endswith((".webp", ".png", ".jpg", ".jpeg", ".bmp")):   # S111 SHOTDIET: WebP is the default
            out.append((n, os.path.getsize(os.path.join(cap, n))))
    return cap, out


def black_times(parts, num, spans):
    """S111 BLACKFRAME: each black run's first / last guest frame -> t_seconds from frames_NNN.csv beside the log (src\\frame_stats.h: its frame column is
    g_guest_frame, the number the [host] lines carry). Sets span["t0"] / span["t1"] where the row exists. -> the CSV path read, why none, or None (no run)."""
    if not spans:
        return None
    p = os.path.join(os.path.dirname(os.path.abspath(parts[-1])), "frames_%s.csv" % num) if num else None
    if not p or not os.path.isfile(p):
        return "no frames_%s.csv beside the log: the runs are not timed" % (num or "NNN")
    want = {}
    for s in spans:
        want.setdefault(s["first"], []).append((s, "t0"))
        want.setdefault(s["last"], []).append((s, "t1"))
    try:
        with open(p, encoding="utf-8", errors="replace") as fh:
            head = fh.readline().strip().split(",")
            if "frame" not in head or "t_seconds" not in head:
                return "%s has no frame / t_seconds column: the runs are not timed" % p
            fi, ti = head.index("frame"), head.index("t_seconds")
            for row in fh:
                c = row.split(",")
                if len(c) <= max(fi, ti) or not c[fi].isdigit():
                    continue
                for s, k in want.get(int(c[fi]), ()):
                    s[k] = float(c[ti])
    except (OSError, ValueError) as e:
        return "%s not read (%s): the runs are not timed" % (p, e)
    return p


def analyse(F, a):
    """Derive the printable facts + verdict."""
    R = {}
    R["problems"] = [{"kind": k, "text": b["line"][:300], "count": b["n"], "first_ts": b["first_ts"],
                      "first_frame": b["first_frame"], "line_no": b["ln"]} for (k, _key), b in F["problems"].items()]
    allow = [re.compile(x) for x in (a.allow_error or [])]
    groups = sorted(F["errors"].values(), key=lambda g: g["line_no"])
    for g in groups:
        g["allowed"] = any(r.search(g["example"]) or r.search(g["key"]) for r in allow)
    R["error_groups"] = groups
    R["errors_failing"] = [g for g in groups if not g["allowed"]] if a.strict_errors else []
    short, unsorted = [], {}
    for (tag, name), c in sorted(F["counters"].items()):
        base = name.split("#")[0]
        hit = table_hit(base)
        if hit:
            if c["nz"] is not None:
                short.append({"tag": tag, "counter": name, "meaning": hit[1], "first_nonzero": c["nz"], "at": c["nz_ts"], "max": c["max"], "last": c["last"]})
        elif hint_hit(base) and c["nz"] is not None:
            u = unsorted.setdefault(base, {"name": base, "tags": [], "max": c["max"], "last": c["last"]})
            if tag not in u["tags"]:
                u["tags"].append(tag)
            u["last"] = c["last"]
            u["max"] = c["max"] if abs(c["max"]) > abs(u["max"]) else u["max"]
    R["shortfalls"], R["unsorted"] = short, sorted(unsorted.values(), key=lambda u: u["name"])
    R["expect_found"] = {e: at for e, at in F["expect"].items() if at is not None}
    R["expect_missing"] = [e for e, at in F["expect"].items() if at is None]
    zf, zok = [], []
    for z, absent_ok in [(z, False) for z in (a.zero or [])] + [(z, True) for z in (a.zero_or_absent or [])]:
        want_tag, _, want = z.rpartition(".")
        want_n = norm_name(want)
        seen = [(tag, name, c) for (tag, name), c in F["counters"].items()
                if name.split("#")[0] == want_n and (not want_tag or tag.strip("[]").replace("][", ".").endswith(want_tag))]
        if not seen:
            (zok if absent_ok else zf).append({"zero": z, "tag": None, "counter": None, "max": None, "last": None, "absent": True})
            continue
        bad = [(t, n, c) for t, n, c in seen if c["nz"] is not None]
        for t, n, c in bad:
            zf.append({"zero": z, "tag": t, "counter": n, "max": c["max"], "last": c["last"], "first_nonzero": c["nz"], "at": c["nz_ts"], "absent": False})
        if not bad:
            zok.append({"zero": z, "tag": None, "counter": ", ".join(sorted("%s %s" % (t, n) for t, n, _c in seen)), "absent": False})
    R["zero_failed"], R["zero_ok"] = zf, zok
    R["unfinished"] = F["done"] is None and not a.allow_unfinished
    R["exit"] = 1 if (R["problems"] or R["expect_missing"] or zf or R["unfinished"] or R["errors_failing"]) else 0
    return R


def wrap(items, indent="  ", width=160):
    """Join items with `, ` into lines of at most `width` chars (an item longer than that has its own line)."""
    out, cur = [], ""
    for it in items:
        if cur and len(indent) + len(cur) + 2 + len(it) > width:
            out.append(indent + cur)
            cur = it
        else:
            cur = it if not cur else cur + ", " + it
    if cur:
        out.append(indent + cur)
    return out


def counted(d):
    return ["%s%s" % (k, " (x%d)" % n if n > 1 else "") for k, n in d.items()]


def render(F, R, a):
    o = []
    P = o.append
    full = a.full
    P("RUN")
    P("  log: %s (%d lines%s)" % (F["parts"][-1], F["lines"], "; %d parts joined, oldest first: %s" % (
        len(F["parts"]), ", ".join(os.path.basename(p) for p in F["parts"])) if len(F["parts"]) > 1 else ""))
    if F["note"]:
        P("  " + F["note"])
    if not F["start_seen"]:
        P("  start: NOT in this log (no `run log number` line: the oldest part may have rotated away)")
    P("  time: first %s, last %s" % (F["ts_first"] or "none", F["ts_last"] or "none"))
    P("  save options: %s" % F["save_options"])
    if F["version"] and full:
        for v in F["version"]:
            P("  build: " + v)
    elif F["version"]:
        P("  build: " + "; ".join((v.split(":", 1)[0] if "KEEP_STREAM" in v else v).replace("[wwe2k14] ", "") for v in F["version"][:6]))
    else:
        P("  build: no version / build-stamp line in this log")
    P("  route: %s" % ("; ".join(F["route"]) if F["route"] else "none (no `bench: start scene` line)"))
    P("  bench: done %s; quit %s%s" % ("reached (line %d)" % F["done"] if F["done"] else "NOT reached: the route did not finish",
                                       "reached (line %d)" % F["quit"] if F["quit"] else "NOT reached",
                                       " (--allow-unfinished)" if not F["done"] and a.allow_unfinished else ""))
    ls = (F["last_scene"] or "").strip()
    P("  last scene mark: %s" % (ls[ls.find("[wwe2k14]"):] if ls else "none"))
    srcs = {}
    for _n, _v, s in F["flags"]:
        srcs[s] = srcs.get(s, 0) + 1
    cmd = [(n, v) for n, v, s in F["flags"] if s == "command line"]
    P("FLAGS: %d modified (%s)%s" % (len(F["flags"]), ", ".join("%d %s" % (n, s) for s, n in sorted(srcs.items(), key=lambda x: -x[1])) or "none",
                                    "" if full or not srcs else "; the command line's (harness and route) below, --full for all"))
    o.extend(wrap(["%s=%s" % (n, v) for n, v, _s in F["flags"]] if full else ["%s=%s" % nv for nv in cmd]))
    P("RENDERER")
    rd = F["renderer"]
    if "stream_free" in rd:
        P("  %s (line %d)" % (rd["stream_free"][1], rd["stream_free"][0]))
    if "owner" in rd:
        P("  %s (line %d)" % (rd["owner"][1], rd["owner"][0]))
    for ln_, s in rd.get("started", []):
        P("  %s (line %d)" % (s, ln_))
    if rd.get("census"):
        P("  %d other [rdr][live] line(s) (the stream build's census)" % rd["census"])
    if not rd:
        P("  NOT SEEN: no stream-free build line, no [rdr][live] owner-mode / started / census line (the old renderer drew this run?)")
    P("PROBLEMS (decide the exit code)")
    for r in R["problems"]:
        P("  [%s] x%d first %s%s line %d: %s" % (r["kind"], r["count"], "frame %s, " % r["first_frame"] if r["first_frame"] else "",
                                                 r["first_ts"] or "?", r["line_no"], r["text"] if full else r["text"][:200]))
    if not R["problems"]:
        P("  none")
    P("ERROR-LEVEL LINES (grouped; do not decide the exit code: --strict-errors makes them)")
    for g in R["error_groups"]:
        P("  %sx%d first %s line %d%s: %s" % ("ALLOWED " if g["allowed"] and a.strict_errors else "", g["count"], g["first_ts"], g["line_no"],
                                             " (a burst of %d lines)" % g["burst_lines"] if g["burst_lines"] else "",
                                             g["example"] if full else g["example"][:150]))
    if not R["error_groups"]:
        P("  none")
    P("LOADED")
    dlc = F["dlc"] if full else {re.sub(r" \(.*\)(?=: )", "", k).split(":")[0] + (" -> offered" if "-> offered" in k else ""): n
                                 for k, n in F["dlc"].items()}
    made = F["made"] if full else {k.replace("event prop made: ", ""): n for k, n in F["made"].items()}
    for title, d in (("event props (loader)", F["props"]), ("event props made", made), ("characters / attires", F["chars"]),
                     ("arena", F["arenas"]), ("entrance / sound banks", F["banks"]), ("DLC packs", dlc)):
        lines = wrap(counted(d), indent="    ")
        if not lines:
            P("  %s: none" % title)
        elif len(lines) == 1 and len(title) + len(lines[0]) < 160:
            P("  %s: %s" % (title, lines[0].strip()))
        else:
            P("  %s:" % title)
            o.extend(lines)
    P("SHORTFALLS (printed non-zero at least once; not all are faults: see docs\\research\\s108\\ALM_alarms.md)%s" % (
        "" if full else "; the largest value, (last N) when the last differs; --full: first non-zero and meaning"))
    if full:
        for s in R["shortfalls"]:
            P("  %s %s: first non-zero %s at %s, max %s, last %s  (%s)" % (s["tag"], s["counter"], s["first_nonzero"], s["at"], s["max"], s["last"],
                                                                         s["meaning"]))
    else:
        by_tag = {}
        for s in R["shortfalls"]:
            by_tag.setdefault(s["tag"], []).append("%s %s%s" % (s["counter"], s["max"], "" if s["last"] == s["max"] else " (last %s)" % s["last"]))
        for tag, items in by_tag.items():
            ws = wrap(items, indent="", width=160 - len(tag) - 3)
            P("  %s %s" % (tag, ws[0]))
            o.extend("  " + " " * len(tag) + " " + w for w in ws[1:])
    if not R["shortfalls"]:
        P("  none")
    cap, sh = F["shots"]
    if cap is None:
        P("SHOTS: none (no captures folder beside logs\\)")
    elif not sh:
        P("SHOTS: none for this run in %s" % cap)
    elif full:
        P("SHOTS: %d in %s" % (len(sh), cap))
        for n, sz in sh:
            P("  %s %d bytes" % (n, sz))
    else:
        sheets = [n for n, _s in sh if "sheet" in n.lower()]
        rest = [n for n, _s in sh if "sheet" not in n.lower()]
        P("SHOTS: %d in %s" % (len(sh), cap))
        P("  sheet: %s; %s" % (", ".join(os.path.join(cap, n) for n in sheets) or "none",
                               ("first %s, last %s" % (rest[0], rest[-1]) if len(rest) > 1 else (rest[0] if rest else "no other picture"))))
    P("BLACK FRAMES (the host's runs of black frames: the branch that chose black <- the input that asked for it; src\\renderer_host\\black_spans.h; never the exit code)")
    bl = F["black"]
    menus = [b for b in bl if b["what"].startswith("2d_only")]
    listed = bl if full else [b for b in bl if not b["what"].startswith("2d_only")]
    for b in (listed if full else listed[:40]):
        t = " t %.2f-%.2f s (%.2f s)" % (b["t0"], b["t1"], b["t1"] - b["t0"]) if "t0" in b and "t1" in b else ""
        P("  frames %d-%d (n %d, held %d)%s: %s | %s | why: %s" % (b["first"], b["last"], b["n"], b["held"], t, b["what"], b["inputs"], b["why"] if full else b["why"][:120]))
    if not full and len(listed) > 40:
        P("  ... %d more run(s) (--full lists every one)" % (len(listed) - 40))
    if menus and not full:
        P("  plus %d 2d_only run(s), %d frames: black under the game's 2D rows (menus / loads); --full lists them" % (len(menus), sum(b["n"] for b in menus)))
    bs = F["black_sum"]
    if bs:
        P("  total: %d run(s), %d black frames, %d held%s" % (bs["spans"], bs["black"], bs["held"],
                                                              "" if bs["printed"] >= bs["spans"] else "; only the first %d runs have a line (black_spans.h kMaxLines)" % bs["printed"]))
    if bl and F.get("black_csv"):
        P("  times: %s" % F["black_csv"])
    if not bl and not bs:
        P("  none (no `[host] black frames` line: no black frame, or a build before S111 BLACKFRAME)")
    if a.expect:
        P("EXPECTED: %d of %d found%s" % (len(R["expect_found"]), len(a.expect),
                                          " (%s)" % ", ".join("line %d" % v for v in R["expect_found"].values()) if R["expect_found"] else ""))
        if R["expect_missing"]:
            P("EXPECTED NOT FOUND (in a game line; setting echoes and the route's own lines do not count)")
            for e in R["expect_missing"]:
                P("  " + e)
    if a.zero or a.zero_or_absent:
        for z in R["zero_ok"]:
            P("ZERO ok: %s: %s" % (z["zero"], "absent (--zero-or-absent)" if z["absent"] else "0 every time (%s)" % z["counter"]))
        if R["zero_failed"]:
            P("ZERO FAILED")
            for z in R["zero_failed"]:
                if z["absent"]:
                    P("  --zero %s: absent: no census printed it (the path never ran?; --zero-or-absent accepts that)" % z["zero"])
                else:
                    P("  --zero %s: %s %s first non-zero %s at %s, max %s, last %s" % (z["zero"], z["tag"], z["counter"], z["first_nonzero"], z["at"],
                                                                                    z["max"], z["last"]))
    P("UNSORTED (%d: lossy-looking counted words not in the table, non-zero at least once; the table can grow)" % len(R["unsorted"]))
    if full:
        for u in R["unsorted"]:
            P("  %s max %s last %s  (%s)" % (u["name"], u["max"], u["last"], ", ".join(u["tags"])))
    else:
        o.extend(wrap(["%s=%s" % (u["name"], u["max"]) for u in R["unsorted"]]))
    why = [w for w, c in (("PROBLEMS", R["problems"]), ("the route did not finish", R["unfinished"]), ("EXPECTED NOT FOUND", R["expect_missing"]),
                          ("ZERO FAILED", R["zero_failed"]), ("error-level groups (--strict-errors)", R["errors_failing"])) if c]
    P("EXIT %d%s" % (R["exit"], " (%s)" % ", ".join(why) if why else ""))
    return "\n".join(o)


def main(argv=None):
    ap = argparse.ArgumentParser(description="print a game check run's facts from its log (read the whole log once)")
    ap.add_argument("log")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--full", action="store_true", help="print every flag, shot, loaded item and unsorted counter")
    ap.add_argument("--expect", action="append", metavar="TEXT")
    ap.add_argument("--zero", action="append", metavar="NAME")
    ap.add_argument("--zero-or-absent", action="append", metavar="NAME")
    ap.add_argument("--allow-unfinished", action="store_true")
    ap.add_argument("--strict-errors", action="store_true")
    ap.add_argument("--allow-error", action="append", metavar="REGEX")
    a = ap.parse_args(argv)
    if a.allow_error and not a.strict_errors:
        print("run_read: --allow-error only goes with --strict-errors (error-level lines do not decide the exit without it)", file=sys.stderr)
        return 2
    parts, num, note = locate(a.log)
    if not parts:
        print("run_read: CANNOT READ: no wwe2k14_NNN.log at or under %s" % a.log, file=sys.stderr)
        return 2
    try:
        F = read_run(parts, a.expect or [])
    except OSError as e:
        print("run_read: CANNOT READ %s: %s" % (", ".join(parts), e), file=sys.stderr)
        return 2
    F["parts"], F["note"] = parts, note
    F["save_options"] = save_row(parts, num)
    F["shots"] = shots(os.path.dirname(os.path.abspath(parts[-1])), num)
    F["black_csv"] = black_times(parts, num, F["black"])   # S111 BLACKFRAME
    R = analyse(F, a)
    if a.json:
        out = {"log": parts[-1], "parts": parts, "note": note, "lines": F["lines"], "start_seen": F["start_seen"], "ts_first": F["ts_first"],
               "ts_last": F["ts_last"], "save_options": F["save_options"], "version": F["version"], "route": F["route"], "bench_done_line": F["done"], "quit_line": F["quit"],
               "unfinished": F["done"] is None, "last_scene": F["last_scene"],
               "flags": [{"name": n, "value": v, "source": s} for n, v, s in F["flags"]], "renderer": F["renderer"] or None,
               "problems": R["problems"], "error_groups": R["error_groups"],
               "loaded": {"event_props": F["props"], "event_props_made": F["made"], "characters": F["chars"], "arenas": F["arenas"],
                          "banks": F["banks"], "dlc": F["dlc"]},
               "shortfalls": R["shortfalls"], "unsorted": R["unsorted"],
               "black_spans": F["black"], "black_total": F["black_sum"], "black_csv": F["black_csv"],
               "shots": {"dir": F["shots"][0], "files": [{"name": n, "bytes": s} for n, s in F["shots"][1]]},
               "expect_found": R["expect_found"], "expect_missing": R["expect_missing"], "zero_failed": R["zero_failed"], "zero_ok": R["zero_ok"],
               "exit": R["exit"]}
        print(json.dumps(out, indent=1))
    else:
        print(render(F, R, a))
    return R["exit"]


if __name__ == "__main__":
    sys.exit(main())
