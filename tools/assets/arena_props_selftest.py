"""S111 ARENAFIX selftest: builds out/tools/arena_props_test.exe (tools/assets/build_arena_props_test.ps1, run here so the prover's parallel lines never race it) and runs it on the
player's files; then rebuilds and runs the loader's older props check (tools/assets/props_check.py, clean and its planted twin), which the new plan must not break.
The clean run must pass every check; each planted twin must fail exactly its named checks.
  py -I tools/assets/arena_props_selftest.py [--game DIR]
"""
import pathlib
import subprocess
import sys

PLANTS = {
    "fixed32": ["ap.variant"],     # the old rule: steps 32 on every arena
    "all_props": ["ap.presence"],  # the old rule: steps and desk on every ring arena
    "loader_fixed": ["ap.load", "ap.deskload"],   # the shipped LoadArena with its old fixed choice (load_detail::AlwaysPropsPlant; its unread plan is desk 6 / top 7 too)
    "desk67": ["ap.desk"],         # S111 DESKMODEL: the old rule, desk 6 / top 7 in every arena (the plan)
    "old_desk": ["ap.deskload"],   # S111 DESKMODEL: the shipped LoadArena with the old 6 / 7 (load_detail::OldDeskPlant)
    "desk_slot0": ["ap.deskload"],   # S111 DESKSKIN: each desk / top mesh whole on its slot 0 row (load_detail::DeskSlot0Plant)
    "gate_off": ["ap.gate"],       # S111 PLACECOUNT0: the placement table read without the loader's block-1 gate (load_detail::PlacementGateOffPlant)
    "unsigned_counts": ["ap.signed"],   # S111 PLACESIGNED: the placement table's counts read unsigned (load_detail::UnsignedCountsPlant)
}


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    game = root / "game_data" / "extracted"
    if not game.is_dir():  # a worktree has no game_data: the main checkout's (three levels up)
        game = root.parents[2] / "game_data" / "extracted"
    if "--game" in sys.argv:
        game = pathlib.Path(sys.argv[sys.argv.index("--game") + 1])
    if not game.is_dir():
        print("FAIL: no game folder %s" % game)
        return 1
    bad = 0
    for script in ("build_arena_props_test.ps1", "build_props_check.ps1"):
        b = subprocess.run(["pwsh", "-NoProfile", "-File", str(root / "tools" / "assets" / script)], capture_output=True, text=True, timeout=600)
        if b.returncode != 0:
            print(b.stdout + b.stderr)
            print("FAIL: tools/assets/%s failed" % script)
            return 1
    exe = root / "out" / "tools" / "arena_props_test.exe"
    r = subprocess.run([str(exe), "--game", str(game)], capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or "FAIL  " in r.stdout:
        print(r.stdout + r.stderr)
        print("FAIL: the clean run did not pass")
        bad += 1
    else:
        print("PASS clean run: %d checks" % r.stdout.count("PASS "))
    for plant, checks in PLANTS.items():
        r = subprocess.run([str(exe), "--game", str(game), "--plant", plant], capture_output=True, text=True, timeout=300)
        failed = [ln.split()[1] for ln in r.stdout.splitlines() if ln.startswith("FAIL  ")]
        if r.returncode == 1 and failed == checks:
            print("PASS plant %s fails exactly %s" % (plant, checks))
        else:
            print(r.stdout)
            print("FAIL: plant %s must fail exactly %s, got rc %d, failed %s" % (plant, checks, r.returncode, failed))
            bad += 1
    pc = root / "tools" / "assets" / "props_check.py"
    for extra, label in (([], "props_check clean"), (["--plant-expect-fail"], "props_check planted twin caught")):
        r = subprocess.run([sys.executable, "-I", str(pc), "--game", str(game), *extra], capture_output=True, text=True, timeout=600)
        if r.returncode == 0:
            print("PASS %s" % label)
        else:
            print(r.stdout + r.stderr)
            print("FAIL: %s (rc %d)" % (label, r.returncode))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
