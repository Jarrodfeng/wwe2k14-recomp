"""The GPU layer's part of the diagnostic run reader (Session 95; SDK native/diag_layer.h, used by tools\\native\\diag_read.py).

    sheet_<scene>.json / .jpg   per pass: target, draws by family, GPU ms, stats, hashes; a 160 x 90 cell in the picture
    truth_<scene>.json          the sampled draws: Bytes, Coverage, the CPU's view; family_coverage = every wrestler draw per pass
    vertex_<scene>.json         the sampled draws: Numbers (the real vertex stage against the interpreter), Solo (solo_<scene>.jpg)

Everything comes from a player's machine: it is untrusted. Fields are read with .get, numbers through num(), and everything shown is escaped
by the page code here. Nothing is written but the page's text and, with --make-ref, hashes and stats under out\\diag\\ref.
"""
import base64
import html
import json
import math
import re
from pathlib import Path

TOLERANCE_ERR = 1e-3      # Numbers: a position error beyond this (clip units, relative to w) is a difference, not rounding (the table's "worst" list)
# Numbers: the error is max over x,y,z,w of |gpu - cpu| / max(1, |w|), i.e. relative to w, so it is the NDC error of the vertex
# (x/w, y/w): 0.01 is 1% of the half screen, ~3.6 px at 720p. Session 95: the first PC runs read up to 0.0152 on skinned draws (32 of 1114
# vertices ~30 units off in y). That was the interpreter's bug, not float order: it read all four bone bytes as the first (Session 95 run fix1). With it
# fixed a healthy 4090 reads 1e-7 on every wrestler draw and up to 6e-4 on a vertex a hair in front of the camera plane (x 1e4, w 1.7: float eps of x).
# A real fault is another scale: a wrong upload or window gives garbage vertices (errors of order 1 and up, 1e30 when one side is not finite),
# a stale buffer a whole mesh off. So: a verdict at VERTEX_WRONG_ERR (0.05, back from 0.02 per review: the error divides by max(1,|w|) so a vertex near the camera plane moves by 7.8e-3 per float step at x 1e5, and the phone's rounding is not measured; near-plane
# vertices), and a note (not a verdict) between VERTEX_NOTE_ERR and that. A smaller offset on MANY vertices (a whole mesh shifted, e.g. last frame's bone
# constants) is also a verdict: more than VERTEX_OUTLIER_FRAC of the compared vertices beyond VERTEX_NOTE_ERR, or a median above VERTEX_MEDIAN_ERR.
VERTEX_NOTE_ERR = 2e-3
VERTEX_WRONG_ERR = 5e-2
VERTEX_OUTLIER_FRAC = 0.05
VERTEX_MEDIAN_ERR = 1e-3
TOLERANCE_DHASH = 10      # reference: dHash bits a pass may differ by across GPUs
TOLERANCE_MEAN = 24.0     # reference: the mean colour's distance
SPIKE_MIN = 1
VARIANTS = (("", "base"), ("_aa", "A/A"), ("_ablate", "flat grey"), ("_vsoff", "our VS off"), ("_psoff", "our PS off"))
AFTER_SMALL = 0.001  # the picture after the observed frame: under this share of its pixels it is a note, not a finding
BASES = ("entrance", "match")


def num(v, default=0.0):
    if isinstance(v, bool):
        return float(v)
    if isinstance(v, (int, float)):
        f = float(v)
        return f if math.isfinite(f) else default  # (json.loads accepts NaN and Infinity: a hostile zip must not reach int())
    return default


def jget(files, name):
    b = files.get(name)
    if b is None:
        return None
    try:
        v = json.loads(b.decode("utf-8", "replace"), parse_constant=lambda c: None)
    except Exception:
        return None
    return v if isinstance(v, dict) else None


def lst(v):
    return v if isinstance(v, list) else []


def dct(v):
    return v if isinstance(v, dict) else {}


