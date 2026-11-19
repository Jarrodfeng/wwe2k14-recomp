"""S103 MSF (the LVF class again: a census the stream host prints and the stream-free (OFF) host never did; run 013 could not say why there was no mat shadow because
`[scene][maps][e2]` is built only by LiveSceneDraw::Census(), which only live.cpp printed).

Rule: every scene census builder that renderer_adapter\\live.cpp prints (a Log(...) line with scene_draw->X, scene_frame::X, scene_frame::missing_hist::X or scene_kind::X, where X names a
Census / MissingLine builder) must ALSO be printed by the OFF host: named in src\\renderer_host\\scene_host.cpp, or inside rdr_scene::live::CensusLog (scene_host calls it through opts_.contributor_census) or scene_skip::MaybeLog (ON+OFF), or the live.cpp line carries a `CENSUS-OFF-ONLY: why` mark.
   py -I tools\\scene\\off_census_lint.py --root .            exit 1 on a builder the OFF host does not print
   py -I tools\\scene\\off_census_lint.py --root . --plant    the twin: `draw_->Census()` dropped from scene_host.cpp; exit 0 = the lint caught it
Text scan, no compile."""
import re
import sys
from pathlib import Path

PAT = re.compile(r"(scene_draw->\w+|scene_frame::missing_hist::\w+|scene_frame::\w+|scene_kind::\w+)\(")
NAME = re.compile(r"Census|MissingLine")


def norm(k):
    return k.replace("scene_draw->", "draw_->")


def live_builders(live):
    """-> {normalised builder: (line no, marked CENSUS-OFF-ONLY)} for every census Log line in live.cpp."""
    out = {}
    for n, line in enumerate(live.splitlines(), 1):
        if "Log(" not in line and "Census()" not in line:
            continue
        for m in PAT.finditer(line):
            if not NAME.search(m.group(1)):
                continue
            k = norm(m.group(1))
            marked = "CENSUS-OFF-ONLY:" in line
            if k not in out or (marked and not out[k][1]):
                out[k] = (n, marked)
    return out


def off_text(host, scene, skip):
    i = scene.find("void CensusLog(")
    body = scene[i: scene.find("\n}\n", i)] if i >= 0 else ""
    j = skip.find("inline void MaybeLog(")   # scene_skip::MaybeLog is ON+OFF: scene_frame::CensusLine rides the scene_skip line
    return host + "\n" + body + "\n" + (skip[j: skip.find("\n}\n", j)] if j >= 0 else "")


def lint(live, host, scene, skip=""):
    bad = []
    b = live_builders(live)
    if not b:
        return ["live.cpp: no census builder found: the lint's pattern is out of date"]
    off = off_text(host, scene, skip)
    for k, (n, marked) in sorted(b.items()):
        tok = k.replace("draw_->", "draw_->")
        if marked:
            continue
        if tok not in off and ("draw_->" not in tok or tok.replace("draw_->", "scene_draw->") not in off):
            bad.append("live.cpp:%d prints %s but the OFF host (scene_host.cpp / CensusLog) does not: print it there or mark the live.cpp line `CENSUS-OFF-ONLY: why`" % (n, k))
    return bad


def main():
    root = Path(sys.argv[sys.argv.index("--root") + 1]) if "--root" in sys.argv else Path(".")
    rd = lambda p: (root / p).read_text(encoding="utf-8", errors="replace")
    live, host, scene, skip = rd("src/renderer_adapter/live.cpp"), rd("src/renderer_host/scene_host.cpp"), rd("src/renderer_scene/live_scene.cpp"), rd("src/scene_live/scene_skip.h")
    if "--plant" in sys.argv:
        planted = host.replace("draw_->Census()", "draw_->XCensus_removed()")
        if planted == host:
            print("FAIL the plant did not apply")
            return 1
        bad = lint(live, planted, scene, skip)
        print(("PASS planted missing print caught: " if bad else "FAIL planted missing print NOT caught: ") + "; ".join(bad))
        return 0 if bad else 1
    bad = lint(live, host, scene, skip)
    for x in bad:
        print("FAIL " + x)
    print(("FAIL" if bad else "PASS") + " off_census_lint (%d builder(s) checked, %d problem(s))" % (len(live_builders(live)), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
