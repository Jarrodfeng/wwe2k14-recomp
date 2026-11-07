"""Session 95: offline test of tools\\native\\diag_read.py on a synthetic diagnostic zip. No game, no game data: every file here is made up.

    py tools\\native\\diag_read_test.py

Writes the zip and the page under the system's temp folder. Checks that the reader
  * finds the first pass where a wrestler's coverage vanishes, from the occlusion counts,
  * flags a wrestler draw that changed nothing, and a target gone black,
  * reads the upload, vertex and binning rules in order, on rows made for each,
  * shows the arms' differences and flags an unstable A/A and a perturbed frame,
  * compares with a reference at a tolerance,
  * survives hostile members: a path with "..", broken json, wrong types, html in names.
"""
import io
import json
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TINY_JPEG = bytes.fromhex("ffd8ffe000104a46494600010100000100010000ffdb004300") + bytes(64) + bytes.fromhex("ffd9")


def cell(p, target, draws, family, xxh, dhash, black=0.1, kind="color", mean=(90, 80, 70)):
    return {"pass": p, "kind": kind, "cell": p, "target": target, "w": 1280, "h": 720, "fmt": 37, "samples": 1, "colors": 1, "draws": draws, "family": family, "multiview": False,
            "vs": "aa", "ps": "bb", "vk_vs": "cc", "vk_fs": "dd", "gpu_ms": 0.25, "min": [0, 0, 0], "max": [255, 255, 255], "mean": list(mean), "black": black, "xxh": xxh, "dhash": dhash}


def sheet(scene, cells):
    return {"scene": scene, "seq": 1, "frame": 100, "mode": 17, "atlas": [2560, 720], "cell": [160, 90], "cols": 16, "passes": len(cells), "cells_used": len(cells), "overflow": 0, "draws_marked": 100,
            "images_usage_added": 2, "timestamp_period": 1.0, "cells": cells}


def row(family, vs, pass_, count=3000, nth=0, samples=100, bytes_diff=0, on_screen=True, numbers=None, solo=None):
    d = {"id": 1, "family": family, "vs": vs, "ps": "ff", "nth": nth, "pass": pass_, "vertex_count": count, "instances": 1, "indexed": True, "target": [1280, 720], "input_hash": vs + "00",
         "coverage": {"samples": samples, "ran": True, "skipped": None},
         "bytes": {"ranges": 2, "bytes": 65536, "diff_ranges": bytes_diff, "first_diff": 64 if bytes_diff else -1, "window": 1, "window_on": True, "list": []},
         "cpu": {"n": 100, "nonfinite": 0, "behind": 0, "box": [-1, -1, 1, 1], "on_screen": on_screen, "area_share": 0.2, "area_px": 180000.0}}
    if numbers is not None:
        d["numbers"] = numbers
    if solo is not None:
        d["solo"] = solo
    return d


def clean_numbers():
    return {"written": 3000, "gpu_nonfinite": 0, "box": [-1, -1, 1, 1], "spikes": 0, "radius95": 0.4, "compared": 2000, "no_compare": "", "cpu_nonfinite": 0, "max_err": 0.00001, "median_err": 0.000001,
            "outliers": 0, "box_cpu": [-1, -1, 1, 1], "worst": []}


def scene_line(scene, stable=True, after_same=True, hash_b="11", dhash="ff00", hash_c=None, after_px=None):
    d = {"scene": scene, "frame": 100, "stage": "entrance", "ms": 1000, "stable": stable, "attempts": 1, "hash_a": hash_b, "hash_b": hash_b, "dhash": dhash, "pending_pipelines": 0,
         "timing300": {"n": 300, "fps": 30.0, "low1_fps": 25.0, "p99_ms": 40.0, "max_ms": 50.0}, "stats_counter": 1, "sheet": True, "hash_c": hash_c or hash_b, "after_same": after_same}
    if after_px is not None:
        d.update(after_px=after_px, after_max=9, after_total=921600)
    return json.dumps(d)


