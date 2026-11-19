"""S103 SOF: an ArenaSet visitor must see EVERY family model (part[], the static cuts after each moving slot, the moving slots).

A loop over `as.part` / `as->part` in an rf_*.cpp visits part[] only: the cuts (the static arena after each moving slot) are then missed by a page stale test, an upload or a census
(the bug class the SOF review found in UpdatePages / PageCensus). Such a loop goes through `ForEachFamilyModel(as, fn)` (rf_c.h). Indexed access `as->part[size_t(Part::Props)]` is fine.

Fails (exit 1) on any `for (... : as.part)` / `for (... : as->part)` / `std::begin(as.part)` / `as.part.begin()`-style walk in src\\renderer_scene\\rf_*.cpp or live_flashes.cpp.

  py -I tools\\renderer\\arena_visit_lint.py [--self-test]
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "renderer_scene"
LOOP = re.compile(r"for\s*\([^;)]*:\s*[\w:]*(?:\*?\s*)?\b\w+\s*(?:\.|->)\s*part\s*\)|\b(?:std::)?(?:begin|end)\s*\(\s*\w+\s*(?:\.|->)\s*part\s*\)|\b\w+\s*(?:\.|->)\s*part\s*\.\s*(?:begin|end)\s*\(")


def scan(text):
    bad = []
    for n, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        if LOOP.search(code):
            bad.append((n, line.strip()))
    return bad


def main():
    if "--self-test" in sys.argv:
        planted = "static void X(ArenaSet& as) {\n  for (FamilyModel& fm : as.part) one(fm);\n}\n"
        clean = "static void X(ArenaSet& as) {\n  ForEachFamilyModel(as, one);\n  const FamilyModel& fm = as->part[size_t(Part::Props)];\n}\n"
        ok = bool(scan(planted)) and not scan(clean) and bool(scan("for (const auto& m : as->part) f(m);"))
        print("arena_visit_lint self-test:", "OK (planted direct loop caught, helper and indexed access pass)" if ok else "FAIL")
        return 0 if ok else 1
    files = sorted(list(SRC.glob("rf_*.cpp")) + [SRC / "live_flashes.cpp"])
    rc = 0
    for f in files:
        for n, line in scan(f.read_text(encoding="utf-8", errors="replace")):
            print(f"{f.relative_to(REPO)}:{n}: loops over the arena set's part[] directly (misses the cuts / moving slots): use ForEachFamilyModel(as, fn): {line}")
            rc = 1
    print("arena_visit_lint:", "FAIL" if rc else f"OK ({len(files)} files, no direct part[] loop)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
