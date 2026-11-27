#!/usr/bin/env python
# S105 A1b: every switch a launcher or the GRAPHICS menu sets has a reader that the STREAM-FREE player build compiles AND runs,
# or the control that sets it is hidden in that build. A NECESSARY condition only: RESOLUTION passes it (it is read) and is still
# broken (row A1); the step check's `[start] settings` / `[gfx][present]` log lines cover the rest.
#
#   py -I tools\launcher\settings_reader_lint.py            the table + verdict, exit 0 / 1 (2 = could not run: fail closed)
#   py -I tools\launcher\settings_reader_lint.py --selftest planted faults P1-P6 (each must FAIL) + the real-fault proof
#   py -I tools\launcher\settings_reader_lint.py --assume-live   every row marked live (the past-fault re-find)
#
# entry: main
#
# Inputs (docs\research\s105\A1b.md "The lint"):
#   producers  the switch names the launchers / the in-game menu emit (text of the producer functions, listed in PRODUCERS)
#   readers    `py tools\q\q.py ours cvar NAME --json --all` per switch (the clang index, both builds)
#   manifest   tools\launcher\settings_map.txt (setting -> switches -> status), tools\launcher\sdk_off_units.txt
# Rules: R1 the switch is defined in the stream-free build (or the SDK); R2 a value read in the stream-free build that runs;
#        R3 a RefuseSetting row names it -> refused; R4 per setting: all live = live, some = partly (every dead one acknowledged),
#        none = dead / refused (the status must be hidden / pending / known). A producer switch no row names is an ERROR.
import copy
import fnmatch
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
Q = os.path.join(ROOT, "tools", "q", "q.py")

# (file, function start regex, kind): the body runs to the first line that starts with "}" (namespace-scope inline functions).
PRODUCERS = [
    ("src/gfx_settings.h", r"^inline .*\bGraphicsFlags\(", "gfx"),
    ("src/gfx_settings.h", r"^inline .*\bNativeRendererFlags\(", "gfx"),
    ("src/launcher/launcher_core.h", r"^inline .*\bGameFlags\(", "arg"),
    ("src/launcher/launcher_core.h", r"^inline .*\bDlcFlags\(", "arg"),
    ("src/launcher_mobile/mobile_settings.h", r"^inline .*\bGameArgs\(", "arg"),
]
# Whole files that set switches by name (the in-game menu's ACCEPT, the precompile boot).
SETTERS = ["src/gfx_menu.h", "src/precompile.h"]
UI_FILES = {"pc": "src/launcher/launcher_main.cpp", "phone": "src/launcher_mobile/mobile_main.cpp", "menu": "src/gfx_menu.h",
            "pccode": "src/launcher/launcher_main.cpp", "phonecode": "src/launcher_mobile/mobile_main.cpp", "menucode": "src/gfx_menu.h",
            "core": "src/launcher/launcher_core.h"}
UI_CALL = re.compile(r"OptionRow|OptionToggle|Toggle|Choice|GroupTitle|SubHeading|MenuRow|BuildUpscaleOrder|AddRow")
GUARD = re.compile(r"kStreamFreeTarget|WWE2K14_STREAM\(\)")
GUARD_WINDOW = 80
# A reader found in these is a reporter / producer, not the game reading the setting.
REPORTER = ("src/perf_report.h", "src/bookmark.h", "src/case/", "src/replay_carried.h", "src/xdet_check.h", "src/inspector.h",
            "src/gfx_applied.h", "src/gfx_menu.h", "src/gfx_settings.h", "src/dlaa.h", "src/launcher/", "src/launcher_mobile/", "tools/")