def build(path):
    wrestler = {"wrestler": 5}
    base = [cell(0, 1, 40, {"bg": 40}, "a0", "1111"), cell(1, 1, 5, wrestler, "a1", "2222"), cell(2, 1, 5, wrestler, "a1", "2222"), cell(3, 2, 10, {"bg": 10}, "a3", "3333"),
            cell(4, 2, 2, {"blur": 2}, "a4", "4444", black=0.99), cell(5, 3, 0, {}, None, None, kind="depth")]
    base[5].pop("xxh"), base[5].pop("dhash")
    aa = [dict(c) for c in base]
    ablate = [dict(c) for c in base]
    ablate[1] = dict(ablate[1], xxh="b1")
    ablate[2] = dict(ablate[2], xxh="b2")
    vsoff = [dict(c) for c in base]
    psoff = [dict(c) for c in base]
    psoff[3] = dict(psoff[3], xxh="c3")
    truth = {"scene": "entrance", "seq": 2, "frame": 110, "mode": 2, "occlusion": "precise", "picks": 3, "picks_found": 3,
             "family_coverage": [{"pass": 1, "family": "wrestler", "draws": 5, "samples": 9000}, {"pass": 2, "family": "wrestler", "draws": 5, "samples": 0}],
             "draws": [row("wrestler", "aaaa0001", 1), row("wrestler", "aaaa0002", 2, samples=0), row("bg", "aaaa0003", 0, bytes_diff=1)]}
    bad_numbers = dict(clean_numbers(), gpu_nonfinite=12, outliers=40, max_err=3.5, spikes=4,
                       worst=[{"raw": 7, "guest": 7, "err": 3.5, "gpu": [1, 2, 3, 1], "cpu": [0.1, 0.2, 0.3, 1]}])
    vertex = {"scene": "match", "seq": 9, "frame": 900, "mode": 12, "occlusion": "precise", "picks": 2, "picks_found": 2, "family_coverage": [],
              "draws": [row("wrestler", "bbbb0001", 7, numbers=bad_numbers, solo={"cell": 0, "px": 500}), row("bg", "bbbb0002", 3, numbers=clean_numbers(), solo={"cell": 1, "px": 2500})]}
    for d in vertex["draws"]:
        d.pop("bytes"), d.pop("cpu"), d.pop("coverage")
    match_truth = {"scene": "match", "seq": 8, "frame": 800, "mode": 2, "occlusion": "precise", "picks": 2, "picks_found": 2, "family_coverage": [{"pass": 7, "family": "wrestler", "draws": 2, "samples": 500}],
                   "draws": [row("wrestler", "bbbb0001", 7), row("bg", "bbbb0002", 3)]}
    state_row = {"scene": "entrance", "seq": 3, "frame": 111, "mode": 12, "occlusion": "precise", "picks": 3, "picks_found": 3, "family_coverage": [],
                 "draws": [dict(row("wrestler", "aaaa0002", 2, samples=0), numbers=clean_numbers(), solo={"cell": 0, "px": 900})]}
    state_row["draws"][0].pop("bytes"), state_row["draws"][0].pop("coverage"), state_row["draws"][0].pop("cpu")
    device = {"deviceName": "Fake GPU <b>", "vendorID": 0x5143, "deviceID": 1, "apiVersion": (1 << 22) | (3 << 12), "driverVersion": 512, "instanceApiVersion": (1 << 22) | (3 << 12),
              "limits": {"maxStorageBufferRange": 134217728}, "features": {"vertexPipelineStoresAndAtomics": False}, "extensions": [], "formats": {}, "driver": {}, "memoryHeaps": []}
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("about.txt", "synthetic")
        z.writestr("arm0/device.json", json.dumps(device))
        z.writestr("arm0/stages.log", '{"stage":"boot"}\n{"stage":"done"}\n')
        z.writestr("arm0/exit.txt", "exit 0\n")
        # entrance_aa: a real change in the A/A picture must warn. match: a late pipeline's pixels between the base's pictures, the A/A equal to the third: no warning.
        z.writestr("arm0/scenes.jsonl", "\n".join([scene_line("entrance", after_same=False, after_px=40), scene_line("entrance_aa", hash_b="66"), scene_line("entrance_ablate", hash_b="22"),
                                                  scene_line("entrance_vsoff", after_same=False, after_px=40), scene_line("entrance_psoff", hash_b="33", after_same=False),
                                                  scene_line("entrance_big", after_same=False, after_px=5000),
                                                  scene_line("match", hash_b="44", hash_c="55", after_same=False, after_px=12), scene_line("match_aa", hash_b="55"), scene_line("match_vsoff", hash_b="55"), scene_line("match_psoff", hash_b="44"), "not json"]))
        for name, cells in (("entrance", base), ("entrance_aa", aa), ("entrance_ablate", ablate), ("entrance_vsoff", vsoff), ("entrance_psoff", psoff)):
            z.writestr("arm0/sheet_%s.json" % name, json.dumps(sheet(name, cells)))
            z.writestr("arm0/sheet_%s.jpg" % name, TINY_JPEG)
        z.writestr("arm0/truth_entrance.json", json.dumps(truth))
        z.writestr("arm0/truth_match.json", json.dumps(match_truth))
        z.writestr("arm0/vertex_match.json", json.dumps(vertex))
        z.writestr("arm0/solo_match.jpg", TINY_JPEG)
        z.writestr("armvtx/stages.log", '{"stage":"done"}\n')
        z.writestr("armvtx/scenes.jsonl", scene_line("entrance"))
        z.writestr("armvtx/vertex_entrance.json", json.dumps(state_row))
        z.writestr("armvtx/truth_entrance.json", json.dumps(dict(truth, draws=[row("wrestler", "aaaa0002", 2, samples=0)])))
        z.writestr("armsplit/stages.log", '{"stage":"done"}\n')
        z.writestr("armsplit/scenes.jsonl", scene_line("entrance", dhash="0fff"))
        fixed = [dict(c) for c in base]
        fixed[2] = dict(fixed[2], xxh="f2")
        z.writestr("armsplit/sheet_entrance.json", json.dumps(sheet("entrance", fixed)))
        z.writestr("armsplit/sheet_entrance.jpg", TINY_JPEG)
        # a small error is a note, not a verdict (a near-plane vertex's float eps on a phone)
        healthy = dict(clean_numbers(), compared=1114, outliers=12, max_err=0.008, median_err=0.0002,
                       worst=[{"raw": 1, "guest": 1, "err": 0.008, "gpu": [-1142.0, -266.3, 2180.5, 2190.4], "cpu": [-1147.5, -236.4, 2180.4, 2190.3]}])
        healthy_row = row("wrestler", "3f91ec05aaaa", 5, numbers=healthy)
        healthy_row.pop("bytes"), healthy_row.pop("cpu"), healthy_row.pop("coverage")
        z.writestr("arm0/vertex_healthy.json", json.dumps({"scene": "healthy", "seq": 6, "frame": 6, "mode": 4, "picks": 1, "picks_found": 1, "family_coverage": [], "draws": [healthy_row]}))
        # a whole mesh shifted by 0.03 of w (below the 0.05 line, every vertex off): a verdict, not a note
        shifted = dict(clean_numbers(), compared=1000, outliers=1000, max_err=0.03, median_err=0.03)
        shifted_row = row("wrestler", "5e1f7ed0aaaa", 5, numbers=shifted)
        shifted_row.pop("bytes"), shifted_row.pop("cpu"), shifted_row.pop("coverage")
        z.writestr("arm0/vertex_shifted.json", json.dumps({"scene": "shifted", "seq": 7, "frame": 7, "mode": 4, "picks": 1, "picks_found": 1, "family_coverage": [], "draws": [shifted_row]}))
        # diag4: a route that failed (run 6629: every arm ended in 3 s at boot, exit 0)
        z.writestr("armfail/stages.log", '{"stage":"boot","frame":1,"ms":244}\n')
        z.writestr("armfail/exit.txt", "0x00000000\nROUTE FAILED at stage 'boot' (after 3 s)\nelapsed_s 3\n")
        z.writestr("arm_summary.txt", "RESULT: THE ROUTE FAILED in at least one arm\narm fail (x): 0x00000000, ROUTE FAILED at stage 'boot' (after 3 s)\n")
        # hostile members
        z.writestr("../evil.txt", "x")
        z.writestr("arm0/sheet_evil.json", "{broken")
        z.writestr("arm0/sheet_inf.json", '{"cells": [{"pass": Infinity, "cell": NaN, "xxh": "a", "black": -Infinity}], "cols": Infinity, "overflow": NaN}')
        spike_row = dict(row("bg", "dddd0001", 4, numbers=dict(clean_numbers(), spikes=9, gpu_nonfinite=3, gpu_nonfinite_cpu_finite=0)))
        spike_row.pop("bytes"), spike_row.pop("cpu"), spike_row.pop("coverage")
        z.writestr("arm0/vertex_spike.json", json.dumps({"scene": "spike", "seq": 5, "frame": 5, "mode": 4, "picks": 1, "picks_found": 1, "family_coverage": [], "draws": [spike_row]}))
        z.writestr("arm0/sheet_weird.json", json.dumps({"cells": [{"pass": "x", "cell": "y", "target": {"a": 1}, "family": 3, "black": "z", "xxh": 5}, 7, None], "scene": "<script>alert(1)</script>"}))
        z.writestr("arm0/truth_weird.json", json.dumps({"draws": [7, {"family": "<img src=x>", "coverage": 3, "numbers": "no", "bytes": []}], "family_coverage": "nope"}))


