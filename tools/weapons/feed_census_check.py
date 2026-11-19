#!/usr/bin/env python3
"""feed_census_check.py - a log tripwire for "a live reader whose input never arrives in the stream-free build" (S103, lane FCC).

Answers: in a stream-free (KEEP_STREAM=OFF) live log, does every reader that the census shows running also show its input
ARRIVING, for the whole scene window, and is every census the scene needs present at all?  Tonight's faults reached a game run
before anyone saw them: readers dead after the Ours return, the plane module null, the plane node never stamping, weapons
read but never drawn (WC), the CAW camera (used 0) and textures (missing 49). The text lint tools\\weapons\\ours_feed_lint.py
scans one source shape; this reads what the build SAYS.

  py tools\\weapons\\feed_census_check.py LOGFILE [--waivers FILE]   one line per reader; exit 1 on any unwaived FAULT
  py tools\\weapons\\feed_census_check.py --selftest                 fixtures + plants (tools\\weapons\\fixtures\\*.txt)
  py tools\\weapons\\feed_census_check.py --lint                     BITS == kInputs, census strings exist in src, waivers, fixtures
  py tools\\weapons\\feed_census_check.py --cut LOG OUT [plane|caw|weapons]   write a counters-only fixture from a log

Verdicts: ok | n/a | waived | FAULT = NO MODULE, DEAD FEED, DIED PART-WAY, TEXTURES MISSING, CENSUS MISSING, MAP NOT MADE, MAP NOT BOUND
(the last two, S103 MSF: rows plane_made / plane_bound judge OUR plane chain from the `[scene][maps][e2]` and `plane_bound` census).

What is judged, and by which rule (each from the game's code, not from a capture):
  * The scene window = [caw enter frame (or the last `scene=NAME (frame N)` line), caw leave frame or the census's last frame].
  * A bit is REQUIRED unless (a) it is in the drop mask the log prints at `caw enter ... drop mask 0xM` (kCawDrop,
    inputs_core.h:118-133), (b) it is sphere_occ: RegisterAll skips it (inputs_core.h:141) and only the sphere hook's
    registration requires it (wwe2k14_app.h:1042-1046); the census cannot say whether that hook registered, so it is n/a here,
    (c) it is an arena-only bit (arena_worlds, ring_rows, stage_clock) and arena_models never stamped.
  * A required bit must stamp inside the window: last-stamp frame /fN within SLACK of the window end, and stamps covering COVER of it.
  * A census the game's state lines say must exist and does not is CENSUS MISSING, never n/a (caw enter -> CAW textures + guest
    models; weapon_rows calls > 0 -> weapons slots + drawn; match ours > 0 -> strikes; every BITS name on the inputs line).
  * NO MODULE (plane read 0, no_module > 0) is expected when the plane census says `structure a/b/c, participants N` with a
    structure byte != 0 or N >= 3 (the game builds no plane module unless G+18/+19/+24 are 0: docs\\research\\s103\\SHD.md PLB).
    Until the log carries those fields the scoped waiver `scene=structure_match` in docs\\renderer\\OURS_INPUT_AUDIT.md is the fallback.
Waivers: `WAIVE <reader> | [scene=<known key> |] <reason>` in docs\\renderer\\OURS_INPUT_AUDIT.md (section "Waivers").
"""
import os, re, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDIT = os.path.join(ROOT, "docs", "renderer", "OURS_INPUT_AUDIT.md")
FIX = os.path.join(ROOT, "tools", "weapons", "fixtures")
INPUTS_CORE = os.path.join(ROOT, "src", "scene_live", "inputs_core.h")

# kInputs order = bit index (inputs_core.h:73-90; a lint keeps this equal to the source).
BITS = ["post_settings", "model_runtime", "sphere_occ", "wrestler_palette", "light_states", "arena_worlds", "ring_rows",
        "caster_entries", "circle_points", "plane_constants", "crowd_snapshot", "particles", "camera", "material_params",
        "screens", "stage_pools", "arena_models", "stage_clock"]
ARENA_ONLY = {"arena_worlds", "ring_rows", "stage_clock"}
# S103 TQF (CAWV Q3, docs\research\s103\CAWV.md): in a CAW window the game stops submitting 3D draws while saving, so the bits the 3D draws feed
# stop at the last body draw (the draw-camera census prints `last_body_draw N`). Judged up to it (+ SLACK); after it they are n/a. Not a waiver by script name.
DRAW_DRIVEN = {"light_states", "arena_worlds", "caster_entries", "particles", "camera", "stage_pools", "arena_models", "stage_clock"}
PLANE_BIT = "plane_constants"
SLACK = 30      # [L fitted] frames a bit's last stamp may trail the window end (the census prints while frames run; lag 2 + the print race)
COVER = 0.5     # [L fitted] share of the window a required bit must stamp; no value exists in the code
KNOWN_SCOPES = {"structure_match"}
# the log names a structure match only by its script's scene label (lv2_weapons = quick type 72, two participants); the plane census's
# own `structure a/b/c, participants N` fields replace this tag once they exist.
SCENE_TAGS = {"structure_match": ["lv1_weapons", "lv2_weapons"]}

# every census string the checker matches (the lint finds each inside a string literal in src)
CENSUS_KEYS = ["[scene][inputs] moved ", "(open) ", "plane read ", "plane_eyes read ", "plane_proxies rows ", "draw_camera source ",
               "[rdr][caw] textures: parts ", "guest models calls ", "no descriptor ", "textures: samplers ", "[scene][weapons] slots ",
               "[rdr][weapons] drawn ", "[scene][weapon_rows] calls ", "weapon calls ", "caw enter #", "caw leave #", "drop mask 0x",
               "match ours ", "[host][strikes] ", "ticks_dropped", "plane made ", " plane_bound ", " plane_wanted_plain "]
OPTIONAL_KEYS = ["structure ", "participants "]   # the plane census addition (a separate builder); not yet in src


