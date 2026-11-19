"""S103 CEF: the arena props (ring steps / announce desk) are added only to arenas with a ring set.
Runs out\\tools\\props_check.exe on arenas 66 (CAW editor, no ring set) and 4 (match arena).
Bars: bg66 steps = desk = 0; bg04 count == its pre-fix count (the --plant run, props on every arena) and > 0.
--plant-expect-fail: judges the planted twin (props added regardless) by the same bars; exits 0 only when the plant is CAUGHT (bars fail).
Usage: py -I tools\\assets\\props_check.py [--game game_data\\extracted] [--plant-expect-fail]"""
import re, subprocess, sys, pathlib
root = pathlib.Path(__file__).resolve().parents[2]
exe = root / "out" / "tools" / "props_check.exe"
args = sys.argv[1:]
game = str(root / "game_data" / "extracted")
if not pathlib.Path(game).is_dir():  # a worktree has no game_data: use the main checkout's (the parent of .claude\worktrees\<name>)
    game = str(root.parents[2] / "game_data" / "extracted")
if "--game" in args:
    game = args[args.index("--game") + 1]


def run(*extra):
    r = subprocess.run([str(exe), "--game", game, *extra, "66", "4"], capture_output=True, text=True)
    out = {}
    for ln in r.stdout.splitlines():
        m = re.match(r"arena (\d+) steps (\d+) desk (\d+)", ln)
        if m:
            out[int(m[1])] = (int(m[2]), int(m[3]))
        else:
            print(ln)
    if len(out) != 2:  # a missing game folder / failed load must never pass (or "catch" a plant) vacuously
        print("props_check: arenas did not load:", r.stdout, r.stderr)
        sys.exit(2)
    return out


def verdict(o, base4=None):
    bad = []
    if o.get(66) != (0, 0):
        bad.append(f"bg66 {o.get(66)} (want (0, 0))")
    if not o.get(4) or o[4][0] <= 0 or o[4][1] <= 0:
        bad.append(f"bg04 {o.get(4)} has no props")
    if base4 is not None and o.get(4) != base4:
        bad.append(f"bg04 {o.get(4)} != pre-fix {base4}")
    return bad


plant = run("--plant")
print("plant (old behaviour):", plant)
if "--plant-expect-fail" in args:
    bad = verdict(plant)
    print("planted twin verdict:", bad or "PASSED (the plant was NOT caught)")
    sys.exit(0 if bad else 1)
cur = run()
print("fixed:", cur)
bad = verdict(cur, plant.get(4))
for b in bad:
    print("FAIL", b)
print("props_check", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)