def main():
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        z = t / "diagnostic_run_test.zip"
        build(z)
        # the reader writes under the current directory's out\diag (ref) and --out (page)
        out = t / "page"
        r = subprocess.run([sys.executable, "-I", str(HERE / "diag_read.py"), str(z), "--out", str(out), "--make-ref", "testref"], capture_output=True, text=True, cwd=td)
        print(r.stdout[-6000:])
        print(r.stderr[-3000:])
        assert r.returncode == 0, "the reader failed"
        text = r.stdout
        page = (out / "index.html").read_text(encoding="utf-8")
        checks = [
            ("the vanish pass from occlusion", "pass 2: 5 wrestler draw(s) passed no samples" in text),
            ("wrestler draws that changed nothing", "left target 1 exactly as it was" in text),
            ("a target gone black", "target 2 went black" in text),
            ("upload rule", "differs from the guest bytes in 1 of 2 fetch range(s)" in text),
            ("vertex rule", "the real vertex stage's output is wrong" in text),
            ("healthy float-order error is no verdict", "3f91ec05" not in text),
            ("healthy float-order error is a note", "3f91ec05" in page and "cause unconfirmed" in page),
            ("a whole mesh shifted by 0.03 is a verdict", "5e1f7ed0" in text),
            ("a failed route is named with its stage", "arm fail stopped at stage 'boot'" in text),
            ("the failed route's summary is in the page", "RESULT: THE ROUTE FAILED" in page),
            ("a spike next to a clean CPU comparison is no verdict", "dddd0001" not in text),
            ("binning rule", "no samples passed, the CPU puts it on screen" in text and "its positions are right" in text),
            ("perturbed frame", "observing left a lasting change" in text),
            ("a real change after the observed frame still warns, with its size", "entrance_big (5000 of 921600 pixels" in text and "the picture after the observed frame differs" in text),
            ("a few pixels after the observed frame are a note (the entrance base only)", "scene entrance: a small part of the picture changed" in page and "scene entrance: the picture after" not in text),
            ("the same few pixels in any other scene stay a verdict", "entrance_vsoff" in text and "match (" in text and "the picture after the observed frame differs" in text),
            ("an A/B equal to the base before the observed frame is the same picture (024cb478 rule)", "match: switching our pixel shaders" not in text),
            ("A/A picture really changed: warned", "entrance: the frozen frame is not stable" in text),
            ("A/A equal to the base's third picture: no warning", "match: the frozen frame is not stable" not in text),
            ("an A/B equal to the base's third picture changed nothing", "match: switching our vertex shaders" not in text),
            ("an A/B that really changes the picture still says so", "entrance: switching our pixel shaders" in text),
            ("A/A sheet identical: said plainly", "the A/A sheet is identical to the base in all" in page),
            ("pixel shader line", "flat grey changes the passes from 1 on" in text),
            ("every verdict line says it", all(("CONFIRMED" in l or "UNCONFIRMED" in l) for l in text.splitlines() if l.startswith(" - "))),
            ("html escaped", "<script>alert" not in page and "<img src=x>" not in page and "Fake GPU <b>" not in page),
            ("arm differences shown", "What each change did" in page and "arm split" in page),
            ("sheets side by side", 'class="c s' in page and "outline:3px" in page),
            ("hostile path skipped", "unsafe member name" in r.stderr),
        ]
        # the reference: the same zip matches itself; a second zip with other hashes is off it
        r2 = subprocess.run([sys.executable, "-I", str(HERE / "diag_read.py"), str(z), "--out", str(out), "--ref", "testref"], capture_output=True, text=True, cwd=td)
        checks.append(("reference: the same run", r2.returncode == 0 and "guest inputs of the sampled draws" in r2.stdout and "off the reference" not in r2.stdout))
        refp = t / "out" / "diag" / "ref" / "testref" / "ref.json"
        ref = json.loads(refp.read_text(encoding="utf-8"))
        ref["cells"]["entrance"][0]["dhash"] = "ffffffffffffffff"
        refp.write_text(json.dumps(ref), encoding="utf-8")
        r3 = subprocess.run([sys.executable, "-I", str(HERE / "diag_read.py"), str(z), "--out", str(out), "--ref", "testref"], capture_output=True, text=True, cwd=td)
        checks.append(("reference: a changed pass", "off the reference" in r3.stdout))
        bad = [n for n, ok in checks if not ok]
        for n, ok in checks:
            print("%s %s" % ("ok  " if ok else "FAIL", n))
        print("diag_read_test: %s" % ("PASS" if not bad else "%d FAILED" % len(bad)))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