def hamming(a, b):
    try:
        return bin(int(str(a), 16) ^ int(str(b), 16)).count("1")
    except Exception:
        return None


def by_prefix(files, arm, prefix, ext):
    out = {}
    pat = re.compile(r"arm%s/%s([A-Za-z0-9_]+)\.%s$" % (re.escape(arm), prefix, ext))
    for n in files:
        m = pat.match(n)
        if m:
            out[m.group(1)] = n
    return out


def cells_of(sheet):
    return [c for c in lst(dct(sheet).get("cells")) if isinstance(c, dict)]


def wrestler_draws(cell):
    return int(num(dct(cell.get("family")).get("wrestler")))


def find_vanish(sheet, truth):
    """The first pass where a wrestler's coverage vanishes. Three kinds of evidence, strongest first:
    the occlusion counts of every wrestler draw of the pass; the pass's target unchanged by wrestler draws; a target gone black."""
    out = []
    cov = {}
    for row in lst(dct(truth).get("family_coverage")):
        row = dct(row)
        if row.get("family") == "wrestler":
            cov[int(num(row.get("pass"), -1))] = (int(num(row.get("draws"))), int(num(row.get("samples"))))
    seen_samples = any(s > 0 for _, s in cov.values())
    on_screen_passes = set()
    for d in lst(dct(truth).get("draws")):
        d = dct(d)
        if d.get("family") == "wrestler" and dct(d.get("cpu")).get("on_screen") is True:
            on_screen_passes.add(int(num(d.get("pass"), -1)))
    for p in sorted(cov):
        draws, samples = cov[p]
        if draws and samples == 0 and (seen_samples or p in on_screen_passes):
            out.append({"pass": p, "kind": "coverage", "text": "pass %d: %d wrestler draw(s) passed no samples%s" %
                        (p, draws, " while the CPU puts them on screen" if p in on_screen_passes else " while other passes' wrestlers did")})
            break
    last_on_target = {}
    black_on_target = {}
    for c in cells_of(sheet):
        t = str(c.get("target"))
        p = int(num(c.get("pass"), -1))
        if c.get("xxh") is None:
            continue
        prev = last_on_target.get(t)
        if prev is not None and wrestler_draws(c) and c.get("xxh") == prev.get("xxh"):
            out.append({"pass": p, "kind": "unchanged", "text": "pass %d: %d wrestler draw(s) left target %s exactly as it was" %
                        (p, wrestler_draws(c), t)})
        black = num(c.get("black"))
        was_black = black_on_target.get(t)
        if was_black is not None and was_black < 0.9 and black >= 0.98:
            out.append({"pass": p, "kind": "black", "text": "pass %d: target %s went black (%.0f%% black, %.0f%% in its pass before)" %
                        (p, t, black * 100, was_black * 100)})
        last_on_target[t] = c
        black_on_target[t] = black
    return out


def align(a, b):
    """The cells of b matched to the cells of a (a pass added or dropped in one frame must not shift every later pass; LOW phones skip shadow passes
    every other frame). First by what the pass is (kind, size, format, both shader modules, n-th such pass), the rest by (kind, size, format) in order,
    since an A/B changes the shaders on purpose. Returns ({a's pass: (a's cell, b's cell or None)}, b's cells left over)."""
    def fine(c):
        return tuple(str(c.get(k)) for k in ("kind", "w", "h", "fmt", "vk_vs", "vk_fs"))

    def coarse(c):
        return tuple(str(c.get(k)) for k in ("kind", "w", "h", "fmt"))

    left_b = cells_of(b)
    index = {}
    seen = {}
    for i, c in enumerate(left_b):
        k = fine(c)
        index.setdefault((k, seen.get(k, 0)), i)
        seen[k] = seen.get(k, 0) + 1
    taken = set()
    out = {}
    seen = {}
    for c in cells_of(a):
        k = fine(c)
        n = seen.get(k, 0)
        seen[k] = n + 1
        i = index.get((k, n))
        if i is not None:
            taken.add(i)
        out[int(num(c.get("pass"), -1))] = [c, left_b[i] if i is not None else None]
    rest = {}
    for i, c in enumerate(left_b):
        if i not in taken:
            rest.setdefault(coarse(c), []).append(i)
    for pair in out.values():
        if pair[1] is None:
            q = rest.get(coarse(pair[0]))
            if q:
                i = q.pop(0)
                taken.add(i)
                pair[1] = left_b[i]
    return {k: (v[0], v[1]) for k, v in out.items()}, [c for i, c in enumerate(left_b) if i not in taken]


