#!/usr/bin/env python3
"""triage.py - read a player report (zip), a game log, or a folder of logs and say what the player saw not drawn.

What it answers: for each game log, the machine and build, the modes entered (the `sequence A -> B (Name)` lines) and, PER MODE (the span
between two sequence changes), the counters that mean "something was not drawn / a load did not finish / the GPU died". Ends with a
FINDINGS block, one line per (mode, class), worst first, each with the first reason text and the log line where the counter first ROSE.

Call (S105 C0; docs\\agents\\report_triage.md):
  py -I tools\\report\\triage.py PATH [PATH...] [--json] [--out FILE] [--gap-s N] [--baseline FILE.json]
  PATH = a report .zip (PC launcher or phone), a wwe2k14_NNN.log, or a folder of logs. Read in place, never copied.
  Rotated parts wwe2k14_NNN.K.log are joined oldest first (highest K first), counters and the current mode carried across.
  --baseline = a reference `--json` output (a clean run): only classes / reasons / birth sites NOT in the same mode of it are reported.
  (S111: the hand-written accepted list and its `--accepted --verdict` PASS / KNOWN / FAIL are gone: triage lists findings, it does not judge them.)
Exit 0 = read fine and no RUN failed (findings or not: a picture / census / loader finding alone never fails a run);
exit 1 = a RUN failed in some log (S111, def run_failures: a [crash] or [hang] line, the route's `script FAILED`, a route with no `quit` line, no end
marker in the newest part): one `triage: RUN FAILED <log>: why` line per such log on stderr (the text on stdout stays the report the launcher's C++ twin
writes too), JSON `logs[].run_failures` ([] = the run did not fail; computed before --baseline hides any finding);
exit 2 = could not read an input (loud, never a silent "no findings").
A class whose line is present but whose format did not parse is UNPARSED; a class whose line never appears is listed "not read" (never "0").

Nothing from a report is written anywhere but stdout / --out.
"""
# entry: main
import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report_facts  # noqa: E402  S108 RPT: the IN PLAY / PROBLEMS / SHORTFALLS / ERRORS / REPRODUCE blocks (the twin of src\launcher\report_facts.h)

FACTS_TABLE = report_facts.load_table()   # src\launcher\report_facts.inc, the one table both twins read