def scene_tags(text):
    return {t for t, names in SCENE_TAGS.items() if any(("scene=" + n) in text for n in names)}


def waiver_lines(path):
    out = []
    try:
        for ln in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"\s*(?:[-*]\s*)?WAIVE\s+(\w+)\s*\|\s*(?:scene=(\w+)\s*\|\s*)?(.+)", ln)
            if m:
                out.append((m.group(1), m.group(2), m.group(3).strip()))
    except OSError:
        pass
    return out


def waivers(path):
    """reader -> list of (scope or None, reason)."""
    out = {}
    for r, sc, why in waiver_lines(path):
        out.setdefault(r, []).append((sc, why))
    return out


def waiver_problems(path):
    bad = []
    for r, sc, why in waiver_lines(path):
        if sc is not None and sc not in KNOWN_SCOPES:
            bad.append("waiver for %s: unknown scope scene=%s (known: %s)" % (r, sc, ", ".join(sorted(KNOWN_SCOPES))))
        if why.startswith("scene="):
            bad.append("waiver for %s: the reason starts with scene= (a scope written without its separating |)" % r)
    return bad


def last(rx, text, flags=0):
    ms = list(re.finditer(rx, text, flags))
    return ms[-1] if ms else None


def inputs_line(text):
    cand = [ln for ln in text.splitlines() if "[scene][inputs] moved" in ln]
    return cand[-1] if cand else ""