def diff_sheets(a, b):
    """The passes of a against their matches in b: the cells whose exact hash differs, and the first."""
    pairs, extra = align(a, b)
    both = {p: pr for p, pr in pairs.items() if pr[1] is not None and pr[0].get("xxh") and pr[1].get("xxh")}
    changed = sorted(p for p, pr in both.items() if pr[0].get("xxh") != pr[1].get("xxh"))
    return {"common": len(both), "changed": changed, "first": changed[0] if changed else None,
            "only_a": sum(1 for pr in pairs.values() if pr[1] is None), "only_b": len(extra)}


def row_key(d):
    return tuple(str(d.get(k)) for k in ("vs", "ps", "vertex_count", "nth"))


def merged_rows(truth, vertex):
    rows = {}
    for d in lst(dct(truth).get("draws")):
        d = dct(d)
        rows[row_key(d)] = dict(d)
    for d in lst(dct(vertex).get("draws")):
        d = dct(d)
        r = rows.setdefault(row_key(d), dict(d))
        for k in ("numbers", "solo"):
            if k in d:
                r[k] = d[k]
    return list(rows.values())


def judge_row(d):
    """Design section 9's order for one sampled draw: (label, line, confirmed-or-not)."""
    who = "%s draw (vs %s, %s vertices, pass %s)" % (d.get("family"), str(d.get("vs"))[:8], d.get("vertex_count"), d.get("pass"))
    bytes_ = dct(d.get("bytes"))
    if num(bytes_.get("diff_ranges")) > 0:
        return ("upload", "%s: the GPU's shared memory differs from the guest bytes in %d of %d fetch range(s) (first at byte %s, storage window %s): "
                "a stale or partial upload" % (who, num(bytes_.get("diff_ranges")), num(bytes_.get("ranges")), bytes_.get("first_diff"), bytes_.get("window")),
                "CONFIRMED that the bytes differ; the pass end is when they were read")
    n = dct(d.get("numbers"))
    if n and "skipped" not in n:
        compared = num(n.get("compared"))
        # With a CPU truth only a disagreement counts (a vertex just in front of the camera plane is huge on both sides; a non-finite one the CPU also gets is the shader's own).
        bad = num(n.get("gpu_nonfinite_cpu_finite")) if compared else num(n.get("gpu_nonfinite"))
        err = num(n.get("max_err"))
        out = num(n.get("outliers"))
        spikes = 0 if compared else num(n.get("spikes"))
        shifted = bool(compared) and (out > VERTEX_OUTLIER_FRAC * compared or num(n.get("median_err")) > VERTEX_MEDIAN_ERR)
        if bad or err > VERTEX_WRONG_ERR or shifted or spikes >= SPIKE_MIN:
            detail = "%d non-finite positions, %d of %d compared beyond %g, max error %.4g, %d spikes" % (bad, out, compared, VERTEX_NOTE_ERR, err, spikes)
            return ("vertex", "%s: the real vertex stage's output is wrong: %s. Then the arms decide: the storage flip fixing it means windows, our VS off "
                    "fixing it means our shader, the system driver fixing it means the driver" % (who, detail),
                    "CONFIRMED (bit-exact capture against the interpreter)" if compared else
                    "UNCONFIRMED (no CPU truth for this draw: a spike or a NaN alone can be real, e.g. a vertex just in front of the camera plane)")
    cov = dct(d.get("coverage"))
    cpu = dct(d.get("cpu"))
    if cov.get("ran") is True and num(cov.get("samples")) == 0 and cpu.get("on_screen") is True:
        checked = bool(n) and "skipped" not in n
        if checked and num(n.get("max_err")) <= VERTEX_NOTE_ERR:
            what = "its positions are right"
        elif checked:
            what = "its positions differ slightly from the interpreter (max error %.4g of w, see the notes)" % num(n.get("max_err"))
        else:
            what = "its positions were not checked"
        return ("state", "%s: %s, no samples passed, the CPU puts it on screen (%.0f px): binning, depth or state" %
                (who, what, num(cpu.get("area_px"))),
                "UNCONFIRMED (a draw behind others also passes no samples; the binning pass is not what Numbers runs)")
    return None


