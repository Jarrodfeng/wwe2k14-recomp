"""Reads a diagnostic run zip (Session 95; src/diag.h, src/diag_plan.h) and writes a page + a verdict.

    py -I tools\\native\\diag_read.py ZIP [--out DIR] [--ref VERSION] [--make-ref VERSION]

The zip comes from a player's machine: it is untrusted. Paths inside are checked (no absolute paths, no "..", only names
under known folders), sizes capped, and everything shown on the page is HTML-escaped. Nothing in the zip is extracted
(members are read into memory and shown). Writes out\\diag\\<name>\\index.html
and prints the verdict. Nothing is committed (the pictures are small game pictures: they stay in out\\, rule 1).

What it reports: the device card (from device.json), the arm table (ran / reached done / crashed at which stage), the scene
table (frame timing, stable pictures, hashes), the in-run A/Bs of the frozen scenes (same frame, one thing changed: a hash or
a perceptual hash that moved), the relaunch arms against arm 0, the counters the game printed per scene ([storage-window],
[s21][HOIST]), a ready `profiles_*.tsv` row for tools\\android\\emulate, and a likely cause. Every cause says CONFIRMED or
UNCONFIRMED (rule 4): a hash only says the picture changed, a person (or the shot-checker) says whether it changed for the
better.

From the GPU layer (SDK native/diag_layer.h): the per-pass sheets side by side across the frozen A/Bs and the arms (changed cells
outlined), the first pass where a wrestler's coverage vanishes, the vertex truth (Bytes, Coverage, Numbers, Solo: per sampled draw, its
worst rows), and the rule order of design section 9.

--make-ref VERSION keeps this run's hashes and stats (no pictures) in out\\diag\\ref\\VERSION\\ref.json; --ref VERSION compares a run with it:
the dHash + stats at a tolerance (exact hashes cannot match across GPUs), the guest input hashes exactly (only a replay arm can expect
them to match across devices: said so on the line).
"""
import argparse
import base64
import html
import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # (run with -I: this script's own folder, nothing else)
import diag_layer_read as layer  # noqa: E402

MAX_MEMBER = 40 << 20
MAX_TOTAL = 200 << 20
# (members are never extracted; names are checked by safe_name)


def safe_name(n):
    if n.startswith("/") or n.startswith("\\") or ".." in Path(n).parts or ":" in n:
        return False
    return True


def load(zpath):
    files = {}
    total = 0
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            if not safe_name(info.filename):
                print("skipped unsafe member name:", info.filename, file=sys.stderr)
                continue
            if info.file_size > MAX_MEMBER or total + info.file_size > MAX_TOTAL:
                print("skipped (too big):", info.filename, file=sys.stderr)
                continue
            total += info.file_size
            files[info.filename] = z.read(info)
    return files


def text(files, name):
    b = files.get(name)
    return b.decode("utf-8", "replace") if b is not None else ""


def jload(files, name):
    try:
        return json.loads(text(files, name))
    except Exception:
        return None


def jlines(files, name):
    out = []
    for line in text(files, name).splitlines():
        try:
            out.append(json.loads(line))
        except Exception:
            pass
    return out


def final_hash(r):
    """The picture a frozen scene finally stood at: its third picture (hash_c) when it took one, else the second (hash_b)."""
    c = r.get("hash_c")
    try:
        if c and int(str(c), 16):
            return c
    except ValueError:
        pass
    return r.get("hash_b")


def base_hashes(r):
    """Session 95: what a frozen variant may equal to be the same picture: the base before the observed frame (hash_b) or as it finally stood. With the warm flags the picture after the
    observed frame differs by a few hundred pixels and the next scene starts from hash_b again (A/A pass hashes 0 of 50 different): comparing with the final hash alone called every arm a change."""
    return (r.get("hash_b"), final_hash(r))


def hamming(a, b):
    try:
        return bin(int(a, 16) ^ int(b, 16)).count("1")
    except Exception:
        return None


def arm_ids(files):
    ids = set()
    for n in files:
        m = re.match(r"arm([A-Za-z0-9_]+)/", n)
        if m:
            ids.add(m.group(1))
    return sorted(ids, key=lambda x: (x != "0", x))