# --------------------------------------------------------------------------------------------------------------------
# THE PATTERN TABLE. One row per class; a new class is one row (docs\agents\report_triage.md "add a class").
#   marker : plain substring a line must contain to be this class's candidate (cheap prefilter)
#   regex  : the strict parse of that line (named groups); a candidate that does not match = UNPARSED for the class
#   fields : group -> aggregation: "sum" (add up the window lines), "delta" (a cumulative counter: the mode's rise),
#            "one" (the number of lines), "count_yes", "first" (first text), "max", "set"
#   finding: the group that must be nonzero for a FINDING (None = shown but never a finding: INFO), prio: lower = worse
#   periodic: the line is a census window printed every N frames that does NOT flush on a mode change (scene_host.cpp:965-998): a rise between
#            two lines that straddle a sequence change is reported as "mode not known", never added to either mode
#   writer / lit : the FILE that prints it and the literal it must hold (any line: a line that only moves is fine; the selftest fails a literal that is gone or moved to another file)
# --------------------------------------------------------------------------------------------------------------------
TS = r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]"
CLASSES = [
    dict(name="problem", prio=0, marker="[rdr][PROBLEM]", writer="src/renderer/problem.cpp", lit="[rdr][PROBLEM] %s: %s",
         regex=r"\[rdr\]\[PROBLEM\] (?P<name>\w+): (?P<why>.*)$",
         fields={"n": "one", "why": "first"}, finding="n", text="{n} [rdr][PROBLEM] line(s)"),
    dict(name="load_stall", prio=0, marker="[load] STALL", writer="src/load_probe.h", lit="[wwe2k14][load] STALL:",
         regex=r"\[wwe2k14\]\[load\] STALL: (?P<why>.*)$",
         fields={"n": "one", "why": "first"}, finding="n", text="{n} loader STALL line(s)"),
    dict(name="load_watch", prio=0, marker="[load] WATCH", writer="src/load_probe.h", lit="[wwe2k14][load] WATCH:",   # A6W (docs\research\s105\A6W_contract.md)
         regex=r"\[wwe2k14\]\[load\] WATCH: (?P<why>.*)$",
         # S105 H: the WATCH dump is several lines (the main line, then request / read-end words and one stack per guest thread): the main line is the
         # finding, these continuation lines are detail (counted as "det", never UNPARSED). A mangled main line ("WATCH no ...") is still UNPARSED.
         detail=r"\[wwe2k14\]\[load\] WATCH (?:request|read-end job|read-end object) 0x[0-9A-Fa-f]+ words \d+\.\.\d+:|\[wwe2k14\]\[load\] WATCH stacks?[ :]|\[wwe2k14\]\[load\] WATCH (?:h\d+ fields|loader heap|where):",   # S106 0F: the A6W2 dump lines (run 164 L2153-2155, and `WATCH where:` L2188-2189, src\load_probe.h:416-431)
         fields={"n": "one", "why": "first", "det": "detail"}, finding="n", text="{n} loader WATCH line(s) (+{det} detail line(s): request words, guest stacks)"),
    dict(name="holes", prio=2, marker="[scene][ready] calls", writer="src/renderer_scene/live_scene_draw.cpp", lit="[scene][ready] calls ", periodic=True,
         regex=r"\[scene\]\[ready\] calls \d+ .*?; holes (?P<n>\d+) skipped \[(?P<rest>.*)\]; no stage",
         fields={"n": "sum", "rest": "first_hole"}, finding="n",
         text="{n} calls with a hole (a model the game draws and ours does not; counts frames, not models)"),
    dict(name="hole_skip", prio=2, marker="skip frame", writer="src/renderer_scene/rf_wrestlers.cpp", lit="[scene][wrestlers] skip frame ",
         regex=r"\[scene\]\[wrestlers\] skip frame (?P<frame>\d+) obj (?P<obj>\S+) reason (?P<why>.*?) hole (?P<hole>yes|no)\s*$",
         fields={"hole": "count_yes", "why": "first_if_yes"}, finding="hole", text="{hole} wrestler skip line(s) marked hole yes"),
    dict(name="caw_holes", prio=2, marker="[rdr][caw] guest draw: models built", writer="src/renderer_scene/guest_model_draw.h",
         lit="[rdr][caw] guest draw: models built ", periodic=True, why_const="[rdr][caw] guest draw summary",
         regex=r"guest draw: models built (?P<built>\d+), holes (?P<holes>\d+),.*?own-skeleton piece holes of ALL wrestlers (?P<osh>\d+)\)",
         fields={"osh": "delta", "holes": "delta"}, finding="osh",
         text="{osh} own-skeleton piece holes (counted over every wrestler drawn from guest rows, stock ones too, not CAW only)"),
    dict(name="caw_textures", prio=9, marker="[rdr][caw] guest draw: models built", writer="src/renderer_scene/guest_model_draw.h",
         lit="[rdr][caw] guest draw: models built ",
         regex=r"textures bound (?P<tb>\d+) from override (?P<ov>\d+) missing (?P<miss>\d+) engine_default_cube (?P<edc>\d+) \(parts without a diffuse (?P<pwd>\d+)\)",
         fields={"miss": "delta", "pwd": "delta"}, finding=None, text="missing textures +{miss}, parts without a diffuse +{pwd}"),
    dict(name="pic_refusals", prio=3, marker="picture refusals", writer="src/renderer_scene/picture_resolver.h", lit="picture refusals ", periodic=True,
         regex=r"picture refusals (?P<n>\d+): (?P<by>[^;]*)(?:; first refused key (?P<key>0x[0-9A-Fa-f]+) \((?P<kwhy>\w+)\): (?P<creator>\w+), birth site (?P<site>.*0x[0-9A-Fa-f]+))?",
         fields={"n": "delta"}, finding="n", text="{n} picture refusals (a 2D sprite whose picture the host has no source for)"),
    dict(name="black_no_3d", prio=3, marker="[host] frames", writer="src/renderer_host/host_core.h", lit="[host] frames %llu", periodic=True,
         why_const="[host] frames census",
         regex=r"\[host\] frames \d+,.*? black_no_3d (?P<n>\d+) \(mid_match (?P<mm>\d+)\)",
         fields={"n": "delta", "mm": "delta"}, finding="n", text="{n} frames with no 3D picture (black_no_3d, {mm} mid-match)"),
    dict(name="rows_not_drawn", prio=4, marker="[host] frames", writer="src/renderer_host/host_core.h", lit="[host] frames %llu", periodic=True,
         regex=r"\[host\] frames \d+,.*? 2d_rows \d+ drawn \d+ not_drawn (?P<n>\d+) \((?P<why>[^)]*)\)",
         fields={"n": "delta", "why": "pairs"}, finding="n", text="{n} 2D rows not drawn"),
    # S106 0F: the [host] frames line's `holes N` (host_core.h:527 Counters::Holes): 2D rows the game draws and the host has no draw for. Never read before.
    dict(name="holes_2d", prio=2, marker="[host] frames", writer="src/renderer_host/host_core.h", lit="[host] frames %llu", periodic=True,
         why_const="[host] frames census holes",
         regex=r"\[host\] frames \d+,.*? 2d_rows \d+ drawn \d+ not_drawn \d+ .*?holes (?P<n>\d+)",
         fields={"n": "delta"}, finding="n", text="{n} 2D holes (sprite rows the game draws and the host has no draw for)"),
    dict(name="strikes", prio=4, marker="[host][strikes]", writer="src/renderer_host/scene_host.cpp", lit="[host][strikes] ", periodic=True,
         regex=r"\[host\]\[strikes\] (?P<n>\d+) strikes: .*\| why: (?P<why>.*)$",
         fields={"n": "delta", "why": "first"}, finding="n", text="{n} strikes (frames the host could not present as a built frame)"),
    dict(name="fallback_2d", prio=4, marker=("[scene][2d] frames", "[scene][2d] exit:"), writer="src/renderer_game/fallback_census.h", lit="[scene][2d] ", cut_totals=True, scene_attr=True,
         # also "[scene][2d] exit: frames ..." (the last window of a run, then "; run totals:" which is NOT read)
         regex=r"\[scene\]\[2d\] (?:exit: )?frames (?P<frames>\d+), 2d-whole (?P<whole>\d+), fallback (?P<n>\d+) \(by reason: (?P<why>.*?)\); rows",
         fields={"n": "sum", "why": "first_nz"}, finding="n", text="{n} 2D frames fell back (not whole 2D)"),
    dict(name="technique_fallback", prio=6, marker="technique fallback:", writer="src/renderer_scene/front_end.cpp", lit="technique fallback:",
         regex=r"\[scene\]\[front_end\] technique fallback: (?P<why>\S+ technique \d+)",
         fields={"n": "one", "why": "first"}, finding="n", text="{n} technique fallback line(s)"),
    dict(name="reflect_fallback", prio=6, marker="reflect fallback:", writer="src/renderer_scene/front_end.cpp", lit="reflect fallback:",
         regex=r"\[scene\]\[front_end\] reflect fallback: (?P<why>\S+ technique \d+:.*)$",
         fields={"n": "one", "why": "first"}, finding="n", text="{n} reflect fallback line(s)"),
    dict(name="mem_alloc_failed", prio=9, marker="MmAllocatePhysicalMemoryEx failed", writer="src/kernel_overrides.h", lit="MmAllocatePhysicalMemoryEx failed",
         regex=r"MmAllocatePhysicalMemoryEx failed #(?P<k>\d+).*?size=(?P<size>0x[0-9A-Fa-f]+)",
         fields={"n": "one", "size": "set"}, finding=None, text="{n} MmAllocatePhysicalMemoryEx failures (sizes {size})"),
    dict(name="heap_alloc_failed", prio=9, marker="BaseHeap::Alloc failed", writer="thirdparty/rexglue-sdk-src/src/system/xmemory.cpp", lit="BaseHeap::Alloc failed",
         regex=r"BaseHeap::Alloc failed (?P<why>.*)$",
         fields={"n": "one"}, finding=None, text="{n} BaseHeap::Alloc failures"),
    dict(name="frames_census", prio=9, marker="[host] frames", writer="src/renderer_host/host_core.h", lit="[host] frames %llu",
         regex=r"\[host\] frames (?P<frames>\d+), ours (?P<ours>\d+), 2d_only (?P<only2d>\d+), repeat_3d (?P<rep>\d+) \(loading (?P<loading>\d+)\)",
         fields={"frames": "delta", "ours": "delta", "only2d": "delta", "rep": "delta"}, finding=None,
         text="frames +{frames}: ours {ours}, 2d only {only2d}, repeat 3d {rep}"),
    dict(name="load_adopted", prio=9, marker="[scene][load] adopted", writer="src/renderer_scene/live_scene_draw.cpp", lit="adopted #",
         regex=r"\[scene\]\[load\] adopted #\d+: (?P<what>.*), waited (?P<ms>\d+) ms",
         fields={"ms": "max", "what": "first"}, finding=None, text="slowest scene load {ms} ms"),
]
# S106 0FF B1: the crash handler's lines (src\crash_handler.h:112 abort, :122 unhandled exception, :128 guest context): a run that crashed is never normal.
# S106 0FFN N2: the hang guard's lines (src\hang_guard.h Say :122): HANG? :326, HANG: :344, BLACK: no present :314, BLACK: dark picture :382. The `over`, `picture` and
# `guard on` lines carry none of the three markers, so they are not read. Never accepted.
CLASSES.append(dict(name="hang", prio=0, marker=("[hang] HANG?", "[hang] HANG:", "[hang] BLACK:"), writer="src/hang_guard.h", lit="HANG? no %s for %u s",
                    regex=r"\[hang\] (?P<why>(?:HANG\?|HANG:|BLACK:) .*)$", fields={"n": "one", "why": "first"}, finding="n", text="{n} [hang] line(s) (the hang guard fired: HANG / BLACK)"))
CLASSES.append(dict(name="crash", prio=0, marker="[wwe2k14][crash]", writer="src/crash_handler.h", lit="[wwe2k14][crash] unhandled exception 0x{:08X}",
                    regex=r"\[wwe2k14\]\[crash\] (?P<why>.*)$", fields={"n": "one", "why": "first"}, finding="n", text="{n} [crash] line(s) (the game crashed)"))