def read(files, ids, scenes):
    """All of the layer's files of the zip, and the verdict lines they support."""
    gpu = {"sheets": {}, "truth": {}, "vertex": {}, "solo": {}, "vanish": [], "arm_diffs": [], "rows": [], "aa": [], "verdict": [], "notes": []}
    verdict, notes = gpu["verdict"], gpu["notes"]
    for a in ids:
        for scene, name in by_prefix(files, a, "sheet_", "json").items():
            v = jget(files, name)
            if v:
                gpu["sheets"][(a, scene)] = v
        for scene, name in by_prefix(files, a, "truth_", "json").items():
            v = jget(files, name)
            if v:
                gpu["truth"][(a, scene)] = v
        for scene, name in by_prefix(files, a, "vertex_", "json").items():
            v = jget(files, name)
            if v:
                gpu["vertex"][(a, scene)] = v
    if not gpu["sheets"]:
        notes.append("No per-pass sheets in this zip (no frozen scene was reached, or the GPU layer was off).")
    for (a, scene), sheet in sorted(gpu["sheets"].items()):
        if num(sheet.get("overflow")) > 0:
            notes.append("arm %s %s: %d pass(es) past the sheet's %d cells have no picture (the last ones, the final composite among them, are in the table only)." %
                         (a, scene, num(sheet.get("overflow")), int(num(sheet.get("cols"), 16)) * 8))
    # the picture after the observed frame (a LATER unarmed frame: it catches lasting damage only, not a change confined to the observed frame)
    for a in ids:
        after_diff = []  # one line per arm (a run that perturbs gave the same line for every scene: 14 lines for one fact)
        for name, r in dct(scenes.get(a)).items():
            if dct(r).get("after_same") is False:
                px, total = num(dct(r).get("after_px"), -1), num(dct(r).get("after_total"))
                size = "" if px < 0 or not total else " (%d of %d pixels, up to %d levels)" % (px, total, num(dct(r).get("after_max")))
                if name == "entrance" and px >= 0 and total and px / total < AFTER_SMALL:
                    # PC runs r2 / r3: the entrance base only (every other scene stays a verdict): one crowd figure comes in at the picture's edge, at or before the observed frame (observing as the cause is not excluded: the first capture of the process, or a timer after the freeze, fits too)
                    # (its sheet equals the A/A's in every pass). Why it comes in is unconfirmed; r2 also had entrance_psoff and match_psoff change (no size: r2 predates after_px).
                    notes.append("arm %s scene %s: a small part of the picture changed between the second picture and the one after the observed frame%s: "
                                 "under %.1f%% of it, so listed rather than judged; the cause is unconfirmed (on the PC: one crowd figure at the picture's edge, "
                                 "in the entrance's first scene)." % (a, name, size, AFTER_SMALL * 100))
                else:
                    after_diff.append(str(name) + size)
            if dct(r).get("sheet") is False and name.split("_")[-1] != "live" and name not in ("menu", "pause"):
                notes.append("arm %s scene %s: no sheet (the capture did not finish)." % (a, name))
        if after_diff:
            verdict.append(("arm %s, %d scene(s) (%s): the picture after the observed frame differs from the one before it: observing left a lasting change, "
                            "its sheet may not be the frame the player sees" % (a, len(after_diff), ", ".join(after_diff)), "CONFIRMED (hashes); the cause is unconfirmed"))
    # A/A per pass, then each frozen A/B against the base, then the relaunch arms against arm 0
    for base in BASES:
        b = gpu["sheets"].get(("0", base))
        if not b:
            continue
        aa = gpu["sheets"].get(("0", base + "_aa"))
        if aa:
            d = diff_sheets(b, aa)
            gpu["aa"].append((base, d))
            if d["changed"]:
                verdict.append(("%s: the A/A sheet differs from the base in %d of %d passes (first: pass %s): this frame is not stable pass by pass, so "
                                "per-pass differences of the A/Bs are unreliable" % (base, len(d["changed"]), d["common"], d["first"]), "CONFIRMED (cell hashes)"))
            elif d["common"]:
                notes.append("%s: the A/A sheet is identical to the base in all %d passes: this frame stands still, so a per-pass difference in an A/B is the A/B's own." % (base, d["common"]))
        for tag, title in VARIANTS[2:]:
            v = gpu["sheets"].get(("0", base + tag))
            if v:
                d = diff_sheets(b, v)
                gpu["arm_diffs"].append((base, title, d))
    for a in ids:
        if a == "0":
            continue
        v, b = gpu["sheets"].get((a, "entrance")), gpu["sheets"].get(("0", "entrance"))
        if v and b:
            d = diff_sheets(b, v)
            d["passes_a"], d["passes_b"] = len(cells_of(b)), len(cells_of(v))
            gpu["arm_diffs"].append(("entrance", "arm " + a, d))
    # where a wrestler vanishes
    for (a, scene), sheet in sorted(gpu["sheets"].items()):
        truth = gpu["truth"].get((a, scene))
        for v in find_vanish(sheet, truth):
            v2 = dict(v, arm=a, scene=scene)
            gpu["vanish"].append(v2)
    first_seen = set()
    for v in gpu["vanish"]:
        key = (v["arm"], v["scene"], v["kind"])
        if key in first_seen or v["scene"].endswith(("_ablate", "_vsoff", "_psoff")):
            continue
        first_seen.add(key)
        verdict.append(("arm %s %s: %s" % (v["arm"], v["scene"], v["text"]),
                        "CONFIRMED that no samples passed (occlusion counts); why is not" if v["kind"] == "coverage" else "UNCONFIRMED (a hash or a black share; look at the cell)"))
    # the sampled draws, in design section 9's order
    for key in sorted(set(gpu["truth"]) | set(gpu["vertex"])):
        rows = merged_rows(gpu["truth"].get(key), gpu["vertex"].get(key))
        for d in rows:
            j = judge_row(d)
            gpu["rows"].append((key[0], key[1], d, j[0] if j else ""))
            if j:
                verdict.append(("arm %s %s: %s" % (key[0], key[1], j[1]), j[2]))
            if not j or j[0] not in ("vertex", "upload"):
                n = dct(d.get("numbers"))
                if n and "skipped" not in n and num(n.get("max_err")) > VERTEX_NOTE_ERR:
                    notes.append("arm %s %s: vs %s: %d of %d compared vertices differ from the interpreter by up to %.4g of w (median %.2g), below the %g "
                                 "that would be a fault; cause unconfirmed (near-plane vertices round this way; not a verdict)" %
                                 (key[0], key[1], str(d.get("vs"))[:8], num(n.get("outliers")), num(n.get("compared")), num(n.get("max_err")),
                                  num(n.get("median_err")), VERTEX_WRONG_ERR))
    # flat grey: geometry against the pixel shader
    wrestlers_fine = [r for r in gpu["rows"] if dct(r[2]).get("family") == "wrestler" and num(dct(dct(r[2]).get("coverage")).get("samples")) > 0]
    for base, title, d in gpu["arm_diffs"]:
        if title == "flat grey" and d["changed"] and wrestlers_fine:
            verdict.append(("%s: wrestler draws pass samples and flat grey changes the passes from %s on: the geometry is there. If the wrestler looks wrong, it is "
                            "the pixel shader; the mediump arm decides between precision and logic" % (base, d["first"]), "UNCONFIRMED (a person judges the pictures)"))
    for key, v in sorted(gpu["truth"].items()):
        if v.get("picks") and not v.get("picks_found"):
            notes.append("arm %s %s: none of the %s picked draws came back on the truth frame (the frame changed between them)." % (key[0], key[1], v.get("picks")))
    return gpu