READ_RE = re.compile(r"REXCVAR_GET\(|GetFlagByName\(|GetFlagInfo\(|Query<|\bFlag(?:Text|Int|Num|IsTrue)\(|\bflag_text\(|\bflag\(")
NOT_READ_RE = re.compile(r"SetFlagByName|GetFlagSource|RegisterChangeCallback")
OK_STATUS_DEAD = ("hidden", "pending", "known", "invisible")
# The rows v0.4.0 shipped dead (the past-fault re-find): with every row marked live the lint must fail on exactly these.
# (ASPECT (PC) and SCREEN SHAPE (phone) were dead in v0.4.0 and are live since S105 A2: the stream-free build fills the screen in play.)
# (PREPARE SHADERS (PC) was dead in v0.4.0 and is live since S105 B3: the precompile boot builds the stream-free store's own list, src\precompile.h owner mode; the row's switch is wwe2k14_precompile.)
# (RENDERER FLAG is not in it: wwe2k14_light_renderer has a compiled reader, light_renderer.h, that sets SDK flags which are inert in the stream-free build.
# The lint is blind to that; the row is marked hidden! and the launcher / menu guards are checked by text.)
EXPECTED_DEAD = {"UPSCALING (PC)", "UPSCALING (menu)", "SAFE MODE step 1 (phone)"}


class Fail(Exception):
    pass


sys.path.insert(0, os.path.join(ROOT, "tools", "split"))
from read_unit import read_text  # a split file is read with its parts expanded (tools/split/read_unit.py; text mode: '\n' line ends)


def read(path):
    return read_text(os.path.join(ROOT, path)).split("\n")


# ---- inputs -------------------------------------------------------------------------------------------------------------------

def producer_switches():
    """name -> [(file, line)] from the producer functions and the setter files."""
    out = {}
    for path, start_re, kind in PRODUCERS:
        lines = read(path)
        i = next((k for k, l in enumerate(lines) if re.search(start_re, l)), None)
        if i is None:
            raise Fail("producer function not found: %s %s" % (path, start_re))
        j = i
        while j < len(lines) and not lines[j].startswith("}"):
            j += 1
        for k in range(i, j + 1):
            code = lines[k].split("//")[0]
            if "const char*" in code:
                continue
            pats = [r'"--([a-z0-9_]+)'] if kind == "arg" else [r'push_back\(\{\s*"([a-z0-9_]+)"', r'^\s*\{\s*"([a-z0-9_]+)"\s*,']
            for p in pats:
                for m in re.finditer(p, code):
                    out.setdefault(m.group(1), []).append((path, k + 1))
    for path in SETTERS:
        for k, l in enumerate(read(path)):
            for m in re.finditer(r'SetFlagByName\(\s*"([a-z0-9_]+)"', l.split("//")[0]):
                out.setdefault(m.group(1), []).append((path, k + 1))
    return out


def parse_manifest(path):
    rows, refusals, forwards, readers = [], [], [], []
    for n, l in enumerate(read(path), 1):
        s = l.strip()
        if not s or s.startswith("#"):
            continue
        p = [x.strip() for x in s.split("|")]
        if p[0] == "FORWARD":
            if len(p) < 4:
                raise Fail("%s:%d: FORWARD needs: FORWARD | from | to | file" % (path, n))
            forwards.append({"from": p[1], "to": p[2], "file": p[3], "line": n})
        elif p[0] == "READER":
            if len(p) < 3:
                raise Fail("%s:%d: READER needs: READER | switch | file" % (path, n))
            readers.append((p[1], p[2]))
        elif p[0] == "REFUSAL":
            if len(p) < 4:
                raise Fail("%s:%d: REFUSAL needs: REFUSAL | label | file | switches" % (path, n))
            refusals.append({"label": p[1], "file": p[2], "switches": [x.strip() for x in p[3].split(",") if x.strip()], "line": n})
        else:
            if len(p) < 6:
                raise Fail("%s:%d: a row needs: SETTING | surface | switches | status | ui | why" % (path, n))
            rows.append({"name": p[0], "surface": p[1], "switches": [x.strip() for x in p[2].split(",") if x.strip()], "status": p[3],
                         "ui": [x.strip() for x in p[4].split(";") if x.strip()], "why": p[5], "line": n})
    return rows, refusals, forwards, readers


def sdk_off_units(path):
    return [l.strip() for l in read(path) if l.strip() and not l.strip().startswith("#")]


