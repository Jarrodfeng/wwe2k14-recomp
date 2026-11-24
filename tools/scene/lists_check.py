"""Lane G (S103) check: the kept draw lists draw the same picture as no lists (frame_rig --lists N, S103 R2: every repeat is drawn with the lists and
without, byte-compared), on two frames of the Brock capture; and the planted fault (the lists ignore a moved world) must FAIL (exit 4).
   py -I tools/scene/lists_check.py [--capture DIR] [--game DIR]     exit 0 = good
Needs out/tools/frame_rig.exe (tools/scene/build_rig.ps1; built here when missing). 0 game runs: frame_rig is the offline renderer.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAIN = os.environ.get("WWE2K14_MAIN_CHECKOUT", r"C:\Users\Jarrod\wwe2k14-recomp")


def pick(rel):
    for base in (ROOT, MAIN):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return p
    return None


def run(exe, args, timeout=900):
    p = subprocess.run([exe] + args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=None)
    ap.add_argument("--game", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "s103", "G"))
    a = ap.parse_args()
    cap = a.capture or pick(os.path.join("out", "captures", "brock_losswin_rt"))
    game = a.game or pick(os.path.join("game_data", "extracted"))
    if not cap or not game:
        print("lists_check: SKIP (capture or game files not on this machine)")
        return 0
    exe = os.path.join(ROOT, "out", "tools", "frame_rig.exe")  # S103 R2: rsc_draw lost --lists when it moved onto the record -> frame library; frame_rig carries it now
    if True:  # S103 R2: always the incremental build (a stale exe measures old code); build_rig.ps1 counts tools\scene\*.h and src\** headers
        b = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "scene", "build_rig.ps1")], capture_output=True, text=True, timeout=1800)
        if b.returncode != 0:
            print(b.stdout + b.stderr)
            print("lists_check: BUILD FAILED")
            return 2
    os.makedirs(a.out, exist_ok=True)
    base = ["--synth-camera", "--game", game, "--size", "960x540"]
    rec = lambda n: os.path.join(cap, "scene", "frame_%06d.rsc" % n)
    bad = 0
    for frame in (30, 180):
        rc, out = run(exe, base + ["--record", rec(frame), "--prefix", os.path.join(a.out, "chk_f%d" % frame), "--lists", "4"])
        line = [l for l in out.splitlines() if "[scene][lists]" in l or "differ from the no-lists" in l]
        print("frame %d: exit %d" % (frame, rc))
        for l in line:
            print("  " + l)
        if rc != 0 or "0 differ" not in out:
            bad += 1
    # S103 R2: the lists must also be equal under the game's see-through order (the live frame's order): one more frame, Distance and Game
    for order in ("game", "distance"):
        rc, out = run(exe, base + ["--record", rec(180), "--prefix", os.path.join(a.out, "chk_order_" + order), "--lists", "4", "--see-order", order])
        ok = rc == 0 and "0 differ" in out
        print("see-order %s: %s (exit %d)" % (order, "0 differ" if ok else "DIFFERS", rc))
        bad += 0 if ok else 1
    for plant in ("--lists-plant",):  # a moved world ignored (the pixel plant)
        rc, out = run(exe, base + ["--record", rec(180), "--prefix", os.path.join(a.out, "chk_plant"), "--lists", "4", plant])
        ok = rc == 4 and "differs from the no-lists draw" in out
        print("plant %s: %s (exit %d)" % (plant, "caught" if ok else "NOT CAUGHT", rc))
        bad += 0 if ok else 1
    # S103 SL: the old pixel twin --lists-plant-specular (SpecularCol's mask without the light) is dead: since L5 no plain arena / prop draw has a highlight (variant_rule 244/244),
    # so it moves no pixel (docs/research/s103/SL_stage_look_lists.md Q2). Review B1's claim is checked pixel-free on the field table instead: pacing_unit must pass clean
    # and --plant-specmask (the mask without kStLight) must fail it (the table check and the light-change stale check). frame_rig still has the switch, unused here.
    unit = os.path.join(ROOT, "out", "tools", "pacing_unit.exe")
    b = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "scene", "build_pacing_unit.ps1")], capture_output=True, text=True, timeout=600)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print("lists_check: pacing_unit BUILD FAILED")
        return 2
    rc, out = run(unit, [])
    ok = rc == 0
    print("specular mask unit (SpecularCol = light + material), clean: %s (exit %d)" % ("pass" if ok else "FAIL", rc))
    bad += 0 if ok else 1
    rc, out = run(unit, ["--plant-specmask"])
    ok = rc == 1 and "SpecularCol must depend on the light" in out and "stale" in out
    print("plant specular mask without the light (pixel-free): %s (exit %d)" % ("caught" if ok else "NOT CAUGHT", rc))
    bad += 0 if ok else 1
    print("lists_check: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