def strip_css(gpu, files):
    """Each sheet's picture once, as a CSS class; the cells are windows on it."""
    css, classes = [], {}
    n = 0
    for (a, scene) in sorted(gpu["sheets"]):
        pic = files.get("arm%s/sheet_%s.jpg" % (a, scene))
        if not pic:
            continue
        cls = "s%d" % n
        n += 1
        classes[(a, scene)] = cls
        css.append(".%s{background-image:url(data:image/jpeg;base64,%s)}" % (cls, base64.b64encode(pic).decode()))
    return "".join(css), classes


def cell_html(sheet, cls, c, changed):
    if not c:
        return "<td class=e>-</td>"
    title = html.escape("pass %s %s target %s %sx%s fmt %s draws %s black %.0f%% %s ms" % (
        c.get("pass"), c.get("kind"), c.get("target"), c.get("w"), c.get("h"), c.get("fmt"), c.get("draws"), num(c.get("black")) * 100, c.get("gpu_ms")), quote=True)
    cell = int(num(c.get("cell"), -1))
    if cell < 0 or not cls:
        return '<td class=e title="%s">%s</td>' % (title, html.escape(str(c.get("kind"))))
    cols = max(1, int(num(sheet.get("cols"), 16)))
    x, y = (cell % cols) * 160, (cell // cols) * 90
    return '<td><div class="c %s%s" style="background-position:-%dpx -%dpx" title="%s"></div></td>' % (cls, " d" if changed else "", x, y, title)


def page(gpu, files, ids):
    esc = html.escape
    h = ["<h2>GPU layer</h2>"]
    css, classes = strip_css(gpu, files)
    h.append("<style>.c{width:160px;height:90px;display:block}.d{outline:3px solid #e00;outline-offset:-3px}.e{width:160px;color:#888;font-size:11px}"
             "details{margin:4px 0}" + css + "</style>")
    if gpu["vanish"]:
        h.append("<h3>Where a wrestler vanishes</h3><ul>" + "".join("<li>arm %s %s: %s <i>[%s]</i></li>" % (esc(v["arm"]), esc(v["scene"]), esc(v["text"]), esc(v["kind"])) for v in gpu["vanish"][:40]) + "</ul>")
    if gpu["arm_diffs"]:
        h.append("<h3>What each change did, pass by pass</h3><table><tr><th>scene</th><th>against the base</th><th>passes compared</th><th>changed</th><th>first changed pass</th></tr>")
        for base, title, d in gpu["arm_diffs"]:
            h.append("<tr><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td>%s</td></tr>" % (esc(base), esc(title), d["common"], len(d["changed"]), esc(str(d["first"]))))
        h.append("</table>")
    for base in BASES:
        cols = [("0", base + tag, title) for tag, title in VARIANTS if ("0", base + tag) in gpu["sheets"]]
        cols += [(a, base, "arm " + a) for a in ids if a != "0" and (a, base) in gpu["sheets"]]
        if not cols:
            continue
        ref = gpu["sheets"].get(cols[0][:2])
        passes = sorted(int(num(c.get("pass"), -1)) for c in cells_of(ref))
        aligned = {k: align(ref, gpu["sheets"][k[:2]])[0] for k in cols}
        h.append("<h3>%s: the sheets, pass by pass (red = differs from the first column)</h3><details open><summary>%d passes</summary><table><tr><th>pass</th><th>target</th><th>draws</th><th>GPU ms</th>" % (esc(base), len(passes)))
        h.append("".join("<th>%s</th>" % esc(c[2]) for c in cols) + "</tr>")
        ref_cells = {int(num(c.get("pass"), -1)): c for c in cells_of(ref)}
        for p in passes[:600]:
            rc = dct(ref_cells.get(p))
            fam = ", ".join("%s %s" % (esc(str(k)), esc(str(v))) for k, v in dct(rc.get("family")).items())
            h.append("<tr><td>%d</td><td>%s %sx%s</td><td>%s<br><small>%s</small></td><td>%s</td>" % (p, esc(str(rc.get("kind"))), esc(str(rc.get("w"))), esc(str(rc.get("h"))), esc(str(rc.get("draws"))), fam, esc(str(rc.get("gpu_ms")))))
            for k in cols:
                sheet = gpu["sheets"][k[:2]]
                pair = aligned[k].get(p)
                c = dct(pair[1] if pair else None)
                changed = bool(c.get("xxh")) and bool(rc.get("xxh")) and c.get("xxh") != rc.get("xxh")
                h.append(cell_html(sheet, classes.get(k[:2]), c, changed))
            h.append("</tr>")
        h.append("</table></details>")
    if gpu["rows"]:
        h.append("<h3>The sampled draws: the vertex truth</h3><table><tr><th>arm</th><th>scene</th><th>family</th><th>vs</th><th>pass</th><th>vertices</th><th>bytes</th><th>samples</th><th>CPU on screen</th>"
                 "<th>compared</th><th>max err</th><th>median</th><th>beyond 1e-2</th><th>GPU NaN (CPU finite)</th><th>spikes</th><th>solo px</th><th>finding</th></tr>")
        for a, scene, d, label in gpu["rows"]:
            b, cov, cpu, n, solo = dct(d.get("bytes")), dct(d.get("coverage")), dct(d.get("cpu")), dct(d.get("numbers")), dct(d.get("solo"))
            bytes_txt = b.get("skipped") or ("%s of %s ranges differ" % (b.get("diff_ranges"), b.get("ranges")) if b else "-")
            cov_txt = cov.get("samples") if cov.get("ran") is True else (cov.get("skipped") or "-")
            cpu_txt = cpu.get("refused") or ("%s (%.0f px)" % (cpu.get("on_screen"), num(cpu.get("area_px"))) if cpu else "-")
            cells = [a, scene, d.get("family"), str(d.get("vs"))[:8], d.get("pass"), d.get("vertex_count"), bytes_txt, cov_txt, cpu_txt,
                     n.get("compared", n.get("skipped", "-")), n.get("max_err", "-"), n.get("median_err", "-"), n.get("outliers", "-"), n.get("gpu_nonfinite_cpu_finite", n.get("gpu_nonfinite", "-")), n.get("spikes", "-"),
                     solo.get("px", solo.get("skipped", "-")), label or "-"]
            h.append("<tr>" + "".join("<td>%s</td>" % esc(str(c)) for c in cells) + "</tr>")
        h.append("</table>")
        worst = [(a, scene, d) for a, scene, d, _ in gpu["rows"] if lst(dct(d.get("numbers")).get("worst")) and num(dct(d.get("numbers")).get("max_err")) > TOLERANCE_ERR]
        if worst:
            h.append("<h4>Worst rows</h4>")
            for a, scene, d in worst[:16]:
                h.append("<details><summary>arm %s %s, %s, vs %s</summary><pre>%s</pre></details>" % (
                    esc(a), esc(scene), esc(str(d.get("family"))), esc(str(d.get("vs"))[:8]),
                    esc("\n".join("raw %s guest %s err %s gpu %s cpu %s" % (dct(w).get("raw"), dct(w).get("guest"), dct(w).get("err"), dct(w).get("gpu"), dct(w).get("cpu")) for w in lst(dct(d.get("numbers")).get("worst"))))))
    solos = [(a, n) for a in ids for n in sorted(files) if n.startswith("arm%s/solo_" % a) and n.endswith(".jpg")]
    if solos:
        h.append("<h3>Solo silhouettes (one cell a sampled draw, its slot in the table's solo column)</h3>")
        for a, n in solos:
            h.append('<span class="f"><img src="data:image/jpeg;base64,%s"><br>arm %s %s</span>' % (base64.b64encode(files[n]).decode(), esc(a), esc(Path(n).stem)))
    return "\n".join(h)


# ---------------------------------------------------------------- the reference (hashes and stats, never pictures; stays in out\diag\ref)
def ref_of(gpu):
    ref = {"cells": {}, "inputs": {}}
    for (a, scene), sheet in gpu["sheets"].items():
        if a != "0":
            continue
        ref["cells"][scene] = [{k: c.get(k) for k in ("pass", "target", "kind", "w", "h", "fmt", "draws", "mean", "black", "dhash", "xxh")} for c in cells_of(sheet)]
    for a, scene, d, _ in gpu["rows"]:
        if a == "0" and d.get("input_hash"):
            ref["inputs"]["%s/%s/%s/%s/%s" % (scene, d.get("vs"), d.get("ps"), d.get("vertex_count"), d.get("nth"))] = d.get("input_hash")
    return ref


def make_ref(gpu, folder):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "ref.json").write_text(json.dumps(ref_of(gpu), indent=0), encoding="utf-8")
    print("reference written:", folder / "ref.json")