def q_cvar(name):
    r = subprocess.run([sys.executable, "-I", Q, "ours", "cvar", name, "--json", "--all"], cwd=ROOT, capture_output=True, text=True)
    try:
        d = json.loads(r.stdout[r.stdout.index("{"):])
    except Exception:
        raise Fail("q ours cvar %s gave no JSON (rc %s): %s" % (name, r.returncode, (r.stderr or r.stdout)[-300:]))
    return d


def refusal_sites():
    """Every RefuseSetting( call: [(file, line, label)] from q's callers (the clang index)."""
    r = subprocess.run([sys.executable, "-I", Q, "ours", "callers", "wwe2k14::start::RefuseSetting", "--json"], cwd=ROOT, capture_output=True, text=True)
    try:
        d = json.loads(r.stdout[r.stdout.index("{"):])
    except Exception:
        raise Fail("q ours callers RefuseSetting gave no JSON: %s" % (r.stderr or r.stdout)[-300:])
    out, stale = [], set()
    for m in d.get("matches", []):
        for s in m.get("sites", []):
            for ln in s.get("lines", []):
                lines = read(s["file"])
                for k in sorted(range(max(0, ln - 4), min(len(lines), ln + 3)), key=lambda x: abs(x - (ln - 1))):  # the exact line first; the index can be a few lines off after an edit
                    mm = re.search(r'RefuseSetting\(\s*"([^"]+)"', lines[k])
                    if mm:
                        out.append((s["file"], k + 1, mm.group(1)))
                        break
                else:  # the index is stale by more than the window: this file is scanned as text below
                    stale.add(s["file"])
    for f in stale:
        out = [o for o in out if o[0] != f]
        for k, l in enumerate(read(f)):
            mm = re.search(r'RefuseSetting\(\s*"([^"]+)"', l)
            if mm:
                out.append((f, k + 1, mm.group(1)))
    return sorted(set(out))


def gather(switches):
    with ThreadPoolExecutor(max_workers=6) as ex:
        res = list(ex.map(q_cvar, switches))
    return dict(zip(switches, res))


def ui_text():
    return {k: read(v) for k, v in UI_FILES.items()}


# ---- rules --------------------------------------------------------------------------------------------------------------------

def is_value_read(u):
    if u["kind"] == "get":
        return True
    if u["kind"] != "string":
        return False
    t = u.get("text", "")
    return bool(READ_RE.search(t)) and not NOT_READ_RE.search(t)


def switch_verdict(sw, qd, surface, off_units, refused_by, reader_ok=()):
    """-> (verdict live|refused|dead|undefined, evidence string)."""
    defs = qd["defs"]
    ok_def = [d for d in defs if d["origin"] == "SDK" or "OFF" in d["builds"] or "Android" in d["builds"] and surface == "phone"]
    if not ok_def:
        return "undefined", "no definition in the stream-free build (defs: %s)" % ", ".join("%s:%d %s" % (d["file"], d["line"], d["builds"]) for d in defs)
    if sw in refused_by:
        return "refused", "RefuseSetting %s" % refused_by[sw]
    live = []
    for u in qd["uses"]:
        b = u["builds"]
        if not (("OFF" in b) or (surface == "phone" and "Android" in b)):
            continue
        if not is_value_read(u):
            continue
        if u["origin"] != "SDK" and u["file"].startswith(REPORTER) and (sw, u["file"]) not in reader_ok:
            continue
        if u["origin"] == "SDK" and any(u["file"].startswith(x) for x in off_units):
            continue
        live.append(u)
    if not live:
        return "dead", "no value read in the stream-free build (an SDK graphics / presenter unit does not run there)"
    return "live", ", ".join("%s:%d" % (u["file"], u["line"]) for u in live[:2])


def row_verdict(row, sv):
    """sv = {switch: (verdict, evidence)} -> (verdict, dead switches)."""
    vs = [sv[s][0] for s in row["switches"] if s in sv]
    dead = [s for s in row["switches"] if s in sv and sv[s][0] != "live"]
    if not vs or len(dead) == len(vs):
        return ("refused" if vs and all(sv[s][0] == "refused" for s in row["switches"] if s in sv) else "dead"), dead
    return ("partly" if dead else "live"), dead