# S106 0FF F7: the [host] frames counters that had no class (host_core.h:526-527). All 0 in a clean run; a rise is a finding.
for _ctr, _ln in (("no_record", 526), ("no_picture", 526), ("no_size", 526), ("resize_black", 526), ("repeat_full", 526), ("particles_not_drawn", 527)):
    CLASSES.append(dict(name=_ctr, prio=4, marker="[host] frames", writer="src/renderer_host/host_core.h", lit="%s %%llu" % _ctr, periodic=True,
                        regex=r"\[host\] frames \d+,.*? %s (?P<n>\d+)" % _ctr, fields={"n": "delta"}, finding="n",
                        text="{n} frames counted as " + _ctr + " (a [host] frames counter that stays 0 in a clean run)"))
# the other literals the reader depends on (checked by the selftest like the rows)
EXTRA_WRITERS = [
    ("sequence line", "src/session_log.h", "sequence {} -> {} ({})"),
    ("sequence line frame", "src/session_log.h", "[wwe2k14][scene] {} at frame {}"),
    ("file open", "src/kernel_overrides.h", "[wwe2k14][file] {} '{}'{} {} access=0x{:X}"),
    ("report condensed marker", "src/launcher/launcher_main.cpp", "[report] lines left out after their first 30 copies"),
    ("report cut marker", "src/launcher/launcher_main.cpp", "[report] ... "),
    ("black frames run line", "src/renderer_host/black_spans.h", "[host] black frames %llu-%llu (n %llu, held %llu): "),   # S111 BLACKFRAME (RX_BLACK)
]
for _c in CLASSES:
    _c["rx"] = re.compile(_c["regex"])
    if _c.get("detail"):
        _c["detail_rx"] = re.compile(_c["detail"])

RX_TS = re.compile(TS)
SEQ_MARK = "[wwe2k14][scene] sequence "
RX_SEQ = re.compile(r"\[wwe2k14\]\[scene\] sequence (?P<a>\d+) -> (?P<b>\d+) \((?P<name>.*?)\) at frame (?P<frame>\d+)")
# src\session_log.h:145 + :133 "[wwe2k14][scene] sequence A -> B (Name) at frame F, ..."
RX_FILE = re.compile(r"\[wwe2k14\]\[file\] Nt(?:Create|Open)File '(?P<path>[^']*)'(?: \(relative to handle 0x[0-9A-Fa-f]+\))? \S+ access=\S+ -> 0x(?P<st>[0-9A-Fa-f]+)")
# src\kernel_overrides.h:215-223 (NtCreateFile / NtOpenFile, optional "(relative to handle 0x..)")
RX_HOSTFRAMES = re.compile(r"\[host\] frames (?P<f>\d+),")
RX_BLACK = re.compile(r"\[host\] black frames (?P<a>\d+-\d+) \(n (?P<n>\d+), held \d+\): (?P<what>.*?) \| inputs: ")   # S111 BLACKFRAME: one run of black frames (src\renderer_host\black_spans.h)
RX_LOGSTART = re.compile(r"\[wwe2k14\] (?:version (?P<ver>.*)|run log number: (?P<num>\d+).*|quality tier: (?P<tier>.*))$")
RX_GPU = re.compile(r"\[wwe2k14\]\[gpu\] device 0: (?P<gpu>.*)$")
RX_SCENE = re.compile(r"; in scene (?P<scene>[A-Za-z0-9_]+)")
RX_PART = re.compile(r"(?:^|[\\/])wwe2k14_(\d+)(?:\.(\d+))?\.log$")
RX_QUIT = re.compile(r"\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: quit \(frame")   # S106 0F: the route's `quit` step (src\input_script.h)
RX_PAIRS = re.compile(r"([A-Za-z_]\w*) (\d+)")
_DAYS = {}
PROBLEM_NOTE = "problems are first-of-name per run: the game logs only the FIRST [rdr][PROBLEM] line of each name (src/renderer/problem.cpp:64), so a count here is not an occurrence count"


def slug(name):
    """The census's scene slug (src\\session_log.h:132 -> fallback_census SceneChange): the sequence name's parenthesis, lowercased, symbols to '_'."""
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def secs(m):
    d = m.group(1)
    if d not in _DAYS:
        y, mo, da = (int(x) for x in d.split("-"))
        _DAYS[d] = (y * 372 + mo * 31 + da) * 86400
    return _DAYS[d] + int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + int(m.group(5)) / 1000.0


def hms(m):
    return "%s:%s:%s" % (m.group(2), m.group(3), m.group(4))


class Span:
    """One visit to a mode: from a sequence change to the next one (or the end of the log)."""

    def __init__(self, seq, name, frame, t, hm):
        self.seq, self.name, self.frame0, self.t0, self.hm0 = seq, name, frame, t, hm
        self.t1 = t
        self.vals = {}      # (class, field) -> value
        self.firsts = {}    # (class, "reason"|"line") -> (text, loc, hm)
        self.base = {}      # (class, field) -> cumulative value at the start of the rise being counted
        self.last = {}      # (class, field) -> last cumulative value seen in this span
        self.carry = {}     # (class, field) -> rise banked before a counter fell mid-span
        self.unparsed = {}  # class -> (count, first loc)
        self.tally = {}     # S106 0F: (class, sub-reason token) -> count, EVERY sub-reason of every rising line (not only the first): the JSON `reasons`
        self.opens = []     # (t, host_frames) of file opens
        self.frames_at_start = None
        self.frames_at_end = None
        self.black = []     # S111 BLACKFRAME: the `[host] black frames` runs black_no_3d counts, logged in this span: "frames A-B WHAT (n N)"


def pair_diff(prev, cur_text):
    """'picture 1568, page_rows 30' against the previous line's pairs -> 'picture +0.. page_rows +30' (only the pairs that rose)."""
    cur = {k: int(v) for k, v in RX_PAIRS.findall(cur_text)}
    out = ["%s +%d" % (k, v - prev.get(k, 0)) for k, v in cur.items() if v > prev.get(k, 0)]
    return cur, (", ".join(out) if out else None)


def reason_of(c, g, diff):
    """The reason text one matched line gives (None = none)."""
    nm = c["name"]
    if nm == "holes":
        rest = g["rest"]
        if rest.strip() in ("-", ""):
            return None
        m = re.search(r"\(first: (.*)\)\s*$", rest)
        return m.group(1) if m else rest
    if nm == "pic_refusals":
        r = diff or g["by"].strip()
        if g.get("key"):
            r += " | first refused key %s (%s): %s, birth site %s" % (g["key"], g["kwhy"], g["creator"], g["site"])
        return r
    if nm == "rows_not_drawn":
        return diff or (g["why"].strip() if g["why"].strip() not in ("-", "") else None)
    if nm == "strikes":
        return g["why"].split(";")[0].strip()
    if nm == "fallback_2d":
        parts = [p.strip() for p in g["why"].split(";")]
        parts = [p for p in parts if p and not re.search(r" -$", p)]
        return "; ".join(parts) or None
    if nm == "hole_skip":
        return g["why"] if g["hole"] == "yes" else None
    if nm in ("black_no_3d", "caw_holes", "holes_2d"):
        return c.get("why_const")
    if g.get("why"):
        return ("%s: %s" % (g["name"], g["why"])) if nm == "problem" else g["why"]
    return None