def profile_row(dev):
    """A profiles_*.tsv row for tools\\android\\emulate (same shape as profiles_a642.tsv)."""
    if not dev:
        return ""
    vid = "%x" % dev.get("vendorID", 0)
    dv = dev.get("driverVersion", 0)
    if dev.get("vendorID") == 0x5143:  # Qualcomm packs major.minor.patch as 10/10/12 bits... as the logs print it
        drv = "%d.%d.%d" % (dv >> 22, (dv >> 12) & 0x3FF, dv & 0xFFF)
    else:
        drv = "%d.%d.%d" % (dv >> 22, (dv >> 12) & 0x3FF, dv & 0xFFF)
    name = re.sub(r"\s+", "", dev.get("deviceName", "gpu"))
    kind = {1: "integrated", 2: "discrete"}.get(dev.get("deviceType"), "integrated")
    lim = dev.get("limits", {})
    keys = ["maxStorageBufferRange", "minStorageBufferOffsetAlignment", "maxFragmentInputComponents", "maxImageDimension2D",
            "maxImageDimension3D", "maxImageDimensionCube", "maxFramebufferWidth", "maxFramebufferHeight"]
    limits = ",".join("%s=%d" % (k, lim[k]) for k in keys if k in lim)
    fmts = dev.get("formats", {})
    flist = ",".join("%s:%s" % (k, v[1]) for k, v in sorted(fmts.items(), key=lambda kv: int(kv[0])) if int(k) in
                     (83, 97, 100, 103, 109, 124, 125, 126, 127, 129, 130))
    exts = {e[0] for e in dev.get("extensions", [])}
    want_off = [e for e in ("VK_EXT_non_seamless_cube_map", "VK_EXT_memory_budget", "VK_EXT_fragment_shader_interlock",
                            "VK_NV_device_diagnostic_checkpoints") if e not in exts]
    feats = dev.get("features", {})
    if "VK_EXT_non_seamless_cube_map" in want_off:
        want_off.append("nonSeamlessCubeMap")
    if "VK_EXT_fragment_shader_interlock" in want_off:
        want_off += ["fragmentShaderSampleInterlock", "fragmentShaderPixelInterlock"]
    for f in ("geometryShader", "shaderCullDistance", "vertexPipelineStoresAndAtomics", "textureCompressionBC"):
        if f in feats and not feats[f]:
            want_off.append(f)
    api_minor = (dev.get("apiVersion", 0) >> 12) & 0x3FF
    # a device without these features would refuse to start unless told (as profiles_mali_g57.tsv does)
    req = ""
    if "vertexPipelineStoresAndAtomics" in feats and not feats["vertexPipelineStoresAndAtomics"]:
        req += " --vulkan_require_vertex_pipeline_stores_and_atomics=false"
    if "fillModeNonSolid" in feats and not feats["fillModeNonSolid"]:
        req += " --vulkan_require_fill_mode_non_solid=false"
    if "fragmentStoresAndAtomics" in feats and not feats["fragmentStoresAndAtomics"]:
        req += " --vulkan_require_fragment_stores_and_atomics=false"
    if "geometryShader" in feats and not feats["geometryShader"]:
        req += " --vulkan_require_geometry_shader=false"
    return ("%s_%s\t--vulkan_emulate_gpu=%s:%s:%s:%s --vulkan_emulate_api_minor=%d%s --vulkan_emulate_limits=%s "
            "--vulkan_emulate_formats=%s --vulkan_emulate_disable=%s" %
            (name.lower(), drv.replace(".", "_"), vid, drv, name, kind, api_minor, req, limits, flist, ",".join(want_off)))