def expand(row, produced):
    """Glob switches (gpu_native_*) -> the produced names they cover."""
    out = []
    for s in row["switches"]:
        out += sorted(p for p in produced if fnmatch.fnmatch(p, s)) if "*" in s else [s]
    return out


def evaluate(ctx, assume_live=False):
    """-> (report lines, errors, warns, row verdicts {name: verdict})."""
    produced, rows, refusals = ctx["produced"], copy.deepcopy(ctx["rows"]), ctx["refusals"]
    ctx = dict(ctx, reader_ok=set(tuple(x) for x in ctx["readers"]))
    errors, warns, report, verdicts = [], [], [], {}
    if assume_live:
        for r in rows:
            if r["status"] != "invisible":
                r["status"] = "live"
    refused_by = {}
    sites = {(f, lab) for f, _, lab in ctx["refuse_sites"]}
    for rf in refusals:
        if not any(lab == rf["label"] and f == rf["file"] for f, lab in sites):
            errors.append("REFUSAL %r: no RefuseSetting(%r ...) call in %s any more (manifest row at %d)" % (rf["label"], rf["label"], rf["file"], rf["line"]))
        for s in rf["switches"]:
            refused_by[s] = rf["label"]
    for f, ln, lab in ctx["refuse_sites"]:
        if lab != "?" and not any(lab == rf["label"] for rf in refusals):
            errors.append("%s:%d RefuseSetting(%r): the manifest has no REFUSAL row for it (a new refusal cannot land unmapped)" % (f, ln, lab))
    covered = set()
    sv_cache = {}
    for row in rows:
        sws = expand(row, produced)
        row["switches_x"] = sws
        covered.update(sws)
        if row["status"] == "invisible":
            verdicts[row["name"]] = "invisible"
            report.append("%-34s %-6s invisible  %s" % (row["name"], row["surface"], row["why"]))
            continue
        sv = {}
        for s in sws:
            key = (s, row["surface"])
            if key not in sv_cache:
                if s not in ctx["q"]:
                    raise Fail("internal: no q data for %s" % s)
                fw = next((f for f in ctx["forwards"] if f["from"] == s), None)
                if fw:
                    src = " ".join(read(fw["file"]))
                    ok = all(re.search(r"\b%s\b" % re.escape(x), src) for x in (fw["from"], fw["to"]))
                    if not ok:
                        errors.append("FORWARD %s -> %s: %s does not name both any more (manifest row %d)" % (fw["from"], fw["to"], fw["file"], fw["line"]))
                    v, ev = switch_verdict(fw["to"], ctx["q"][fw["to"]], row["surface"], ctx["off_units"], refused_by, ctx["reader_ok"])
                    sv_cache[key] = (v, "forwarded by %s to %s: %s" % (fw["file"], fw["to"], ev))
                else:
                    sv_cache[key] = switch_verdict(s, ctx["q"][s], row["surface"], ctx["off_units"], refused_by, ctx["reader_ok"])
            sv[s] = sv_cache[key]
        row["switches"] = sws
        v, dead = row_verdict(row, sv)
        verdicts[row["name"]] = v
        st = row["status"]
        base = st.split(":")[0].rstrip("!")  # "hidden!" = hidden on purpose although a reader is compiled (the lint cannot see that it does nothing)
        report.append("%-34s %-6s %-8s [%s]" % (row["name"], row["surface"], v, st))
        for s in sws:
            report.append("    %-44s %-9s %s" % (s, sv[s][0], sv[s][1]))
        for s in sws:
            if sv[s][0] == "undefined":
                errors.append("%s: %s is not defined in the stream-free build (R1: the command line drops it silently)" % (row["name"], s))
        if v == "live":
            if base == "partly":
                warns.append("%s: marked partly but every switch has a live reader" % row["name"])
            elif base in ("hidden", "pending", "known") and not st.split(":")[0].endswith("!"):
                warns.append("%s: marked %s but every switch has a live reader (stale mark?)" % (row["name"], base))
        elif v == "partly":
            acked = set(x.strip() for x in st.split(":", 1)[1].split(",")) if base == "partly" and ":" in st else set()
            miss = [s for s in dead if s not in acked]
            if base in OK_STATUS_DEAD:
                pass
            elif miss:
                errors.append("%s: dead switch(es) with no acknowledgement in status partly: %s" % (row["name"], ", ".join(miss)))
            elif base != "partly":
                errors.append("%s: computed partly but status is %r" % (row["name"], st))
            for a in acked - set(dead):
                warns.append("%s: acknowledged dead switch %s is live now" % (row["name"], a))
        else:  # dead / refused
            if base not in OK_STATUS_DEAD:
                errors.append("%s: computed %s (%s) but status is %r: hide the control, or mark it pending:<row> / known:<why>" %
                              (row["name"], v, "; ".join("%s %s" % (s, sv[s][1]) for s in dead[:3]), st))
            elif base == "pending":
                warns.append("%s: %s, pending %s" % (row["name"], v, st.split(":", 1)[-1]))
            elif base == "known":
                warns.append("%s: %s, known issue (%s)" % (row["name"], v, st.split(":", 1)[-1]))
        if base == "hidden":
            for u in row["ui"]:
                if ":" not in u:
                    errors.append("%s: ui entry %r needs FILE-KEY:LABEL" % (row["name"], u))
                    continue
                key, label = u.split(":", 1)
                lines = ctx["ui"][key]
                if key.endswith("code") or key == "core":  # a code line, not a control: the label is plain text
                    hits = [i for i, l in enumerate(lines) if label in l.split("//")[0]]
                else:
                    hits = [i for i, l in enumerate(lines) if '"%s"' % label in l and UI_CALL.search(l)]
                if not hits:
                    errors.append("%s: control %r not found in %s (the row's ui entry is stale)" % (row["name"], label, UI_FILES[key]))
                for i in hits:
                    window = lines[max(0, i - GUARD_WINDOW):i + 1]
                    if not any(GUARD.search(w) for w in window):
                        errors.append("%s: %s:%d control %r is not behind a stream-free guard (kStreamFreeTarget / WWE2K14_STREAM()) within %d lines above" %
                                      (row["name"], UI_FILES[key], i + 1, label, GUARD_WINDOW))
    for name, locs in sorted(produced.items()):
        if name not in covered:
            errors.append("%s emitted at %s:%d has no row in settings_map.txt (a new flag cannot land unmapped)" % (name, locs[0][0], locs[0][1]))
    for row in rows:
        for s in row["switches"]:
            if s not in produced and not any("*" in x for x in row["switches"]):
                warns.append("%s: manifest switch %s is emitted by no producer (stale row?)" % (row["name"], s))
    return report, errors, warns, verdicts