RX_FAM = re.compile(r"(\w+) (\d+) \(first: (.*?)\)(?:, (?=\w+ \d+ \(first)|$)")   # S106 0FF: family, its call count, its first text


def tokens_of(c, g, diff, dl, amount):
    """S106 0F: every sub-reason token one RISING line gives, [(token, count)]: what the verdict judges (feed keeps the first reason only).
    fallback_2d: 'kind name' per by-reason entry (window frames); rows_not_drawn / pic_refusals: the pairs that rose; holes: 'family: first text';
    hole_skip / strikes / technique / reflect: the reason text; black_no_3d: 'mid_match <rise>'; holes_2d: '2d holes'; caw_holes: the own-skeleton piece holes."""
    nm = c["name"]
    out = []
    if nm == "fallback_2d":
        for part in g["why"].split(";"):
            k, _, v = part.strip().partition(" ")
            if v in ("-", "n/a", ""):
                continue
            for tok in v.split(","):
                name, sep, cnt = tok.strip().rpartition(":")
                if not sep:
                    name = "all"      # S106 0FF B3: `pass_unknown N` (fallback_census.h:240-243) names no reason: the whole count under one token no entry accepts
                if cnt.isdigit() and int(cnt):
                    out.append(("%s %s" % (k, name), int(cnt)))
        if amount > sum(n for _t, n in out):
            out.append(("uncovered", amount - sum(n for _t, n in out)))      # S106 0FF B3: frames the by-reason text does not name: no entry matches, FAIL
    elif nm in ("rows_not_drawn", "pic_refusals"):
        out = [(k, int(v)) for k, v in re.findall(r"(\w+) \+(\d+)", diff or "")]
    elif nm == "holes":
        rest = g["rest"].strip()
        if rest not in ("-", ""):
            fams = RX_FAM.findall(rest)
            out = [("%s: %s" % (f, t), int(c)) for f, c, t in fams] or [(rest[:160], 1)]
    elif nm in ("hole_skip", "technique_fallback", "reflect_fallback", "strikes"):
        r = reason_of(c, g, diff)
        if r:
            if nm in ("technique_fallback", "reflect_fallback"):
                m = re.match(r"\S+ technique \d+", r)
                r = m.group(0) if m else r
            out = [(r, 1)]
    elif nm == "black_no_3d":
        out = [("mid_match %d" % dl.get("mm", 0), amount)]
    elif nm == "holes_2d":
        out = [("2d holes", amount)]
    elif nm == "caw_holes":
        out = [("guest draw own-skeleton piece holes", amount)]
    return out


RX_ORPHAN = re.compile(r"orphan notes (\d+), pending dropped (\d+)")
END_MARKERS = ("Execution complete", "Title terminated")   # the last lines of a finished run (src\rex app exit); a log without them was cut, killed or crashed
RUN_FAIL_CLASSES = ("crash", "hang")   # S111: a line of these classes means the run itself failed (def run_failures), whatever else the log holds


def census_checks(body):
    """S106 0FF B3: the fallback census window's self-check tokens, [(token, count)]: SUM MISMATCH, UNCOUNTED n, orphan notes n, pending dropped n (all clean in a good run)."""
    out = []
    if "SUM MISMATCH" in body:
        out.append(("SUM MISMATCH", 1))
    m = re.search(r"; UNCOUNTED (\d+)", body)
    if m:
        out.append(("UNCOUNTED", int(m.group(1))))
    m = RX_ORPHAN.search(body)
    if m:
        if int(m.group(1)):
            out.append(("orphan notes", int(m.group(1))))
        if int(m.group(2)):
            out.append(("pending dropped", int(m.group(2))))
    return out


def feed(span, c, g, loc, hm, cum, pairs, crossing):
    """Add one matched line of class c into the span. Returns (amount, reason, toks): the finding field's rise / count on this line,
    its first reason, and (S106 0F) every sub-reason token of the line."""
    cname, ffield = c["name"], c["finding"]
    amount = 0
    diff = None
    dl = {}
    if "why" in c["fields"] and c["fields"]["why"] == "pairs" or cname == "pic_refusals":
        src = g.get("why") if cname == "rows_not_drawn" else g.get("by", "")
        prev = pairs.get(cname, {})
        pairs[cname], diff = pair_diff(prev, src or "")
    for fld, how in c["fields"].items():
        key = (cname, fld)
        if how == "one":
            if fld == ffield:
                amount = 1
            if not crossing:
                span.vals[key] = span.vals.get(key, 0) + 1
        elif how == "sum":
            v = int(g[fld])
            if fld == ffield:
                amount = v
            if not crossing:
                span.vals[key] = span.vals.get(key, 0) + v
        elif how == "max":
            span.vals[key] = max(span.vals.get(key, 0), int(g[fld]))
        elif how == "count_yes":
            y = 1 if g[fld] == "yes" else 0
            if fld == ffield:
                amount = y
            span.vals[key] = span.vals.get(key, 0) + y
        elif how == "delta":
            v = int(g[fld])
            b = cum.get(key, 0)
            prev = 0 if v < b else b           # a counter that fell = a new run: its rise is the new value
            dl[fld] = v - prev
            if fld == ffield:
                amount = v - prev
            if key not in span.base:
                span.base[key] = v if crossing else prev
                span.last[key] = span.base[key]
            if not crossing:
                if v < span.last[key]:         # fell mid-span: bank the rise so far, count again from 0
                    span.carry[key] = span.carry.get(key, 0) + span.last[key] - span.base[key]
                    span.base[key] = 0
                span.last[key] = v
            cum[key] = v
        elif how == "set":
            span.vals.setdefault(key, set()).add(g[fld])
    reason = reason_of(c, g, diff)
    rose = (amount > 0) if ffield is not None else True
    if not crossing and rose:
        if reason and (cname, "reason") not in span.firsts:
            span.firsts[(cname, "reason")] = (reason, loc, hm)
        if (cname, "line") not in span.firsts:
            span.firsts[(cname, "line")] = (None, loc, hm)
    toks = tokens_of(c, g, diff, dl, amount) if (ffield is not None and rose) else []
    if not crossing:
        for tk, n in toks:
            span.tally[(cname, tk)] = span.tally.get((cname, tk), 0) + n
    return amount, reason, toks


def finish_span(span):
    for key in set(span.last) | set(span.carry):
        span.vals[key] = span.carry.get(key, 0) + max(0, span.last.get(key, 0) - span.base.get(key, 0))