def life(s, f, opn, wstart, wend):
    """-> (verdict, detail) for a required bit that stamped s times, last at frame f."""
    det = "stamps %d/f%d" % (s, f)
    if s == 0:
        return "DEAD FEED", det + (" (open: the census says no reader is built)" if opn else " (stamp 0 in the scene window)")
    w = (wend - wstart) if (wend is not None and wend > wstart) else None
    if w and f < wstart:
        return "DEAD FEED", det + " (the last stamp is before the scene window starts at %d)" % wstart
    if w and f < wend - SLACK:
        return "DIED PART-WAY", det + " (window ends %d, %d frames later; slack %d [L fitted])" % (wend, wend - f, SLACK)
    if w and min(s, w) / float(w) < COVER:
        return "DEAD FEED", det + " (stamps cover %d%% of the %d-frame window; min %d%% [L fitted])" % (100 * min(s, w) // w, w, int(COVER * 100))
    return "ok", det


E2_PLANE = r"plane made (\d+) \(draws (\d+); ([^;]*); no-draw frames (\d+); blank-plane frames (\d+), passes skipped (\d+); no-caster frames (\d+); chain-failed frames (\d+);[^)]*?seen (\d+)\)"   # S103: the pre-MSF shape (five tools parse it), seen M at the end
MADE_SHARE = 0.9   # [L fitted] share of the plane-read frames the map must be made for (blank / no-draw frames excluded); the game's rule is "every frame with a module and a drawable square", no number exists in the code


def plane_chain_rows(rows, text, inl, plane, partial, plane_dropped, drop):
    """S103 MSF (docs\\research\\s103\\MSH.md): rows that judge OUR plane chain, not the feed. Run 013 read the plane every match frame and drew no mat shadow, and the OFF host never printed the
    `[scene][maps][e2] ... plane made N` line, so it read ok. plane_made: made >= MADE_SHARE of the frames read, blank-plane and no-draw frames excluded (the game's own empty frames);
    plane_bound: draws bound to the plane-shadow variant > 0 whenever a map was made. plane_made is CENSUS MISSING when the plane was read and the line is absent."""
    if plane_dropped:
        return
    rd = int(plane.group(1)) if plane else 0
    # S103 LV5: the OFF host prints two e2 lines per window (the drawer's own census + the live census's copy, one of them 0 when its
    # hook never drew); cumulative counters, so the line with the most `seen` is the run's real one, not simply the last line
    e2s = list(re.finditer(E2_PLANE, text))
    e2 = max(e2s, key=lambda m: (int(m.group(9)), int(m.group(1)))) if e2s else None
    if not e2:
        if rd > 0 and not partial:
            rows.append(("plane_made", "CENSUS MISSING", "plane read %d but no `[scene][maps][e2] ... plane made N (... seen M)` line (the drawer's Census() is not printed: the OFF host must print it)" % rd))
        return
    made, seen, draws = int(e2.group(1)), int(e2.group(9)), int(e2.group(2))
    nodraw, blank, nocast, chainf = int(e2.group(4)), int(e2.group(5)), int(e2.group(7)), int(e2.group(8))
    reads = rd if rd > 0 else seen
    det = "made %d of %d seen, plane read %d, blank %d, no-draw %d, no-caster %d, chain-failed %d, last skip: %s" % (made, seen, reads, blank, nodraw, nocast, chainf, e2.group(3).strip() or "ok")
    need = reads - blank - nodraw
    if reads == 0:
        rows.append(("plane_made", "n/a", det + " (the plane was never read)"))
    elif need > 0 and made < MADE_SHARE * need:
        rows.append(("plane_made", "MAP NOT MADE", det + " (made %d%% of the %d drawable frames; min %d%% [L fitted])" % (100 * made // need, need, int(MADE_SHARE * 100))))
    else:
        rows.append(("plane_made", "ok", det))
    pb = last(r"\bplane_bound (\d+)", text)
    if made == 0:
        rows.append(("plane_bound", "n/a", "no plane map was made"))
    elif not pb:
        rows.append(("plane_bound", "CENSUS MISSING", "plane made %d but no `plane_bound N` (the [scene][draw] census line)" % made))
    else:
        wp = last(r"\bplane_wanted_plain (\d+)", text)
        det2 = "plane_bound %s%s, plane made %d" % (pb.group(1), (", ring-mat draws that wanted it and got a plain variant %s" % wp.group(1)) if wp else "", made)
        if int(pb.group(1)) > 0:
            rows.append(("plane_bound", "ok", det2))
        else:
            rows.append(("plane_bound", "MAP NOT BOUND", det2 + " (maps made, no mat draw read one: raw map (chain failed %d) or the mat takes a plain variant)" % chainf))


def evaluate(text):
    """-> (rows, tags). rows = [(reader, verdict, detail)]; verdict 'ok' | 'n/a' | a FAULT name."""
    partial = text.lstrip().startswith("# partial")
    rows = []
    tags = scene_tags(text)
    inl = inputs_line(text)

    ce = last(r"caw enter #\d+ at frame (\d+) \(sequence \d+, drop mask 0x([0-9a-fA-F]+)\)", text)
    cl = last(r"caw leave #\d+ at frame (\d+)", text)
    caw = ce is not None
    drop = int(ce.group(2), 16) if caw else 0
    scenes = [m for m in re.finditer(r"scene=(\w+) \(frame (\d+)\)", text) if m.group(1) != "end"]
    wstart = int(ce.group(1)) if caw else (int(scenes[-1].group(2)) if scenes else 0)

    bits = {}
    mm = re.search(r"\[scene\]\[inputs\] moved \d+ of \d+,(.*?)(?:\[blocks|$)", inl)
    if mm:
        for b in re.finditer(r"(\w+) (\(open\) )?(\d+)/f(\d+)", mm.group(1)):
            bits[b.group(1)] = (int(b.group(3)), int(b.group(4)), bool(b.group(2)))
    last_run = bits["post_settings"][1] if "post_settings" in bits else (max(v[1] for v in bits.values()) if bits else None)
    wend = last_run
    if caw and cl and last_run is not None:
        wend = min(int(cl.group(1)), last_run)
    if last_run is None or wend is None or wend <= wstart:
        wend = None   # no window to judge partial death against (a partial fixture)

    lb = re.search(r"last_body_draw (\d+)", inl)
    lbd = int(lb.group(1)) if lb and int(lb.group(1)) > 0 else None

    if not inl and not partial:
        rows.append(("inputs_census", "CENSUS MISSING", "no [scene][inputs] moved line (not a stream-free live log, or the census was not printed)"))

    # ---- the stamp bits
    for idx, b in enumerate(BITS):
        if b not in bits:
            if inl and not partial:
                rows.append((b, "CENSUS MISSING", "the name is absent from the [scene][inputs] line"))
            continue
        s, f, opn = bits[b]
        if b == "sphere_occ":
            rows.append((b, "n/a", "stamps %d/f%d; not required in this build: RegisterAll skips it (inputs_core.h:141), only the sphere hook's registration requires it (wwe2k14_app.h:1042-1046)" % (s, f)))
            continue
        if caw and (drop >> idx) & 1:
            rows.append((b, "n/a", "stamps %d/f%d; in the CAW drop mask 0x%x (kCawDrop, inputs_core.h:118-133)" % (s, f, drop)))
            continue
        if b in ARENA_ONLY and bits.get("arena_models", (0, 0, False))[0] == 0:
            rows.append((b, "n/a", "stamps %d/f%d; arena_models never stamped, so there is no arena in this scene" % (s, f)))
            continue
        if b == PLANE_BIT and s == 0:
            continue   # the plane rows below judge a bit that never stamped
        bend = wend
        if caw and b in DRAW_DRIVEN and lbd is not None and wend is not None and wstart < lbd < wend:
            bend = lbd   # CAWV Q3: the game submits no 3D draw after the last body draw; the bit is n/a after it
        v, det = life(s, f, opn, wstart, bend)
        if bend != wend and v == "ok":
            det += " (judged to last_body_draw %d, the game submits no 3D draw after it while saving: CAWV Q3; window ends %d)" % (lbd, wend)
        if b == "camera":
            dc = re.search(r"draw_camera source \S+: calls (\d+) accepted (\d+).*?used (\d+)", inl)
            if dc:
                det += ", draw-thread slot reads accepted %s, used %s" % (dc.group(2), dc.group(3))
        rows.append((b, v, det))

    # ---- the plane family
    pc = bits.get(PLANE_BIT)
    plane = re.search(r"\[plane read (\d+), no_module (\d+)", inl)
    eyes = re.search(r"\[plane_eyes read (\d+), no_module (\d+)", inl)
    prox = re.search(r"\[plane_proxies rows (\d+), no_copy (\d+)", inl)
    st = re.search(r"structure (\w+)/(\w+)/(\w+)", inl)
    pt = re.search(r"participants (\d+)", inl)
    have_struct = bool(st and pt)
    plane_dropped = caw and (drop >> BITS.index(PLANE_BIT)) & 1

    def plane_row(name, m):
        if plane_dropped:
            rows.append((name, "n/a", "plane_constants is in the CAW drop mask 0x%x: the editor has no ring, no plane node" % drop)); return
        if not m:
            if inl and not partial:
                rows.append((name, "CENSUS MISSING", "no [%s read ...] segment on the inputs line" % name))
            return
        rd, nm = int(m.group(1)), int(m.group(2))
        sv = pc[0] if pc else None
        det = "read %d, no_module %d, plane_constants %s" % (rd, nm, sv if sv is not None else "absent")
        if rd == 0 and nm > 0:
            if have_struct:
                expected = any(x != "0" for x in st.groups()) or int(pt.group(1)) >= 3
                sd = "structure %s/%s/%s, participants %s" % (st.group(1), st.group(2), st.group(3), pt.group(1))
                if expected:
                    rows.append((name, "ok", det + " (no module is expected: %s)" % sd))
                else:
                    rows.append((name, "NO MODULE", det + " (module missing on every frame; NOT expected: %s)" % sd))
            else:
                rows.append((name, "NO MODULE", det + " (module missing on every frame)"))
        elif rd > 0 and sv is not None and sv == 0:
            rows.append((name, "DEAD FEED", det + " (read Ok > 0 but stamp 0)"))
        elif rd == 0 and nm == 0:
            rows.append((name, "n/a", det + " (idle)"))
        else:
            rows.append((name, "ok", det))

    plane_row("plane", plane)
    plane_row("plane_eyes", eyes)
    if have_struct:
        tags.discard("structure_match")   # the census states it: the scene-name waiver must not hide a 1v1 with no module
    plane_chain_rows(rows, text, inl, plane, partial, plane_dropped, drop)
    if prox:
        rows.append(("plane_proxies", "ok", "rows %s, no_copy %s (cause line for a dead plane stamp; not judged alone)" % (prox.group(1), prox.group(2))))

    # ---- CAW textures + guest models (required once the CAW was entered)
    caw_tx = {}
    for m in re.finditer(r"\[rdr\]\[caw\] textures: parts (\d+), bound (\d+), from override (\d+), missing (\d+)(?: \(model (\w+))?", text):
        caw_tx[m.group(5) or "?"] = (int(m.group(1)), int(m.group(2)), int(m.group(4)))
    if caw_tx:
        miss = sum(v[2] for v in caw_tx.values())
        bound = sum(v[1] for v in caw_tx.values())
        rows.append(("caw_textures", "TEXTURES MISSING" if miss > 0 else "ok", "%d model(s), bound %d, missing %d" % (len(caw_tx), bound, miss)))
    elif caw:
        rows.append(("caw_textures", "CENSUS MISSING", "caw enter was logged but no [rdr][caw] textures line"))
    else:
        rows.append(("caw_textures", "n/a", "no CAW entered"))

    gm = last(r"guest models calls (\d+),[^\n]*?no descriptor (\d+)", text)
    if gm:
        calls, nd = int(gm.group(1)), int(gm.group(2))
        det = "calls %d, no descriptor %d" % (calls, nd)
        if calls > 0 and nd == calls:
            rows.append(("guest_models", "DEAD FEED", det + " (every call without a descriptor)"))
        elif calls == 0 and caw:
            rows.append(("guest_models", "DEAD FEED", det + " (the CAW was entered and no guest model was read)"))
        else:
            rows.append(("guest_models", "ok" if calls else "n/a", det))
    elif caw:
        rows.append(("guest_models", "CENSUS MISSING", "caw enter was logged but no guest models census"))
    gt = last(r"textures: samplers (\d+) bound (\d+) override \d+ missing (\d+)", text)
    if gt and int(gt.group(1)) > 0:
        s, b, ms = (int(gt.group(i)) for i in (1, 2, 3))
        rows.append(("guest_textures", "TEXTURES MISSING" if ms > 0 else "ok", "samplers %d, bound %d, missing %d" % (s, b, ms)))

    # ---- weapons: calls > 0 needs the slots + drawn lines; rows and draws must follow (debug lines never count)
    wr = last(r"\[scene\]\[weapon_rows\] calls (\d+)[^\n]*?\brows (\d+)", text)
    ws = last(r"\[scene\]\[weapons\] slots (\d+)", text)
    dr = last(r"\[rdr\]\[weapons\] drawn (\d+)", text)
    wcalls = int(wr.group(1)) if wr else 0
    if wcalls > 0:
        if not ws:
            rows.append(("weapons_slots", "CENSUS MISSING", "[scene][weapon_rows] calls %d but no [scene][weapons] slots line" % wcalls))
        if not dr:
            rows.append(("weapons_drawn", "CENSUS MISSING", "[scene][weapon_rows] calls %d but no [rdr][weapons] drawn line" % wcalls))
        nrows = int(wr.group(2))
        rows.append(("weapon_rows", "ok" if nrows > 0 else "DEAD FEED", "calls %d, rows %d%s" % (wcalls, nrows, "" if nrows > 0 else " (read but never recorded)")))
    if ws and int(ws.group(1)) > 0 and dr:
        d = int(dr.group(1))
        rows.append(("weapons", "ok" if d > 0 else "DEAD FEED", "slots %s, drawn %d%s" % (ws.group(1), d, "" if d > 0 else " (named, never drawn)")))
    elif not wcalls:
        rows.append(("weapons", "n/a", "no weapon calls in the census"))

    # ---- host strikes: a match with Ours frames needs the line (or the host census's `strikes 0`); a struck frame is an input that never arrived
    mo = [m for m in re.finditer(r"match ours (\d+)/(\d+)", text) if int(m.group(1)) > 0]
    sk = last(r"\[host\]\[strikes\] (\d+) strikes: ([^|]*)", text)
    hc = last(r", strikes (\d+), ticks_dropped", text)
    if mo and not sk and not hc:
        rows.append(("host_strikes", "CENSUS MISSING", "match ours > 0 but neither a [host][strikes] line nor the host census's `strikes N`"))
    elif sk or hc:
        n = int(sk.group(1)) if sk else int(hc.group(1))
        w = (wend - wstart) if wend is not None else (bits["post_settings"][0] if "post_settings" in bits else None)
        det = "%d strikes%s of %s window frames" % (n, (": " + sk.group(2).strip()) if sk else "", w if w else "?")
        if w and (1.0 - min(n, w) / float(w)) < COVER:
            rows.append(("host_strikes", "DEAD FEED", det + " (built under %d%% of the window [L fitted])" % int(COVER * 100)))
        else:
            rows.append(("host_strikes", "ok", det))
    else:
        rows.append(("host_strikes", "n/a", "no Ours match frames and no strikes"))
    return rows, tags


def judge(rows, wv, tags=()):
    fault = 0
    out = []
    for name, v, det in rows:
        if v not in ("ok", "n/a"):
            hit = [w for w in wv.get(name, []) if w[0] is None or w[0] in tags]
            if hit:
                out.append((name, "waived", "%s: %s [waiver%s: %s]" % (v, det, " scene=" + hit[0][0] if hit[0][0] else "", hit[0][1][:90])))
                continue
            fault += 1
        out.append((name, v, det))
    return out, fault


def run_text(text, wv):
    rows, tags = evaluate(text)
    out, fault = judge(rows, wv, tags)
    print("scene tags: %s" % (", ".join(sorted(tags)) or "none"))
    for name, v, det in out:
        print("%-16s %-17s %s" % (name, v, det))
    return fault


def strip_braces(s):
    return re.sub(r"\{[^{}]*\}", "", s)


def cut(log, outfile, kind="plane"):
    """a counters-only fixture (no game data; `{...}` blocks hold guest floats and are stripped)."""
    text = open(log, encoding="utf-8", errors="replace").read()
    inl = inputs_line(text)

    def one(rx, src=text):
        m = last(rx, src)
        assert m, "no %s in %s" % (rx[:40], log)
        return strip_braces(m.group(0))

    out = ["# partial census fixture (counters only), cut from %s by --cut %s" % (os.path.basename(log), kind)]
    for t, names in sorted(SCENE_TAGS.items()):
        for n in names:
            m = last(r"scene=%s \(frame \d+\)" % n, text)
            if m:
                out.append("[input] " + m.group(0))
    if kind == "plane":
        out.append("[scene][inputs] moved 18 of 18, %s %s" % (one(r"plane_constants \d+/f\d+", inl),
                   " ".join(one(r"\[%s [^\]]*\]" % k, inl) for k in ("plane_proxies", "plane read", "plane_eyes"))))
    elif kind == "caw":
        out.append(one(r"\[scene\]\[kind\] caw enter #\d+ at frame \d+ \(sequence \d+, drop mask 0x[0-9a-fA-F]+\)"))
        m = last(r"\[scene\]\[kind\] caw leave #\d+ at frame \d+", text)
        if m:
            out.append(m.group(0))
        out.append(one(r"\[rdr\]\[caw\] textures: parts \d+, bound \d+, from override \d+, missing \d+") + " (model X)")
        out.append("[scene][live] " + one(r"guest models calls \d+, [^\n]*?no descriptor \d+"))
        out.append("[scene][live] " + one(r"textures: samplers \d+ bound \d+ override \d+ missing \d+"))
        out.append("[scene][inputs] moved 18 of 18, %s, %s, %s %s" % (one(r"post_settings \d+/f\d+", inl), one(r"camera \d+/f\d+", inl),
                   one(r"arena_models \d+/f\d+", inl), one(r"draw_camera source \S+: calls \d+ accepted \d+ .*?used \d+", inl)))
    elif kind == "weapons":
        out.append(one(r"\[scene\]\[weapons\] slots \d+ \(ids [^)]*\)"))
        out.append(one(r"\[rdr\]\[weapons\] drawn \d+ \(ids [^)]*\), unknown id \d+, no model \d+"))
        out.append(one(r"\[scene\]\[weapon_rows\] calls \d+ objects \d+,[^\n]*?\brows \d+"))
    else:
        raise SystemExit("unknown cut kind " + kind)
    with open(outfile, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def fixture_problems():
    bad = []
    if not os.path.isdir(FIX):
        return ["no fixtures folder"]
    for fn in sorted(os.listdir(FIX)):
        if not fn.endswith(".txt"):
            continue
        bad += fixture_text_problems(fn, open(os.path.join(FIX, fn), encoding="utf-8").read())
    return bad


def fixture_text_problems(fn, t):
    bad = []
    if "{" in t or "}" in t:
        bad.append("%s: holds a { } block (guest floats): fixtures are counters only" % fn)
    if len(t) > 6000:
        bad.append("%s: %d bytes, over 6000: a fixture is a few census lines" % (fn, len(t)))
    for i, ln in enumerate(t.splitlines(), 1):
        if ln and not (ln.startswith("#") or ln.startswith("[")):
            bad.append("%s:%d: not a census line (comment or a [tag] line only)" % (fn, i))
    return bad


def lint(bits=None, keys=None):
    bad = []
    try:
        src = open(INPUTS_CORE, encoding="utf-8", errors="replace").read()
        blk = src[src.index("kInputs[kInputCount] = {"):]
        names = re.findall(r'\{kIn\w+, "(\w+)"', blk[:blk.index("};")])
        if names != (bits or BITS):
            bad.append("BITS != kInputs names in inputs_core.h: checker %s, source %s" % (bits or BITS, names))
    except (OSError, ValueError) as e:
        bad.append("cannot read kInputs from %s: %s" % (INPUTS_CORE, e))
    texts = []
    for d, _, fs in os.walk(os.path.join(ROOT, "src")):
        for fn in fs:
            if fn.endswith((".h", ".cpp", ".inc", ".hpp")):
                try:
                    texts.append(open(os.path.join(d, fn), encoding="utf-8", errors="replace").read())
                except OSError:
                    pass
    for key in (keys or CENSUS_KEYS):
        rx = re.compile(r'"[^"\n]*' + re.escape(key))   # the key sits inside a string literal (a log call builds the line from it)
        if not any(key in t and rx.search(t) for t in texts):
            bad.append("census string %r is matched by the checker but is in no string literal under src (the LVF shape: a line the checker waits for that the game never prints)" % key)
    for key in OPTIONAL_KEYS:
        rx = re.compile(r'"[^"\n]*' + re.escape(key))
        if not any(key in t and rx.search(t) for t in texts):
            print("note: %r is not in src yet (the plane census addition); the structure rule stays on the scoped waiver until it lands" % key)
    bad += waiver_problems(AUDIT)
    bad += fixture_problems()
    return bad


def selftest():
    bad = 0

    def expect(label, text, want_fault, wv=None, want_in=None):
        nonlocal bad
        rows, tags = evaluate(text)
        out, fault = judge(rows, wv or {}, tags)
        ok = (fault > 0) == want_fault
        if ok and want_in:
            ok = any(n == want_in[0] and v == want_in[1] for n, v, _ in out)
        print("%s  %s (fault rows %d)" % ("PASS" if ok else "FAIL", label, fault))
        if not ok:
            bad += 1
            for r in out:
                print("      ", r)

    def check(label, cond):
        nonlocal bad
        print("%s  %s" % ("PASS" if cond else "FAIL", label))
        bad += 0 if cond else 1

    def fx(name):
        return open(os.path.join(FIX, name), encoding="utf-8").read()

    P = "# partial\n"
    real = waivers(AUDIT)
    p10, p08, c11, w09, w10 = fx("plane_010.txt"), fx("plane_008.txt"), fx("caw_011.txt"), fx("weapons_009.txt"), fx("weapons_010.txt")

    # --- the plane family
    expect("log 010 plane lines, no waivers -> exit 1, NO MODULE", p10, True, want_in=("plane", "NO MODULE"))
    expect("log 010 (structure match) + the audit's waivers -> exit 0, plane waived", p10, False, wv=real, want_in=("plane", "waived"))
    expect("log 008 plane lines (read 2490, stamps 0) -> exit 1, DEAD FEED", p08, True, want_in=("plane", "DEAD FEED"))
    expect("log 008 plane lines + the audit's waivers -> still exit 1 (the CAW waiver is gone)", p08, True, wv=real, want_in=("plane", "DEAD FEED"))
    doc = p08.replace("[plane read 2490,", "[plane read 5000,").replace("plane_constants 0/f0", "plane_constants 5000/f5000")
    assert doc != p08
    expect("doctored plane read 5000 / plane_constants 5000/f5000 -> exit 0", doc, False)
    plain = p10.replace("scene=lv1_weapons", "scene=lv1_ring")
    assert plain != p10
    expect("a plain-ring log, plane module missing, no structure fields -> exit 1 with the audit's waivers", plain, True, wv=real, want_in=("plane", "NO MODULE"))
    def with_struct(t, a, n):
        return t.replace("[plane_proxies", "structure %s, participants %d [plane_proxies" % (a, n))

    tri = with_struct(plain, "0/0/0", 3)
    expect("plain-ring triple threat (participants 3), no module -> exit 0", tri, False, wv=real, want_in=("plane", "ok"))
    one = with_struct(plain, "0/0/0", 2)
    expect("plain-ring 1v1 (participants 2), no module -> exit 1", one, True, wv=real, want_in=("plane", "NO MODULE"))
    tlc = with_struct(plain, "0/1/0", 2)
    expect("a structure byte != 0, no module -> exit 0", tlc, False, wv=real)
    one_tagged = with_struct(p10, "0/0/0", 2)
    expect("1v1 with no module in a lv1_weapons-labelled log -> exit 1 (the census fields beat the scene-name waiver)", one_tagged, True, wv=real, want_in=("plane", "NO MODULE"))
    capture = P + "[scene][kind] caw enter #1 at frame 10 (sequence 76, drop mask 0x640)\n[rdr][caw] textures: parts 1, bound 1, from override 0, missing 0\n[scene][live] guest models calls 4, no descriptor 0\n" + p08
    expect("CAW with plane_constants in the drop mask -> plane n/a, exit 0", capture, False, want_in=("plane", "n/a"))
    nodrop = P + "[scene][kind] caw enter #1 at frame 10 (sequence 76, drop mask 0x0)\n[rdr][caw] textures: parts 1, bound 1, from override 0, missing 0\n[scene][live] guest models calls 4, no descriptor 0\n" + p08
    expect("CAW with an empty drop mask -> plane DEAD FEED again", nodrop, True, want_in=("plane", "DEAD FEED"))
    with tempfile.TemporaryDirectory() as d:
        wf = os.path.join(d, "w.md")
        open(wf, "w").write("- WAIVE plane | scene=structure_match | one\nWAIVE plane_eyes | scene=structure_match | two\n")
        expect("a scoped waiver passes its own scene tag", p10, False, wv=waivers(wf))
        expect("the same scoped waiver does not pass another scene", plain, True, wv=waivers(wf))
        open(wf, "w").write("- WAIVE plane | scene=caw_editor | gone\nWAIVE plane_eyes | scene=structure_match, no pipe\nWAIVE x | fine\n")
        probs = waiver_problems(wf)
        check("waiver lint: an unknown scope is refused", any("unknown scope" in p for p in probs))
        check("waiver lint: a reason that starts with scene= is refused", any("starts with scene=" in p for p in probs))
        check("waiver lint: the audit's own waivers are clean", not waiver_problems(AUDIT))
    expect("an unscoped waiver for another reader hides nothing", p08, True, wv={"sphere_occ": [(None, "x")]})

    # --- CAW: kCawDrop from the log, the camera, textures, guest models
    expect("log 011 CAW cut -> exit 1, camera: 2 stamps, last f7317 of a window ending 7753", c11, True, want_in=("camera", "DIED PART-WAY"))
    expect("log 011 CAW cut: textures missing 49", c11, True, want_in=("caw_textures", "TEXTURES MISSING"))
    expect("log 011 CAW cut: guest textures missing 245", c11, True, want_in=("guest_textures", "TEXTURES MISSING"))
    expect("log 011 CAW cut: arena_models is in the CAW window and stamped, ok", c11, True, want_in=("arena_models", "ok"))
    expect("CAW entered with no textures / guest models census -> CENSUS MISSING", P + "[scene][kind] caw enter #1 at frame 10 (sequence 76, drop mask 0x640)\n", True, want_in=("caw_textures", "CENSUS MISSING"))
    expect("CAW entered with no guest models census -> CENSUS MISSING", P + "[scene][kind] caw enter #1 at frame 10 (sequence 76, drop mask 0x640)\n", True, want_in=("guest_models", "CENSUS MISSING"))
    expect("plant: guest models, every call no descriptor", P + "[scene][live] guest models calls 50, named, no descriptor 50, x\n", True, want_in=("guest_models", "DEAD FEED"))
    expect("plant: guest textures missing", P + "[scene][live] textures: samplers 10 bound 4 override 0 missing 6\n", True, want_in=("guest_textures", "TEXTURES MISSING"))
    expect("guest textures all bound -> exit 0", P + "[scene][live] textures: samplers 10 bound 10 override 0 missing 0\n", False, want_in=("guest_textures", "ok"))

    # --- weapons (the re-find of known bug WC, log 009)
    expect("log 009 weapons cut (slots 13, drawn 0, rows 0) -> exit 1", w09, True, want_in=("weapons", "DEAD FEED"))
    expect("log 009 weapons cut: rows 0 with calls > 0", w09, True, want_in=("weapon_rows", "DEAD FEED"))
    expect("log 010 weapons cut (drawn 7, rows > 0) -> exit 0", w10, False, want_in=("weapons", "ok"))
    expect("weapon_rows calls > 0 with no slots / drawn lines -> CENSUS MISSING", P + "[scene][weapon_rows] calls 5 objects 2, x, rows 3, frames 4\n", True, want_in=("weapons_slots", "CENSUS MISSING"))
    expect("debug weapon_rows frame lines alone count for nothing", P + "[wwe2k14][weapon_rows] frame 4758: object 0xAA156CC0 instance 0 own submit true\n", False, want_in=("weapons", "n/a"))

    # --- strikes
    mo = "[scene][kind] match ours 5/10 (50.0%)\n"
    expect("match ours > 0 with no strikes census -> CENSUS MISSING", P + mo, True, want_in=("host_strikes", "CENSUS MISSING"))
    expect("match ours > 0 with the host census's `strikes 0` -> ok", P + mo + "blah, strikes 0, ticks_dropped 3 of 9\n", False, want_in=("host_strikes", "ok"))
    expect("plant: strikes on most window frames", P + "[scene][inputs] moved 18 of 18, post_settings 100/f100\n[host][strikes] 80 strikes: no_last3d 80 | why\n", True, want_in=("host_strikes", "DEAD FEED"))
    check("no Ours frames and no strikes -> n/a stays n/a", dict((n, v) for n, v, _ in evaluate(P)[0]).get("host_strikes") == "n/a")

    # --- the bits
    def line(**kw):
        d = {b: "9/f9" for b in BITS}
        d.update(kw)
        return "[scene][inputs] moved 18 of 18, " + ", ".join("%s %s" % (b, d[b]) for b in BITS) + " [blocks the frame: none]\n"
    full = line()
    plane_ok = " [plane read 9, no_module 0] [plane_eyes read 9, no_module 0]"
    def maps(made, seen, bound, blank=0, chainf=0, nodraw=0, wanted=0):
        # counters-only census lines (no game data): the e2 line of LiveMapsHook::CensusLine and the plane_bound part of LiveSceneDraw::Census
        return ("[scene][maps][e2] rim made 5 (objects 2, meshes 3; ok), plane made %d (draws 4; ok; no-draw frames %d; blank-plane frames %d, passes skipped %d; no-caster frames 0; chain-failed frames %d; mask passes 0, of them empty 0; seen %d), pre-light made 1 (slots 1; ok)\n"
                "[scene][draw] mirror sheets 0 (failed 0); live frames 5 shadow_raw_bound 0 plane_bound %d plane_wanted_plain %d wet_unknown 0\n" % (made, nodraw, blank, blank, chainf, seen, bound, wanted))
    base = full.rstrip("\n") + plane_ok.replace("read 9", "read 1000") + "\n"
    expect("a clean full census -> exit 0", base + maps(990, 1000, 4000), False, want_in=("plane_made", "ok"))
    # --- S103 MSF: OUR plane chain (run 013: plane read every frame, no mat shadow, the line never printed)
    expect("MSF: plane read 1000, no [scene][maps][e2] line (run 013) -> CENSUS MISSING", base, True, want_in=("plane_made", "CENSUS MISSING"))
    expect("MSF plant WWE_RDR_NO_PLANE_READER=1: made 0 of 0 seen, bound 0, read grows -> plane_made FAIL", base + maps(0, 0, 0), True, want_in=("plane_made", "MAP NOT MADE"))
    expect("MSF plant WWE_RDR_NO_PLANE_READER=1: ... and plane_bound is n/a (nothing made)", base + maps(0, 0, 0), True, want_in=("plane_bound", "n/a"))
    expect("MSF plant WWE_RDR_PLANE_NO_CHAIN=1: made 1000, bound 0 -> plane_bound FAIL", base + maps(1000, 1000, 0, chainf=1000), True, want_in=("plane_bound", "MAP NOT BOUND"))
    expect("MSF: made 1000, bound > 0 -> both rows ok, exit 0", base + maps(1000, 1000, 2500), False, want_in=("plane_bound", "ok"))
    expect("MSF: made 100 of 1000 read, none blank -> MAP NOT MADE (90% [L fitted])", base + maps(100, 1000, 300), True, want_in=("plane_made", "MAP NOT MADE"))
    expect("MSF: made 100 of 1000 read, 900 blank-plane frames explain the gap -> ok", base + maps(100, 1000, 300, blank=900), False, want_in=("plane_made", "ok"))
    expect("MSF: made 100 of 1000 read, 900 game no-draw frames explain the gap -> ok", base + maps(100, 1000, 300, nodraw=900), False, want_in=("plane_made", "ok"))
    expect("MSF: made but the plane_bound count is absent -> CENSUS MISSING", base + maps(1000, 1000, 1).split("[scene][draw]")[0], True, want_in=("plane_bound", "CENSUS MISSING"))
    expect("MSF: plane never read (a 1v1 with no module, no e2 census needed) -> no MSF fault", full.rstrip("\n") + " [plane read 0, no_module 0] [plane_eyes read 9, no_module 0]\n", False, want_in=("plane", "n/a"))
    expect("a clean full census (plane read 9, e2 made 9) -> exit 0", full.rstrip("\n") + plane_ok + "\n" + maps(9, 9, 20), False)
    expect("a bit missing from a full inputs line -> CENSUS MISSING", full.replace("camera 9/f9, ", "") + plane_ok, True, want_in=("camera", "CENSUS MISSING"))
    expect("plant: a required bit with stamp 0", line(light_states="0/f0"), True, want_in=("light_states", "DEAD FEED"))
    expect("plant: an (open) bit is read and judged", line().replace("light_states 9/f9", "light_states (open) 0/f0"), True, want_in=("light_states", "DEAD FEED"))
    expect("plant: a bit that dies part-way (last stamp f400 of 9000)", line(camera="400/f400", post_settings="9000/f9000"), True, want_in=("camera", "DIED PART-WAY"))
    # S103 TQF: CAW window judged to last_body_draw (CAWV Q3), never by script name
    cawlog = fx("caw_011.txt")
    check("TQF fixture caw_011 carries a camera stamp pattern", "caw enter" in cawlog)
    base_caw = P + "[scene][kind] caw enter #1 at frame 100 (sequence 76, drop mask 0x0)\n[scene][kind] caw leave #1 at frame 1000\n[rdr][caw] textures: parts 1, bound 1, from override 0, missing 0\n[scene][live] guest models calls 4, no descriptor 0\n"
    def cawline(lbd, **o):
        d = {b: ("800/f900" if b in DRAW_DRIVEN else "900/f1000") for b in BITS}
        d["sphere_occ"] = "0/f0"
        d.update(o)
        return "[scene][inputs] moved 18 of 18, " + " ".join("%s %s" % (b, d[b]) for b in BITS) + " [draw_camera source x: calls 5 accepted 5 rejected 0 used 5 last_body_draw %d] [blocks\n" % lbd
    expect("TQF: draw-driven bits stop at last_body_draw 900, window ends 1000 -> ok", base_caw + cawline(900), False, want_in=("light_states", "ok"))
    expect("TQF plant: a draw-driven bit dies BEFORE last_body_draw (light_states f400 of last draw 900) -> DIED PART-WAY", base_caw + cawline(900, light_states="300/f400"), True, want_in=("light_states", "DIED PART-WAY"))
    expect("TQF plant: no last_body_draw in the census -> the old rule (stops at 900 of 1000 = DIED PART-WAY)", base_caw + cawline(0), True, want_in=("camera", "DIED PART-WAY"))
    expect("TQF: model_runtime is not draw-driven, still judged to the window end", base_caw + cawline(900, model_runtime="800/f900"), True, want_in=("model_runtime", "DIED PART-WAY"))
    expect("plant: a bit that covers a sliver of the window", line(camera="2/f9000", post_settings="9000/f9000"), True, want_in=("camera", "DEAD FEED"))
    vd = dict((n, v) for n, v, _ in evaluate(line(arena_worlds="0/f0", ring_rows="0/f0", stage_clock="0/f0", arena_models="0/f0"))[0])
    check("ARENA_ONLY: arena bits at 0/f0 with arena_models never stamped -> all three n/a", vd["arena_worlds"] == vd["ring_rows"] == vd["stage_clock"] == "n/a")
    expect("ARENA_ONLY: arena bits at 0/f0 with arena_models stamping -> DEAD FEED", line(arena_worlds="0/f0"), True, want_in=("arena_worlds", "DEAD FEED"))
    check("n/a stays n/a: sphere_occ at 0/f0 is judged by no reader", dict((n, v) for n, v, _ in evaluate(line(sphere_occ="0/f0"))[0]).get("sphere_occ") == "n/a")

    # --- lints
    check("lint: BITS == kInputs and every census string is in src", not [p for p in lint() if "census string" in p or "BITS" in p])
    pl = lint(bits=BITS[:-1], keys=CENSUS_KEYS + ["[scene][lvf] a line the game never prints "])
    check("lint plant: a BITS list that differs from kInputs is refused", any("BITS !=" in p for p in pl))
    check("lint plant: a census string that is in no src literal (the LVF shape) is refused", any("lvf" in p for p in pl))
    check("lint: fixtures are counters only", not fixture_problems())
    check("lint plant: a { } block in a fixture is refused", bool(fixture_text_problems("x.txt", "# partial\n[scene][live] a {b: 1.5}\n")))
    check("lint plant: a non-census line in a fixture is refused", bool(fixture_text_problems("x.txt", "# partial\nmodel AAB142F0 vtx 5608\n")))
    check("cut strips { } blocks", strip_braces("a {x: 1.5, y: 2} b {z}") == "a  b ")
    print("selftest %s" % ("OK" if not bad else "FAILED (%d)" % bad))
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--lint" in argv:
        probs = lint()
        for p in probs:
            print("LINT FAIL: " + p)
        print("feed_census_check --lint: %s" % ("FAIL, %d problem(s)" % len(probs) if probs else "ok"))
        return 1 if probs else 0
    if "--cut" in argv:
        i = argv.index("--cut")
        cut(argv[i + 1], argv[i + 2], argv[i + 3] if len(argv) > i + 3 else "plane")
        return 0
    wf = AUDIT
    if "--waivers" in argv:
        i = argv.index("--waivers")
        wf = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    text = open(args[0], encoding="utf-8", errors="replace").read()
    probs = waiver_problems(wf)
    for p in probs:
        print("WAIVER LINT: " + p)
    fault = run_text(text, waivers(wf)) + len(probs)
    print("feed_census_check: %s" % ("FAIL, %d unwaived fault(s)" % fault if fault else "ok"))
    return 1 if fault else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