def build_ctx():
    produced = producer_switches()
    rows, refusals, forwards, readers = parse_manifest("tools/launcher/settings_map.txt")
    off_units = sdk_off_units("tools/launcher/sdk_off_units.txt")
    need = set()
    for row in rows:
        if row["status"] == "invisible":
            continue
        need.update(expand(row, produced))
    need.update(f["to"] for f in forwards)
    q = gather(sorted(need))
    return {"produced": produced, "rows": rows, "refusals": refusals, "forwards": forwards, "readers": readers, "off_units": off_units, "q": q, "refuse_sites": refusal_sites(),
            "ui": ui_text()}


# ---- planted faults -----------------------------------------------------------------------------------------------------------

def plant(ctx, which):
    c = copy.deepcopy(ctx)
    if which == "P1":  # drop_reader: SGSR's reader gone -> QUALITY / SGSR rows must fail
        d = c["q"]["present_effect"]
        d["uses"] = [u for u in d["uses"] if not (u["file"] == "src/renderer_host/host_start.cpp")]
        want = {"SGSR UPSCALING (phone)", "QUALITY (PC)"}
    elif which == "P2":  # new_flag: a flag with no row
        c["produced"]["wwe2k14_fake_setting"] = [("src/launcher/launcher_core.h", 1)]
        want = {"*fake"}
    elif which == "P3":  # sdk_only: resolution_scale's only reader is an SDK unit that does not run
        d = c["q"]["resolution_scale"]
        d["uses"] = [u for u in d["uses"] if u["origin"] == "SDK" and u["file"].startswith("src/graphics/") and u["kind"] in ("get", "string")][:1]
        if not d["uses"]:
            d["uses"] = [{"file": "src/graphics/pipeline/texture/cache.cpp", "line": 282, "origin": "SDK", "kind": "get", "builds": "ON and OFF", "text": "REXCVAR_GET(resolution_scale)"}]
        want = {"RESOLUTION (PC)"}
    elif which == "P4":  # refused_marked_live: UPSCALING marked live although RefuseSetting("DLSS / DLAA") refuses it (S105 A2: was ASPECT, now live)
        for r in c["rows"]:
            if r["name"] == "UPSCALING (PC)":
                r["status"] = "live"
        want = {"UPSCALING (PC)"}
    elif which == "P5":  # undefined_in_off: V-SYNC's stream-free definition removed
        d = c["q"]["wwe2k14_vsync"]
        d["defs"] = [x for x in d["defs"] if "OFF" not in x["builds"]]
        want = {"V-SYNC (PC)"}
    elif which == "P6":  # unguarded_hidden: the stream-free guard text removed from the launcher source
        c["ui"] = {k: [GUARD.sub("always_shown", l) for l in v] for k, v in c["ui"].items()}
        want = {"UPSCALING (PC)"}
    else:
        raise Fail("unknown plant " + which)
    return c, want