def read_log(name, parts):
    """Triage one log: parts = [(label, text)] oldest first. Raises ValueError when no line looks like a game log."""
    cum, pairs, cstate = {}, {}, {}
    machine, unparsed_all = {}, {}
    cur = Span(-1, "(before the first sequence line)", 0, 0.0, "")
    spans = [cur]
    slugs = {}
    crossings = []
    seen = {c["name"]: 0 for c in CLASSES}
    unstamped = {}
    unplaced = {}
    notes = []
    ts_seen = n_lines = 0
    t_first = t_last = None
    last_t = last_hm = None
    file_lines = trailer = 0
    host_frames = 0
    condensed = cut = in_trailer = False
    fails = {}
    seq_lines = seq_bad = 0
    quit_seen = script_failed = route_seen = False
    markers = [(mk, c) for c in CLASSES for mk in ((c["marker"],) if isinstance(c["marker"], str) else c["marker"])]
    multi = len(parts) > 1
    facts = report_facts.Facts(FACTS_TABLE)
    for part_i, (label, text) in enumerate(parts):
        for i, line in enumerate(text.splitlines(), 1):
            n_lines += 1
            loc = ("%s:%d" % (label, i)) if multi else i
            m = RX_TS.match(line)
            if m:
                in_trailer = False
                t = secs(m)
                last_t, last_hm = t, hms(m)
                if t_first is None:
                    t_first = t
                    cur.t0 = cur.t1 = t
                t_last = t
                ts_seen += 1
                stamped = True
            else:
                if line.startswith("[report]"):
                    if "lines left out after their first 30 copies" in line:
                        condensed = in_trailer = True
                    elif "bytes left out" in line:
                        cut = True
                    continue
                if in_trailer:
                    trailer += 1
                    continue
                if not line.strip():
                    continue
                t = last_t
                stamped = False
            hm = last_hm
            facts.feed(line, stamped, hm, part_i == len(parts) - 1)   # S108 RPT: every line, the same pass
            if stamped:
                if "[wwe2k14] " in line and ("version " in line or "run log number" in line or "quality tier" in line):
                    ms = RX_LOGSTART.search(line)
                    if ms:
                        if ms.group("ver"): machine.setdefault("version", ms.group("ver"))
                        if ms.group("num"): machine.setdefault("run_log", ms.group("num"))
                        if ms.group("tier"): machine.setdefault("quality_tier", ms.group("tier"))
                if "][gpu] device 0:" in line and "gpu" not in machine:
                    mg = RX_GPU.search(line)
                    if mg: machine["gpu"] = mg.group("gpu")
                if SEQ_MARK in line:
                    ms = RX_SEQ.search(line)
                    if ms:
                        seq_lines += 1
                        cur.t1 = t
                        cur = Span(int(ms.group("b")), ms.group("name"), int(ms.group("frame")), t, hm)
                        cur.frames_at_start = host_frames
                        spans.append(cur)
                        slugs[slug(ms.group("name"))] = int(ms.group("b"))
                    else:
                        seq_bad += 1
                        u = unparsed_all.setdefault("sequence", [0, loc, line[:200]])
                        u[0] += 1
                    continue
            if "modified: wwe2k14_input_script" in line or "[wwe2k14][input] " in line:
                route_seen = True      # S106 0FFN N1: this run had an input-script route, so it must end with the route's own quit line
            if "[wwe2k14][input] " in line:     # S106 0F: the route's own ending, for the verdict (a quiet idle before `quit` is normal, a FAILED script is not)
                if RX_QUIT.search(line):
                    quit_seen = True
                elif "[wwe2k14][input] script FAILED" in line:
                    script_failed = True
            if "NtCreateFile" in line or "NtOpenFile" in line:
                mf = RX_FILE.search(line) if "[file]" in line else None
                if mf:
                    file_lines += 1
                    cur.opens.append((t, host_frames))
                    if mf.group("st") != "00000000":
                        fails[mf.group("path")] = fails.get(mf.group("path"), 0) + 1
                    continue
            if "[host] frames" in line:
                mh = RX_HOSTFRAMES.search(line)
                if mh:
                    host_frames = int(mh.group("f"))
                    cur.frames_at_end = host_frames
            if "[host] black frames " in line:   # S111 BLACKFRAME: the runs black_no_3d counts (no_last3d / draw_fault / fault_black; not 2d_only, resize, no_record, game_draws_no_scene)
                mb = RX_BLACK.search(line)
                if mb and (mb.group("what").startswith(("no_last3d", "draw_fault")) or "fault_black" in mb.group("what")):
                    cur.black.append("frames %s %s (n %s)" % (mb.group("a"), mb.group("what"), mb.group("n")))
            for marker, c in markers:
                if marker not in line:
                    continue
                cname = c["name"]
                if c.get("detail_rx") and c["detail_rx"].search(line):
                    # S105 H: a continuation line of a multi-line class: detail, not a finding, not UNPARSED
                    cur.vals[(cname, "det")] = cur.vals.get((cname, "det"), 0) + 1
                    continue
                seen[cname] += 1
                if not stamped:
                    if last_hm is None:
                        unplaced[cname] = unplaced.get(cname, 0) + 1
                        continue
                    unstamped[cname] = unstamped.get(cname, 0) + 1
                body = line.split("; run totals:")[0] if c.get("cut_totals") else line
                g = c["rx"].search(body)
                if g is None:
                    n, ln = cur.unparsed.get(cname, (0, loc))
                    cur.unparsed[cname] = (n + 1, ln)
                    u = unparsed_all.setdefault(cname, [0, loc, line[:200]])
                    u[0] += 1
                    continue
                gd = g.groupdict()
                target = cur
                if c.get("scene_attr"):
                    sm = RX_SCENE.search(body)
                    if sm and sm.group("scene") in slugs:
                        want = slugs[sm.group("scene")]
                        for sp in reversed(spans):
                            if sp.seq == want:
                                target = sp
                                break
                if cname == "fallback_2d":      # S106 0FF B3: the census's own self-check text, after "; rows" (never read before): any failure is a finding
                    for tk, n in census_checks(body):
                        target.tally[("census_check", tk)] = target.tally.get(("census_check", tk), 0) + n
                crossing = False
                if c.get("periodic"):
                    prev_idx = cstate.get(cname)
                    here = len(spans) - 1
                    crossing = prev_idx is not None and prev_idx != here
                    cstate[cname] = here
                amount, reason, toks = feed(target, c, gd, loc, hm, cum, pairs, crossing)
                if crossing and amount > 0:
                    crossings.append(dict(cls=cname, amount=amount, from_idx=prev_idx, to_idx=len(spans) - 1, reason=reason, loc=loc, hm=hm, toks=toks))
    if ts_seen == 0:
        raise ValueError("%s: no line starts with a [YYYY-MM-DD HH:MM:SS.mmm] stamp: not a game log" % name)
    cur.t1 = t_last
    for s in spans:
        finish_span(s)
    if condensed:
        notes.append("LOG CONDENSED: the launcher left out repeated lines after their first 30 copies (%d trailer line(s) not read); counts of repeated lines are LOW" % trailer)
    if cut:
        notes.append("LOG CUT: the launcher kept only the first 4 MB and last 20 MB of an over-24 MB log; counters between them are missing (cumulative counters still rise correctly across the cut)")
    for cn, n in sorted(unplaced.items()):
        notes.append("%d line(s) of class %s came before the first stamped line: not placed in a mode, not counted" % (n, cn))
    for cn, n in sorted(unstamped.items()):
        notes.append("%d unstamped line(s) of class %s (a continuation or a broken stamp) were taken into the previous stamp's mode" % (n, cn))
    ended = any(any(mk in ln for mk in END_MARKERS) for ln in parts[-1][1].splitlines()[-400:]) if parts else False
    modes_read = seq_lines > 0
    if not modes_read:
        notes.append("NO MODE READ: no `sequence A -> B (Name)` line parsed%s, so every counter below is for the whole log, not per mode" %
                     (" (%d line(s) of the sequence format changed: UNPARSED)" % seq_bad if seq_bad else ""))
    not_read = [c["name"] for c in CLASSES if seen[c["name"]] == 0]
    if not file_lines:
        not_read.append("no_file_open")
    return dict(name=name, lines=n_lines, machine=machine, spans=spans, unparsed=unparsed_all, file_lines=file_lines, notes=notes, parts=[p[0] for p in parts],
                t_first=t_first, t_last=t_last, failed_opens=sum(fails.values()), failed_open_paths=len(fails), crossings=crossings,
                modes_read=modes_read, ended=ended, classes_not_read=not_read, condensed=condensed, cut=cut, quit_seen=quit_seen, script_failed=script_failed, route_seen=route_seen,
                  head_seen=bool(parts) and "[wwe2k14] run log number:" in parts[0][1],
                span_seqs=[s.seq for s in spans], facts=facts, marker_lines=dict(seen))