def compare_ref(gpu, folder):
    """Lines (text, confirmed-or-not). Exact hashes are not compared across GPUs (rounding, ASTC against BC): the dHash and the stats at a tolerance."""
    p = folder / "ref.json"
    try:
        ref = json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return [("no reference at %s" % p, "UNCONFIRMED")]
    out = []
    mine = ref_of(gpu)
    for scene, cells in dct(ref.get("cells")).items():
        have = {int(num(c.get("pass"), -1)): c for c in lst(dct(mine["cells"]).get(scene))}
        want = {int(num(dct(c).get("pass"), -1)): dct(c) for c in lst(cells)}
        if not have:
            continue
        if len(have) != len(want):
            out.append(("%s: %d passes here, %d in the reference: this device's frame has another shape (settings or renderer)" % (scene, len(have), len(want)), "CONFIRMED (counts)"))
        off = []
        for pnum, c in sorted(want.items()):
            m = have.get(pnum)
            if not m or not c.get("dhash") or not m.get("dhash"):
                continue
            hd = hamming(c.get("dhash"), m.get("dhash"))
            ma, mb = lst(c.get("mean")), lst(m.get("mean"))
            mean = max((abs(num(x) - num(y)) for x, y in zip(ma, mb)), default=0.0)
            if (hd is not None and hd > TOLERANCE_DHASH) or mean > TOLERANCE_MEAN:
                off.append((pnum, hd, mean))
        if off:
            out.append(("%s: %d of %d passes are off the reference beyond the tolerance (first: pass %d, %s dHash bits, mean colour off by %.0f)" % (scene, len(off), len(want), off[0][0], off[0][1], off[0][2]),
                        "UNCONFIRMED (a different GPU may round; look at the cells)"))
    ins = dct(ref.get("inputs"))
    same = sum(1 for k, v in mine["inputs"].items() if ins.get(k) == v)
    diff = sum(1 for k, v in mine["inputs"].items() if k in ins and ins.get(k) != v)
    if same or diff:
        out.append(("guest inputs of the sampled draws: %d the same as the reference, %d different" % (same, diff),
                    "UNCONFIRMED (an as-played arm has no fixed timing; only a replay arm can expect them to match across devices)"))
    return out
