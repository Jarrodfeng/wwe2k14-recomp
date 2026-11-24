# Lane C's check (Session 103): builds c_driver, draws brock_losswin frame 110 and brock_losswin_rt frame 100 (the arena, ring, props, screens, flashes from the record,
# with synthetic stage rows), then c_sheet.py checks the planted faults, the rest-matrix rows, validation and magenta. Exit 0 = all good. 0 game runs.
#   py -I tools\scene\c_check.py [--root .] [--game DIR] [--captures DIR]
import argparse, os, subprocess, sys
ap = argparse.ArgumentParser()
ap.add_argument("--root", default=".")
ap.add_argument("--game", default=r"C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted")
ap.add_argument("--captures", default=r"C:\Users\Jarrod\wwe2k14-recomp\out\captures")
ap.add_argument("--no-build", action="store_true")
a = ap.parse_args()
root = os.path.abspath(a.root)
if not a.no_build:
    r = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(root, "tools", "scene", "build_c_driver.ps1")], cwd=root)
    if r.returncode: sys.exit(r.returncode)
out = os.path.join(root, "out", "s103", "C", "gate")
os.makedirs(out, exist_ok=True)
rc = 0
for cap, frame, tag in (("brock_losswin", 110, "g_a110"), ("brock_losswin_rt", 100, "g_b100")):
    env = dict(os.environ, WWE_RDR_VALIDATION="1")
    with open(os.path.join(out, tag + "_log.txt"), "w", encoding="utf-8") as lg:
        r = subprocess.run([os.path.join(root, "out", "tools", "c_driver.exe"), "--game", a.game, "--capture", os.path.join(a.captures, cap), "--frame", str(frame), "--out", out, "--tag", tag],
                           stdout=lg, stderr=subprocess.STDOUT, env=env, timeout=600)
    print(f"{tag}: driver exit {r.returncode}")
    rc |= r.returncode
    rc |= subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "scene", "c_sheet.py"), out, tag]).returncode
print("RESULT", "OK" if rc == 0 else "FAIL")
sys.exit(1 if rc else 0)