def mode_table(res, gap_s):
    """Aggregate the spans of one log by mode id (order of first visit)."""
    modes, order = {}, []
    last_span = res["spans"][-1]
    for s in res["spans"]:
        if s.seq == -1 and not (s.vals or s.firsts or s.opens or s.base or s.unparsed):
            continue
        k = s.seq
        if k not in modes:
            modes[k] = dict(seq=s.seq, name=s.name, visits=0, first_t=s.hm0, first_frame=s.frame0, seconds=0.0, frames=0,
                            vals={}, firsts={}, unparsed={}, gap=None, trail=None, visit_gaps=[], tally={})
            order.append(k)
        md = modes[k]
        md["visits"] += 1
        md["seconds"] += max(0.0, s.t1 - s.t0)
        if s.frames_at_start is not None and s.frames_at_end is not None:
            md["frames"] += max(0, s.frames_at_end - s.frames_at_start)
        for key, v in s.vals.items():
            if isinstance(v, set):
                md["vals"].setdefault(key, set()).update(v)
            else:
                md["vals"][key] = md["vals"].get(key, 0) + v
        for key, v in s.firsts.items():
            md["firsts"].setdefault(key, v)
        for key, v in s.tally.items():
            md["tally"][key] = md["tally"].get(key, 0) + v
        for cn, (n, ln) in s.unparsed.items():
            old = md["unparsed"].get(cn, (0, ln))
            md["unparsed"][cn] = (old[0] + n, old[1])
        vg = 0.0
        if res["file_lines"] and s.frames_at_start is not None:
            pts = [(s.t0, s.frames_at_start)] + list(s.opens)
            pts.append((s.t1, s.frames_at_end if s.frames_at_end is not None else s.frames_at_start))
            for idx, ((ta, fa), (tb, fb)) in enumerate(zip(pts, pts[1:])):
                if tb - ta >= gap_s and fb - fa >= 60:
                    vg = max(vg, tb - ta)
                    if md["gap"] is None or tb - ta > md["gap"][0]:
                        md["gap"] = (tb - ta, fb - fa, ta)
                    if s is last_span and idx == len(pts) - 2:
                        md["trail"] = (tb - ta, fb - fa, ta)     # the gap that runs to the END of the log
        md["visit_gaps"].append(round(vg, 1))
    return [modes[k] for k in order]


def span_label(res, i0, i1):
    names = []
    for sp in res["spans"][i0:i1 + 1]:
        n = "%d %s" % (sp.seq, sp.name)
        if n not in names:
            names.append(n)
    return " / ".join(names)


def black_why(res, seqs, cap=6):
    """S111 BLACKFRAME: the black_no_3d finding's WHY, from the run lines logged in these modes: ' | why: frames A-B no_last3d <- never_fed (n N); ...' ('' = none)."""
    runs = [b for sp in res["spans"] if sp.seq in seqs for b in sp.black]
    return (" | why: " + "; ".join(runs[:cap]) + (" (+%d more run(s))" % (len(runs) - cap) if len(runs) > cap else "")) if runs else ""


def findings_for(res, modes, gap_s):
    out = []
    has_watch = False
    for md in modes:
        for c in CLASSES:
            if c["finding"] is None:
                continue
            n = md["vals"].get((c["name"], c["finding"]), 0)
            if n:
                reason = md["firsts"].get((c["name"], "reason"), (None, None, None))
                line = md["firsts"].get((c["name"], "line"), (None, None, None))
                vals = {f: md["vals"].get((c["name"], f), 0) for f in c["fields"]}
                vals = {f: (v if not isinstance(v, set) else ",".join(sorted(v))) for f, v in vals.items()}
                if c["name"] == "load_watch":
                    has_watch = True
                out.append(dict(mode="%d %s" % (md["seq"], md["name"]), mode_seq=md["seq"], cls=c["name"], count=n, prio=c["prio"], crossing=False,
                                text=c["text"].format(**vals), reason=reason[0] or c.get("why_const") or "", line=reason[1] or line[1], at=reason[2] or line[2]))
                if c["name"] == "black_no_3d":
                    out[-1]["text"] += black_why(res, {md["seq"]})
        if md["trail"] is not None:
            g, fr, ta = md["trail"]
            out.append(dict(mode="%d %s" % (md["seq"], md["name"]), mode_seq=md["seq"], cls="no_file_open", count=int(g), prio=1, crossing=False,
                            text="no file opened for %d s while %d frames advanced" % (round(g), fr),
                            reason="the gap runs to the END of the log: a load that never finished, OR a normal quit after an idle menu (the loader WATCH line, when present, is the finding; this is supporting evidence)",
                            line=None, at=""))
        for cn, (n, ln) in md["unparsed"].items():
            out.append(dict(mode="%d %s" % (md["seq"], md["name"]), mode_seq=md["seq"], cls=cn, count=n, prio=-1, crossing=False,
                            text="UNPARSED: %d line(s) of class %s present, format not read" % (n, cn), reason="the line's format changed: update the pattern table", line=ln, at=""))
    merged = {}
    for cr in res["crossings"]:
        k = (cr["cls"], span_label(res, cr["from_idx"], cr["to_idx"]))
        if k in merged:
            merged[k]["amount"] += cr["amount"]
            merged[k]["toks"] = merged[k]["toks"] + cr["toks"]
        else:
            merged[k] = dict(cr)
    for (cn, label), cr in merged.items():
        c = next(x for x in CLASSES if x["name"] == cn)
        vals = {f: "?" for f in c["fields"]}
        vals[c["finding"]] = cr["amount"]
        out.append(dict(mode="mode not known: " + label, mode_seq=None, cls=cn, count=cr["amount"], prio=c["prio"], crossing=True,
                        text=c["text"].format(**vals) + " [rise between census lines that straddle a mode change]", reason=cr["reason"] or "", line=cr["loc"], at=cr["hm"],
                        toks=cr["toks"], straddles=sorted({sp.seq for sp in res["spans"][cr["from_idx"]:cr["to_idx"] + 1]})))
        if cn == "black_no_3d":
            out[-1]["text"] += black_why(res, set(out[-1]["straddles"]))
    for cn, (n, loc, _l) in res["unparsed"].items():
        if cn == "sequence":
            out.append(dict(mode="(whole log)", mode_seq=None, cls="sequence", count=n, prio=-1, crossing=False,
                            text="UNPARSED: %d line(s) of the sequence format present, not read" % n, reason="the sequence line's format changed: update RX_SEQ", line=loc, at=""))
    out.sort(key=lambda f: (f["prio"], -f["count"]))
    return out