def device_card(dev):
    if not dev:
        return "<p><b>No device.json</b> (the game died before its device was made, or the SDK build predates the report).</p>"
    lim, feats = dev.get("limits", {}), dev.get("features", {})
    rows = [
        ("GPU", dev.get("deviceName")), ("Vendor / device id", "0x%x / 0x%x" % (dev.get("vendorID", 0), dev.get("deviceID", 0))),
        ("API", "1.%d.%d" % ((dev.get("apiVersion", 0) >> 12) & 0x3FF, dev.get("apiVersion", 0) & 0xFFF)),
        ("Driver", "%s %s (version %d)" % (dev.get("driver", {}).get("driverName", ""), dev.get("driver", {}).get("driverInfo", ""),
                                           dev.get("driverVersion", 0))),
        ("maxStorageBufferRange", lim.get("maxStorageBufferRange")),
        ("minStorageBufferOffsetAlignment", lim.get("minStorageBufferOffsetAlignment")),
        ("vertexPipelineStoresAndAtomics", feats.get("vertexPipelineStoresAndAtomics")),
        ("fragmentStoresAndAtomics", feats.get("fragmentStoresAndAtomics")),
        ("occlusionQueryPrecise", feats.get("occlusionQueryPrecise")),
        ("geometryShader", feats.get("geometryShader")), ("textureCompressionBC", feats.get("textureCompressionBC")),
        ("textureCompressionASTC_LDR", feats.get("textureCompressionASTC_LDR")),
        ("shaderFloat16 (1.2)", dev.get("features12", {}).get("shaderFloat16")),
        ("float controls: denorm preserve fp32 / flush fp32", "%s / %s" % (dev.get("floatControls", {}).get("shaderDenormPreserveFloat32"),
                                                                             dev.get("floatControls", {}).get("shaderDenormFlushToZeroFloat32"))),
        ("memory heaps (bytes, flags)", dev.get("memoryHeaps")), ("extensions", len(dev.get("extensions", []))),
        ("emulated (fake device)", dev.get("emulate")),
    ]
    return "<table>" + "".join("<tr><th>%s</th><td>%s</td></tr>" % (html.escape(str(k)), html.escape(str(v))) for k, v in rows) + "</table>"


VINPUT_RE = re.compile(r"\[vinput\] frames (\d+)-(\d+): attribute draws (\d+) \(ours (\d+), translated (\d+), ring (\d+)\), storage draws (\d+) \{([^}]*)\}"
                       r".*?callback calls (\d+).*?window changes (\d+)")


