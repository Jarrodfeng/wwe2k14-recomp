"""S103 NR (3): Warm and DrawLive must apply the SAME hole rules. The arena's entrance holes and the crowd's light holes live in the appliers (rf_arena_live.cpp, rf_crowd.cpp),
which both builds run; a `warm()` test in front of a hole statement would make Warm READY on a frame DrawLive then refuses. This lint fails when one comes back.
   py -I tools\\scene\\nr_symmetry_lint.py --root .   [--plant]     (--plant: the twin, a planted bad copy: exit 0 = the lint caught it)
"""
import re
import sys
from pathlib import Path


def lint(arena: str, crowd: str, draw: str):
    bad = []
    # arena: the two entrance hole blocks must not sit behind `!ctx.warm()`
    for m in re.finditer(r"^.*warm\(\).*$", arena, re.M):
        line = m.group(0)
        if "UpdatePages" in line:
            continue
        if re.search(r"if\s*\(\s*!\s*ctx\.warm\(\)", line):
            bad.append("rf_arena_live.cpp: a hole rule behind !ctx.warm(): " + line.strip()[:120])
    if "MissingFor" not in arena or "pools_incomplete" not in arena:
        bad.append("rf_arena_live.cpp: the entrance hole rules are gone")
    # crowd: LightHoles is asked before the warm return
    i_holes, i_warm = crowd.find("LightHoles(t)"), crowd.find("if (ctx.warm()) return true;")
    if i_holes < 0 or i_warm < 0 or i_holes > i_warm:
        bad.append("rf_crowd.cpp: LightHoles must be asked before the warm return")
    if re.search(r"if\s*\(\s*ctx\.warm\(\)\s*\|\|\s*pre\.misses", crowd):
        bad.append("rf_crowd.cpp: the old early return `ctx.warm() || pre.misses` is back")
    # Warm and DrawLive: one terms function (S111 SHADOWKEY review: the shadow input hole is removed: the fitted shadow light needs no key light)
    w = draw[draw.find("void LiveSceneDraw::Warm"):draw.find("uint32_t LiveSceneDraw::BuildTerms")]
    d = draw[draw.find("bool LiveSceneDraw::DrawLive"):draw.find("bool LiveSceneDraw::DrawToPng")]
    if "BuildTerms(" not in w or "BuildTerms(" not in d:
        bad.append("live_scene_draw.cpp: Warm and DrawLive must both use BuildTerms")
    if "TotalHoles" in w:
        bad.append("live_scene_draw.cpp: Warm has its own hole test again (use BuildTerms)")
    # S103 HN: a hole / shadow input the OFF build SKIPS must still be counted (class + first reason): NoteTerms takes the skipped terms, Warm and DrawLive hand them over
    n = draw[draw.find("void LiveSceneDraw::NoteTerms"):draw.find("std::string LiveSceneDraw::ReadyCensusLine")]
    if "terms |= skipped" not in n:
        bad.append("live_scene_draw.cpp: NoteTerms does not count the terms OFF skips (a skipped hole would be unnamed)")
    if "NoteTerms(/*live=*/false, terms, reports, error, skipped)" not in w or "error, skipped)" not in d:
        bad.append("live_scene_draw.cpp: Warm / DrawLive do not hand the skipped terms to NoteTerms")
    return bad


def main():
    root = Path(sys.argv[sys.argv.index("--root") + 1]) if "--root" in sys.argv else Path(".")
    rs = root / "src" / "renderer_scene"
    arena = (rs / "rf_arena_live.cpp").read_text(encoding="utf-8", errors="replace")
    crowd = (rs / "rf_crowd.cpp").read_text(encoding="utf-8", errors="replace")
    draw = (rs / "live_scene_draw.cpp").read_text(encoding="utf-8", errors="replace")
    if "--plant" in sys.argv:   # twin: the old Warm guards back
        arena = arena.replace("  if (stage_look::PageEntrance(ctx.t)) {", "  if (!ctx.warm() && stage_look::PageEntrance(ctx.t)) {", 1)   # S111 PAGEPOOLS: the page-hole block asks PageEntrance
        crowd = crowd.replace("if (pre.misses || pre.failures) return true;", "if (ctx.warm() || pre.misses || pre.failures) return true;", 1)
        draw = draw.replace("terms |= skipped;", "", 1)   # S103 HN twin: a skipped hole that is not counted
    bad = lint(arena, crowd, draw)
    if "--plant" in sys.argv:   # the twin: the lint must CATCH the planted guards (exit 0 = caught)
        print(("PASS planted guards caught: " if bad else "FAIL planted guards NOT caught: ") + "; ".join(bad))
        return 0 if bad else 1
    for b in bad:
        print("FAIL " + b)
    print(("FAIL" if bad else "PASS") + " nr_symmetry_lint (%d problem(s))" % len(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