def run_failures(res, finds):
    """S111 (the main session's rule after the accepted list went): why this log's RUN failed, [] when it did not. What was hand-ACCEPTED is gone (no
    list of findings that may appear and still pass); what is a run failure fails: a [crash] / [hang] line (every marker line counts, parsed or
    UNPARSED, placed or not), the route's `script FAILED`, a route (input script) that never logged its `quit` line, no end marker in the newest part.
    Picture / census / loader findings (black_no_3d, 2D holes, problem, load_watch, ...) never fail a run by themselves: they are listed as findings.
    finds = the findings BEFORE --baseline (a reference run that crashed too never hides a crash)."""
    out = []
    for cn in RUN_FAIL_CLASSES:
        n = res["marker_lines"].get(cn, 0)
        if n:
            f = next((x for x in finds if x["cls"] == cn), None)
            out.append("[%s] %d line(s)%s" % (cn, n, (": " + re.sub(r"\s+", " ", f["reason"] or f["text"])[:160]) if f else ""))
    if res.get("script_failed"):
        out.append("route: script FAILED (the route did not finish)")
    if res.get("route_seen") and not res.get("quit_seen"):
        out.append("route: no quit line (the run ended before the route finished)")
    if not res.get("ended"):
        out.append("log: no end marker (Execution complete / Title terminated in the newest part): the run was cut, killed or crashed")
    return out


def norm_reason(r):
    m = re.search(r"birth site (?:\(never seen born\) )?0x[0-9A-Fa-f]+", r or "")
    site = m.group(0) if m else ""
    r = re.sub(r"birth site (?:\(never seen born\) )?0x[0-9A-Fa-f]+", "", r or "")
    r = re.sub(r"\b[0-9A-Fa-f]{8}\b|0x[0-9A-Fa-f]+|\d+", "#", r)
    return r.strip() + "|" + site


def baseline_keys(path):
    j = json.loads(Path(path).read_text(encoding="utf8"))
    keys = set()
    for lg in j["logs"]:
        for f in lg["findings"]:
            keys.add((f["mode_seq"] if f["mode_seq"] is not None else f["mode"], f["cls"], norm_reason(f["reason"])))
    return keys


def machine_from_zip(zf):
    info = []
    names = {n.split("/")[-1]: n for n in zf.namelist()}
    if "about.txt" in names:
        for ln in zf.read(names["about.txt"]).decode("utf8", "replace").splitlines():
            if re.match(r"(Version|Made|GPU|Monitor|CPU|RAM|Windows|CPU threads):", ln) or re.match(r"\s+.*: driver ", ln):
                info.append(ln.strip())
    for nm in ("report.txt", "phone.txt"):
        if nm in names:
            for ln in zf.read(names[nm]).decode("utf8", "replace").splitlines():
                if not ln.strip():
                    if nm == "report.txt" and info:
                        break
                    continue
                if re.match(r"(Launcher|Game log|Phone|Kernel|GPU driver|Quality tier|Safe mode|Last failure|Android's exit reason|Last start-up step):", ln) \
                        or (nm == "phone.txt" and ln.startswith(("OK ", "WARN", "FAIL"))):
                    info.append(ln.strip())
                if ln.startswith("Settings (from"):
                    break
    return info


def group_parts(entries):
    """entries = [(member name, text)] -> {NNN: [(K or None, label, text)]} ordered oldest first: highest K first, the base file (newest) last."""
    groups = {}
    for nm, text in entries:
        m = RX_PART.search(nm)
        if not m:
            continue
        groups.setdefault(m.group(1), []).append((int(m.group(2)) if m.group(2) else None, nm.replace("\\", "/").split("/")[-1], text))
    for num, lst in groups.items():
        lst.sort(key=lambda p: (p[0] is None, -(p[0] or 0)))
    return groups


def part_notes(lst):
    ks = sorted(k for k, _l, _t in lst if k is not None)
    notes = []
    have_base = any(k is None for k, _l, _t in lst)
    if ks:
        gaps = [k for k in range(1, ks[-1] + 1) if k not in ks]
        if gaps or not have_base:
            notes.append("PARTS MISSING: parts %s present%s; missing between/after: %s: counters across the missing part are not read" %
                         (", ".join(["." + str(k) for k in ks] + ([] if not have_base else ["(newest)"])), "" if have_base else " but not the newest file",
                          ", ".join(".%d" % k for k in gaps) or "the newest file"))
    return notes


def gather(paths):
    """-> list of dict(source, name, parts [(label, text)], machine, notes). Raises ValueError / OSError on an unreadable input."""
    items = []
    for p in paths:
        pp = Path(p)
        if not pp.exists():
            raise ValueError("%s: no such file or folder" % p)
        if pp.is_dir():
            entries = [(f.name, f.read_text(encoding="utf8", errors="replace")) for f in sorted(pp.glob("wwe2k14_*.log"))]
            groups = group_parts(entries)
            if not groups:
                raise ValueError("%s: no wwe2k14_*.log in the folder" % p)
            for num in sorted(groups):
                lst = groups[num]
                items.append(dict(source=str(pp), name="wwe2k14_%s.log" % num, parts=[(l, t) for _k, l, t in lst], machine=[], notes=part_notes(lst)))
        elif pp.suffix.lower() == ".zip":
            try:
                zf = zipfile.ZipFile(pp)
                bad = zf.testzip()
                if bad:
                    raise ValueError("%s: bad member %s" % (p, bad))
            except zipfile.BadZipFile as e:
                raise ValueError("%s: not a readable zip (%s)" % (p, e))
            mach = machine_from_zip(zf)
            entries = [(n, zf.read(n).decode("utf8", errors="replace")) for n in zf.namelist() if RX_PART.search(n)]
            groups = group_parts(entries)
            if not groups:
                raise ValueError("%s: no wwe2k14_NNN.log inside the zip" % p)
            for num in sorted(groups):
                lst = groups[num]
                items.append(dict(source=str(pp), name="wwe2k14_%s.log" % num, parts=[(l, t) for _k, l, t in lst], machine=mach, notes=part_notes(lst)))
                mach = []
        else:
            mp = RX_PART.search(pp.name)
            if mp and not mp.group(2):      # S106 0FF F4: wwe2k14_NNN.log is the NEWEST part; its older wwe2k14_NNN.K.log parts sit beside it: read them all, oldest first
                entries = [(f.name, f.read_text(encoding="utf8", errors="replace")) for f in sorted(pp.parent.glob("wwe2k14_%s*.log" % mp.group(1)))]
                lst = group_parts(entries).get(mp.group(1))
                if lst:
                    items.append(dict(source=str(pp.parent), name=pp.name, parts=[(l, t) for _k, l, t in lst], machine=[], notes=part_notes(lst)))
                    continue
            items.append(dict(source=str(pp.parent), name=pp.name, parts=[(pp.name, pp.read_text(encoding="utf8", errors="replace"))], machine=[], notes=[]))
    return items


