"""S103 NRBF (review F4): the process hard-exits, so LiveSink::Stop's census lines never run in a normal run; FinalCensus (live.cpp) is what prints them. Every census line Stop prints must also be
in the FinalCensus reports lambda, or a scene shorter than one window prints nothing ("[scene][ready] census missing on hard exit").
   py -I tools\\scene\\nrbf_census_lint.py --root .   [--plant]     (--plant: the twin, one line removed from FinalCensus: exit 0 = the lint caught it)
"""
import re
import sys
from pathlib import Path

PAT = re.compile(r"(scene_draw->\w+|scene_frame::missing_hist::\w+|scene_frame::\w+|scene_kind::\w+|fallback_census::g_census\.\w+)\(")


def lint(src: str):
    bad = []
    s0 = src.find("void LiveSink::Stop()")
    stop = src[s0: src.find("\n}\n", s0)] if s0 >= 0 else ""
    f0 = src.find("FinalCensus final_census{")
    final = src[f0: src.find("}};", f0)] if f0 >= 0 else ""
    if not stop or not final:
        return ["live.cpp: LiveSink::Stop or the FinalCensus reports lambda was not found"]
    keys = set()
    for line in stop.splitlines():
        if "Log(" in line and "Census" in line or "MissingLine" in line or "FinalLine" in line or "ReadyCensusLine" in line:
            m = PAT.search(line)
            if m:
                keys.add(m.group(1))
    if not keys:
        bad.append("live.cpp: no census line found in LiveSink::Stop: the lint's pattern is out of date")
    for k in sorted(keys):
        if k.replace("scene_draw->", "") not in final if k.startswith("scene_draw->") else k not in final:
            bad.append("live.cpp: LiveSink::Stop prints %s but FinalCensus does not" % k)
    return bad


def main():
    root = Path(sys.argv[sys.argv.index("--root") + 1]) if "--root" in sys.argv else Path(".")
    src = (root / "src" / "renderer_adapter" / "live.cpp").read_text(encoding="utf-8", errors="replace")
    if "--plant" in sys.argv:
        planted = src.replace("scene_draw->ReadyCensusLine(true).c_str()); }\n      Log(scene_frame::CensusLine", "scene_draw->Census().c_str()); }\n      Log(scene_frame::CensusLine", 1)
        if planted == src:
            print("FAIL the plant did not apply")
            return 1
        src = planted
    bad = lint(src)
    if "--plant" in sys.argv:
        print(("PASS planted missing line caught: " if bad else "FAIL planted missing line NOT caught: ") + "; ".join(bad))
        return 0 if bad else 1
    for b in bad:
        print("FAIL " + b)
    print(("FAIL" if bad else "PASS") + " nrbf_census_lint (%d problem(s))" % len(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