def vinput_table(files, ids):
    """Session 95: the storage draws of each arm by reason, from the [vinput] windows the game printed (the last one is the quit's tail
    window). Returns (html, notes). Reasons are the game's own names (vertex_input.cpp kRefusalNames); 'crowd' and 'reset-values' are the
    two the design leaves on storage; anything else, or a window with changes above 0 once the storage draws are only those, is noted."""
    rows, notes = [], []
    for a in ids:
        tot = {"windows": 0, "attr": 0, "ours": 0, "translated": 0, "ring": 0, "storage": 0, "calls": 0, "changes": 0}
        why = {}
        tail = None
        for n, b in files.items():
            if not (n.startswith("arm%s/" % a) and n.endswith(".log")):
                continue
            for line in b.decode("utf-8", "replace").splitlines():
                m = VINPUT_RE.search(line)
                if not m:
                    continue
                f0, f1, attr, ours, tr, ring, sto, reasons, calls, chg = m.groups()
                tot["windows"] += 1
                for k, v in (("attr", attr), ("ours", ours), ("translated", tr), ("ring", ring), ("storage", sto), ("calls", calls), ("changes", chg)):
                    tot[k] += int(v)
                for part in reasons.split(","):
                    mm = re.match(r"\s*([a-z0-9-]+) (\d+)\s*$", part)
                    if mm:
                        why[mm.group(1)] = why.get(mm.group(1), 0) + int(mm.group(2))
                tail = (int(f0), int(f1))
        if not tot["windows"]:
            continue
        typed = tot["attr"] + tot["storage"] - why.get("not-typed", 0)  # (storage includes the not-typed draws; they are no vertex read to remove)
        share = "%.2f%%" % (100.0 * tot["attr"] / typed) if typed else "n/a"
        reasons_txt = ", ".join("%s %d" % (k, v) for k, v in sorted(why.items(), key=lambda kv: -kv[1])) or "none"
        rows.append((a, tot["windows"], "%d (ours %d, translated %d, ring %d)" % (tot["attr"], tot["ours"], tot["translated"], tot["ring"]),
                     tot["storage"], share, reasons_txt, tot["calls"], tot["changes"], "%d-%d" % tail))
        other = {k: v for k, v in why.items() if k not in ("crowd", "reset-values")}
        if other:
            notes.append("arm %s: storage draws for reasons other than crowd / reset-values: %s" % (a, ", ".join("%s %d" % kv for kv in other.items())))
        if tot["changes"] > 0:
            notes.append("arm %s: %d storage window changes over %d [vinput] windows (target near 0)" % (a, tot["changes"], tot["windows"]))
    if not rows:
        return "", notes
    h = ("<h2>Storage vertex draws by reason ([vinput] windows)</h2><table><tr><th>arm</th><th>windows</th><th>attribute draws</th><th>storage draws</th>"
         "<th>attribute share of typed</th><th>storage by reason</th><th>callback calls</th><th>window changes</th><th>last window (frames)</th></tr>" +
         "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % html.escape(str(c)) for c in r) for r in rows) + "</table>")
    return h, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--out", default="")
    ap.add_argument("--ref", default="", help="compare with out\\diag\\ref\\VERSION\\ref.json")
    ap.add_argument("--make-ref", default="", help="keep this run's hashes and stats as out\\diag\\ref\\VERSION\\ref.json")
    args = ap.parse_args()
    zpath = Path(args.zip)
    files = load(zpath)
    out = Path(args.out) if args.out else Path("out") / "diag" / zpath.stem
    out.mkdir(parents=True, exist_ok=True)

    ids = arm_ids(files)
    verdict, notes = [], []
    arm_rows, scene_rows, ab_rows = [], [], []
    scenes = {}  # arm -> {scene: record}
    dev0 = None
    for a in ids:
        d = jload(files, "arm%s/device.json" % a)
        if a == "0":
            dev0 = d
        exit_txt = text(files, "arm%s/exit.txt" % a).strip().replace("\n", "; ")
        hang = text(files, "arm%s/hang.txt" % a).strip().replace("\n", "; ")
        stages = jlines(files, "arm%s/stages.log" % a) if False else [l for l in text(files, "arm%s/stages.log" % a).splitlines()]
        last_stage = "-"
        for l in stages:
            try:
                last_stage = json.loads(l).get("stage", last_stage)
            except Exception:
                pass
        reached = last_stage == "done"
        recs = jlines(files, "arm%s/scenes.jsonl" % a)
        scenes[a] = {r["scene"]: r for r in recs if "scene" in r}
        arm_rows.append((a, "yes" if d else "NO device.json", last_stage, "reached done" if reached else "DID NOT reach done",
                         hang or "-", exit_txt or "-", len(recs)))
        if not reached:
            verdict.append(("arm %s stopped at stage '%s'%s" % (a, last_stage, (" (watchdog: %s)" % hang) if hang else ""),
                            "CONFIRMED (stages.log / hang.txt)"))
    for a in ids:
        for name, r in scenes[a].items():
            t = r.get("timing300", {})
            scene_rows.append((a, name, r.get("frame"), "yes" if r.get("stable") else "NO (%s tries)" % r.get("attempts"),
                               r.get("hash_b"), r.get("dhash"), t.get("fps", "-"), t.get("low1_fps", "-"), t.get("p99_ms", "-"),
                               t.get("max_ms", "-")))
    # in-run A/Bs (arm 0): each frozen variant against the frozen base of the same scene
    s0 = scenes.get("0", {})
    for base in ("entrance", "match"):
        b = s0.get(base)
        aa = s0.get(base + "_aa")
        if not b:
            continue
        # (against the base as it finally stood: a small change between its second and third picture is named by after_px, and is no A/B's doing)
        if aa and (aa.get("hash_b") not in base_hashes(b) or not b.get("stable")):
            verdict.append(("%s: the frozen frame is not stable (A/A control differs from the base or the base never settled): "
                            "the frozen A/B results for this scene are unreliable" % base, "CONFIRMED (hashes)"))
        for tag, meaning in (("ablate", "every replaced pixel shader drawn flat grey"), ("vsoff", "our vertex shaders off (translated ones)"),
                             ("psoff", "our pixel shaders off (translated ones)")):
            v = s0.get("%s_%s" % (base, tag))
            if not v:
                ab_rows.append((base, tag, meaning, "not run", "", ""))
                continue
            if not v.get("stable") or v.get("pending_pipelines", 0) > 0:  # review F17: an unsettled variant says nothing yet
                ab_rows.append((base, tag, meaning, "NOT SETTLED (stable=%s, pipelines pending=%s): no verdict" %
                                (v.get("stable"), v.get("pending_pipelines", "?")), "", v.get("hash_b")))
                continue
            same = v.get("hash_b") in base_hashes(b)
            hd = hamming(v.get("dhash"), b.get("dhash"))
            ab_rows.append((base, tag, meaning, "IDENTICAL picture" if same else "picture CHANGED", str(hd) if hd is not None else "-",
                            v.get("hash_b")))
            if tag == "ablate" and same:
                verdict.append(("%s: flat-grey ablation changed nothing: either no replaced shader draws anything visible here, or "
                                "the switch did not take effect under this renderer" % base, "UNCONFIRMED (the ablate switch under TRUE NATIVE was never run)"))
            if tag in ("vsoff", "psoff") and not same:
                verdict.append(("%s: switching %s changes the picture (%s dHash bits differ). Look at the pair: if the change fixes "
                                "the bug, the bug is in our %s shaders" % (base, meaning, hd, "vertex" if tag == "vsoff" else "pixel"),
                                "CONFIRMED that it changes; WHICH is right needs eyes"))
    # relaunch arms against arm 0's entrance
    e0 = s0.get("entrance")
    for a in ids:
        if a == "0":
            continue
        e = scenes[a].get("entrance")
        if not e:
            verdict.append(("relaunch arm %s produced no entrance scene" % a, "CONFIRMED"))
            continue
        hd = hamming(e.get("dhash"), e0.get("dhash")) if e0 else None
        ab_rows.append(("entrance", "arm " + a, "relaunch with one setting changed", "dHash distance to arm 0: %s" % hd,
                        "" if hd is None else ("similar" if hd <= 6 else "DIFFERENT"), e.get("hash_b")))
        if hd is not None and hd > 6:
            verdict.append(("arm %s's entrance differs from arm 0's (dHash distance %d): the setting it flips (see its command.txt) "
                            "may change what is drawn. A relaunch is not the same frame (camera variants, clock-driven trons), so look at "
                            "both pictures; if arm 0 shows the bug and this arm does not, that setting is implicated" % (a, hd),
                            "UNCONFIRMED (no A/A relaunch distance has been measured; the seed is fixed but the arms are not frame-exact)"))
    # device rules
    if dev0:
        lim, feats = dev0.get("limits", {}), dev0.get("features", {})
        iv = dev0.get("instanceApiVersion", 0)
        if iv and ((iv >> 22) < 1 or ((iv >> 12) & 0x3FF) < 2):  # review F18: features12 may read as false zeros
            notes.append("The game's Vulkan instance is below 1.2: the 1.2 / 1.3 feature flags in device.json may read as false "
                         "(a driver reporting MISSING under a lower instance version).")
        if "dense" in ids or "sparse" in ids:
            sp = feats.get("sparseResidencyBuffer")
            notes.append("sparseResidencyBuffer is %s on arm 0's device: %s" % (
                "not in device.json" if sp is None else ("true" if sp else "false"),
                "the dense arm (sparse off) and the sparse arm (sparse on) are real changes only when it is true, and only one of them differs from arm 0: arm 0's report line [sparse] shared memory says which (a Turnip driver is plain by rule, so dense repeats arm 0 there); with false both repeat arm 0" if sp is not False else
                "sparse binding was never in use, so the dense and sparse arms repeat arm 0 and say nothing about sparse"))
        if not feats.get("vertexPipelineStoresAndAtomics"):
            notes.append("No vertexPipelineStoresAndAtomics: vertex memory export is left out on this GPU (Mali-style).")
        if lim.get("maxStorageBufferRange", 1 << 31) < (512 << 20):
            notes.append("maxStorageBufferRange %d MB < 512 MB: the shared memory needs the storage windows (or the old split)."
                         % (lim.get("maxStorageBufferRange", 0) >> 20))
    # counters the game printed per scene
    counters = []
    for a in ids:
        for n, b in files.items():
            if n.startswith("arm%s/" % a) and n.endswith(".log"):
                for line in b.decode("utf-8", "replace").splitlines():
                    if "[diag-stats" in line and ("storage-window" in line or "HOIST" in line):
                        counters.append((a, line[:300]))
                    m = re.search(r"over the window\D{0,12}(\d+)", line)
                    if m and "[storage-window]" in line and int(m.group(1)) > 0:  # review F15: "over the window 0" is fine
                        notes.append("arm %s: a draw went over its storage window: %s" % (a, line[:200]))
    vi_html, vi_notes = vinput_table(files, ids)
    notes += vi_notes
    # the GPU layer: sheets, vanish, vertex truth, the arms' differences, the rule order
    gpu = layer.read(files, ids, scenes)
    verdict += gpu["verdict"]
    notes += gpu["notes"]
    ref_lines = []
    if args.make_ref:
        layer.make_ref(gpu, Path("out") / "diag" / "ref" / re.sub(r"[^A-Za-z0-9._-]", "_", args.make_ref))
    if args.ref:
        ref_lines = layer.compare_ref(gpu, Path("out") / "diag" / "ref" / re.sub(r"[^A-Za-z0-9._-]", "_", args.ref))
        verdict += [v for v in ref_lines if v[1].startswith(("CONFIRMED", "UNCONFIRMED"))]
    if not verdict:
        verdict.append(("nothing in the automatic rules points at a cause; open the sheets and compare the pictures", "UNCONFIRMED"))

    # page
    esc = html.escape
    h = ["<!doctype html><meta charset=utf-8><title>Diagnostic run: %s</title>" % esc(zpath.name),
         "<style>body{font:14px sans-serif;margin:20px;max-width:1300px}table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid #888;"
         "padding:3px 8px;text-align:left;vertical-align:top}th{background:#eee}img{max-width:420px;border:1px solid #444;margin:2px}"
         ".f{display:inline-block;margin:4px;font-size:12px}code{background:#eee;padding:1px 4px}</style>",
         "<h1>Diagnostic run: %s</h1>" % esc(zpath.name)]
    h.append("<h2>Verdict (each line says CONFIRMED or UNCONFIRMED)</h2><ul>" + "".join("<li>%s <i>[%s]</i></li>" % (esc(t), esc(c)) for t, c in verdict) + "</ul>")
    if notes:
        h.append("<h3>Notes</h3><ul>" + "".join("<li>%s</li>" % esc(n) for n in notes) + "</ul>")
    h.append("<h2>Device (arm 0)</h2>" + device_card(dev0))
    h.append("<h3>profiles_*.tsv row for tools\\android\\emulate</h3><pre>%s</pre>" % esc(profile_row(dev0)))
    for n in ("system.txt", "system_end.txt", "arm_summary.txt", "base_flags.txt"):
        if n in files:
            h.append("<h3>%s</h3><pre>%s</pre>" % (esc(n), esc(text(files, n)[:20000])))
    h.append("<h2>Arms</h2><table><tr><th>arm</th><th>device.json</th><th>last stage</th><th>route</th><th>watchdog</th><th>exit</th><th>scenes</th></tr>" +
             "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % esc(str(c)) for c in r) for r in arm_rows) + "</table>")
    h.append("<h2>Scenes</h2><table><tr><th>arm</th><th>scene</th><th>frame</th><th>stable</th><th>hash</th><th>dHash</th><th>fps</th>"
             "<th>1% low fps</th><th>p99 ms</th><th>max ms</th></tr>" +
             "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % esc(str(c)) for c in r) for r in scene_rows) + "</table>")
    h.append("<h2>A/Bs</h2><table><tr><th>scene</th><th>change</th><th>meaning</th><th>result</th><th>dHash bits</th><th>hash</th></tr>" +
             "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % esc(str(c)) for c in r) for r in ab_rows) + "</table>")
    h.append(layer.page(gpu, files, ids))
    h.append(vi_html)
    h.append("<h2>Counters the game printed at each scene</h2><pre>%s</pre>" % esc("\n".join("arm %s: %s" % c for c in counters[:60]) or "(none: no [diag-stats] lines in the logs)"))
    h.append("<h2>Pictures</h2>")
    for a in ids:
        pics = sorted(n for n in files if n.startswith("arm%s/shots/" % a) and n.endswith(".jpg"))
        if not pics:
            continue
        h.append("<h3>arm %s</h3>" % esc(a))
        for n in pics:
            data = base64.b64encode(files[n]).decode()
            h.append('<span class="f"><img src="data:image/jpeg;base64,%s"><br>%s</span>' % (data, esc(Path(n).stem)))
    (out / "index.html").write_text("\n".join(h), encoding="utf-8")
    (out / "profile_row.tsv").write_text(profile_row(dev0) + "\n", encoding="utf-8")
    print("diag_read: %s" % zpath.name)
    for t, c in verdict:
        print(" - %s  [%s]" % (t, c))
    print("page:", out / "index.html")


if __name__ == "__main__":
    main()