def render(results, baseline_hidden):
    out = []
    allf = []
    for r in results:
        res, modes, finds, mach = r["res"], r["modes"], r["findings"], r["machine_lines"]
        out.append("=" * 100)
        out.append("LOG %s  (%s)%s" % (res["name"], r["source"], ("  parts: " + ", ".join(res["parts"])) if len(res["parts"]) > 1 else ""))
        out.extend(res["facts"].render(res["lines"]))   # S108 RPT: what was in play, at the top of the log's section
        for ml in mach:
            out.append("  " + ml)
        m = res["machine"]
        bits = ["%s %s" % (k, m[k]) for k in ("version", "quality_tier", "gpu", "run_log") if k in m]
        if bits:
            out.append("  log start: " + "; ".join(bits))
        out.append("  %d lines, %.0f s of log, file opens logged: %s" % (res["lines"], (res["t_last"] or 0) - (res["t_first"] or 0),
                                                                        res["file_lines"] if res["file_lines"] else "none (class no_file_open not read)"))
        for n in r["notes"] + res["notes"]:
            out.append("  NOTE: " + n)
        out.append("  CLASSES NOT READ (their line never appears in this log: not the same as 0): " + (", ".join(res["classes_not_read"]) or "none"))
        out.append("  MODES (sequence changes), then the not-drawn counters per mode:")
        for md in modes:
            out.append("   %3d %-44s t=%s frame %-6d visits %d, %5.0f s, ~%d frames" % (md["seq"], md["name"][:44], md["first_t"], md["first_frame"],
                                                                                    md["visits"], md["seconds"], md["frames"]))
            for c in CLASSES:
                vals = {f: md["vals"].get((c["name"], f), 0) for f in c["fields"]}
                if not any(v for v in vals.values()):
                    continue
                vv = {f: (v if not isinstance(v, set) else ",".join(sorted(v))) for f, v in vals.items()}
                try:
                    out.append("         %-18s %s" % (c["name"], c["text"].format(**vv)))
                except KeyError:
                    pass
            if md["gap"] is not None:
                out.append("         %-18s longest gap with no file open %d s (%d frames advanced)%s" % (
                    "no_file_open", round(md["gap"][0]), md["gap"][1], "  <- runs to the end of the log" if md["trail"] else ""))
            for cn, (n, ln) in md["unparsed"].items():
                out.append("         %-18s UNPARSED x%d (first at log line %s)" % (cn, n, ln))
        if res["failed_opens"]:
            out.append("  INFO: %d file open attempts failed on %d distinct paths (the game probes optional files; not a finding by itself)" % (res["failed_opens"], res["failed_open_paths"]))
        out.append("  FINDINGS (worst first):")
        if not finds:
            out.append("    none%s" % (" (after the baseline)" if baseline_hidden else ""))
        for f in finds:
            ln = ("log line %s" % f["line"]) if f["line"] else "no single line"
            at = (" at %s" % f["at"]) if f["at"] else ""
            out.append("    [%s] %s: %s; first reason: %s (%s%s)" % (f["cls"], f["mode"], f["text"], (f["reason"] or "-")[:200], ln, at))
        if any(f["cls"] == "problem" for f in finds):
            out.append("    NOTE: " + PROBLEM_NOTE)
        allf.extend(finds)
    out.append("=" * 100)
    if baseline_hidden:
        out.append("BASELINE: %d finding line(s) also present in the baseline's same mode (hidden)" % baseline_hidden)
    out.append("TOTAL: %d log(s), %d finding line(s)" % (len(results), len(allf)))
    return "\n".join(out)


def to_json(results, baseline_hidden):
    j = []
    for r in results:
        res = r["res"]
        j.append(dict(source=r["source"], log=res["name"], parts=res["parts"], machine_lines=r["machine_lines"], log_start=res["machine"], lines=res["lines"],
                      file_opens_logged=res["file_lines"], notes=r["notes"] + res["notes"], modes_read=res["modes_read"], condensed=res["condensed"], cut=res["cut"],
                      classes_not_read=res["classes_not_read"],
                      modes=[dict(seq=m["seq"], name=m["name"], visits=m["visits"], seconds=round(m["seconds"], 1), frames=m["frames"],
                                  first_t=m["first_t"], first_frame=m["first_frame"],
                                  counters={"%s.%s" % k: (v if not isinstance(v, set) else sorted(v)) for k, v in m["vals"].items()},
                                  gap_s=(round(m["gap"][0], 1) if m["gap"] else None), trail_s=(round(m["trail"][0], 1) if m["trail"] else None),
                                  visit_gap_s=m["visit_gaps"], unparsed={k: v[0] for k, v in m["unparsed"].items()},
                                  reasons={c: {t: n for (c2, t), n in sorted(m["tally"].items()) if c2 == c} for c in sorted({k[0] for k in m["tally"]})})
                             for m in r["modes"]],
                      findings=r["findings"], run_failures=r.get("run_failures", []), in_play=res["facts"].to_json(res["lines"])))
    return dict(tool="triage", version=2, baseline_hidden=baseline_hidden, problems_note=PROBLEM_NOTE, logs=j)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Read a report zip / game log / folder of logs and list what the player saw not drawn.")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json", action="store_true", help="print one JSON object instead of the text")
    ap.add_argument("--out", help="also write the output to this file (under out\\)")
    ap.add_argument("--gap-s", type=float, default=10.0, help="report a span with frames advancing and no file open longer than this (default 10)")
    ap.add_argument("--baseline", help="a reference --json output: report only classes / reasons / birth sites not in the same mode of it (a comparison, not a verdict)")
    a = ap.parse_args(argv)
    try:
        items = gather(a.paths)
        bkeys = baseline_keys(a.baseline) if a.baseline else None
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as e:
        print("triage: CANNOT READ: %s" % e, file=sys.stderr)
        return 2
    results = []
    hidden = 0
    for it in items:
        try:
            res = read_log(it["name"], it["parts"])
        except ValueError as e:
            print("triage: CANNOT READ: %s (%s)" % (e, it["source"]), file=sys.stderr)
            return 2
        modes = mode_table(res, a.gap_s)
        finds = findings_for(res, modes, a.gap_s)
        rfail = run_failures(res, finds)      # S111: before --baseline, so a crash the reference run also had still fails this run
        if bkeys is not None:
            keep = [f for f in finds if f["prio"] < 0 or (f["mode_seq"] if f["mode_seq"] is not None else f["mode"], f["cls"], norm_reason(f["reason"])) not in bkeys]
            hidden += len(finds) - len(keep)
            finds = keep
        results.append(dict(source=it["source"], res=res, modes=modes, machine_lines=it["machine"], notes=it["notes"], findings=finds, run_failures=rfail))
    text = json.dumps(to_json(results, hidden), indent=1) if a.json else render(results, hidden)
    print(text)
    for r in results:
        if r["run_failures"]:
            print("triage: RUN FAILED %s: %s" % (r["res"]["name"], "; ".join(r["run_failures"])), file=sys.stderr)
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(text + "\n", encoding="utf8")
    return 1 if any(r["run_failures"] for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