def selftest(ctx):
    bad = 0
    rep, errs, warns, verdicts = evaluate(ctx)
    if errs:
        print("selftest: the real tree is not clean, planted faults prove nothing:")
        for e in errs:
            print("  ERROR", e)
        return 1
    for p in ("P1", "P2", "P3", "P4", "P5", "P6"):
        c, want = plant(ctx, p)
        _, e, _, _ = evaluate(c)
        hit = bool(e) and (want == {"*fake"} and any("wwe2k14_fake_setting" in x for x in e) or any(any(w in x for w in want) for x in e))
        print("selftest %s: %s (%d error(s))" % (p, "FAILS as planted (good)" if hit else "DID NOT FAIL: the lint passed a planted fault", len(e)))
        if not hit:
            bad += 1
    rep, errs, warns, verdicts = evaluate(ctx, assume_live=True)
    failing = {n for n, v in verdicts.items() if v in ("dead", "refused", "partly")}
    # past-fault re-find: with every row marked live the failing rows must be the dead ones v0.4.0 shipped with (partly rows are
    # not faults: they are acknowledged in the manifest, and a row marked live that is partly is reported by its error text)
    dead_rows = {n for n, v in verdicts.items() if v in ("dead", "refused")}
    flagged = {n for n in verdicts if any(e.startswith(n + ":") for e in errs)}
    print("selftest real-fault proof: marked live, these rows have no live reader at all (dead / refused): %s" % ", ".join(sorted(dead_rows)))
    print("selftest real-fault proof: every row that fails marked live (the dead ones above, plus rows partly dead): %s" % ", ".join(sorted(flagged)))
    if not EXPECTED_DEAD <= dead_rows:
        print("selftest real-fault proof: MISSED %s" % ", ".join(sorted(EXPECTED_DEAD - dead_rows)))
        bad += 1
    extra = dead_rows - EXPECTED_DEAD
    if extra:
        print("selftest real-fault proof: dead rows beyond today's list (new faults, or the list is stale): %s" % ", ".join(sorted(extra)))
        bad += 1
    if not bad:
        print("selftest: PASS (6 planted faults fail, the real-fault proof re-finds %d dead rows)" % len(EXPECTED_DEAD))
    return 1 if bad else 0


def main(argv):
    try:
        ctx = build_ctx()
        if "--selftest" in argv:
            return selftest(ctx)
        report, errors, warns, _ = evaluate(ctx, assume_live="--assume-live" in argv)
    except Fail as e:
        print("settings_reader_lint: cannot run (fails closed): %s" % e)
        return 2
    if "-q" not in argv:
        print("\n".join(report))
    for w in warns:
        print("WARN", w)
    for e in errors:
        print("ERROR", e)
    print("settings_reader_lint: %s (%d error(s), %d warning(s))" % ("FAIL" if errors else "PASS", len(errors), len(warns)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
