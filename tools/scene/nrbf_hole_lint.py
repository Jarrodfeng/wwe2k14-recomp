"""S103 NRBF (review F2): every skip of a VISIBLE wrestler object in rf_wrestlers.cpp must count a hole, unless its reason is "not resident" (still loading = a miss, counted by the cache).
A skip without `++rep.holes` lets the live frame go ours without that wrestler (it failed open for the palette-size mismatch before this lint). The invisible-object branch (skipped_invisible) is
decided by the NR / NRB rules (own-skeleton pieces) and is not checked here.
   py -I tools\\scene\\nrbf_hole_lint.py --root .   [--plant]     (--plant: the twin, a planted bad copy: exit 0 = the lint caught it)
"""
import re
import sys
from pathlib import Path

CHECKED = ("skipped_unnamed", "skipped_not_resident", "skipped_palette_mismatch", "skipped_body_scale")


def lint(src: str):
    bad = []
    lines = src.splitlines()
    seen = set()
    for i, line in enumerate(lines):
        m = re.search(r"\bskip\(st\.(\w+),", line)
        if not m or m.group(1) not in CHECKED:
            continue
        seen.add(m.group(1))
        window = "\n".join(lines[max(0, i - 2): i + 3])
        if "++rep.holes" not in window:
            bad.append("rf_wrestlers.cpp:%d: skip(%s) without ++rep.holes" % (i + 1, m.group(1)))
        elif m.group(1) == "skipped_not_resident" and 'find("not resident")' not in window:
            bad.append("rf_wrestlers.cpp:%d: the not-resident skip must count a hole unless its reason is \"not resident\"" % (i + 1))
    for name in CHECKED:
        if name not in seen:
            bad.append("rf_wrestlers.cpp: no skip(st.%s, ...) found: the lint's pattern is out of date" % name)
    return bad


def lint_light_refs(files):
    """Review F1: a light-ref read in rf_*.cpp is evidence about the GAME's draws only when the ref is WALKED; an OURS frame carries synthetic refs (kLsSynthetic) from the latch.
    Every `t.light_refs` read must filter (WalkedRef / kLsSynthetic within 2 lines) or say `synthetic ok` (the reason, on the line)."""
    bad = []
    for name, src in files.items():
        lines = src.splitlines()
        for i, line in enumerate(lines):
            if "t.light_refs" not in line or line.lstrip().startswith("//"):
                continue
            window = "\n".join(lines[max(0, i - 1): i + 3])
            if "WalkedRef(" not in window and "kLsSynthetic" not in window and "synthetic ok" not in window:
                bad.append("%s:%d: a light_refs read with no kLsSynthetic filter" % (name, i + 1))
    return bad


def main():
    root = Path(sys.argv[sys.argv.index("--root") + 1]) if "--root" in sys.argv else Path(".")
    src = (root / "src" / "renderer_scene" / "rf_wrestlers.cpp").read_text(encoding="utf-8", errors="replace")
    rf_files = {p.name: p.read_text(encoding="utf-8", errors="replace") for p in sorted((root / "src" / "renderer_scene").glob("rf_*.cpp"))}
    if "--plant-synth" in sys.argv:   # twin: a hole exemption reading refs with no filter
        bad = lint_light_refs({"planted.cpp": "for (const LightRef& r : t.light_refs)\n  if (r.object == addr) return true;\n"})
        print(("PASS planted unfiltered light_refs read caught: " if bad else "FAIL NOT caught: ") + "; ".join(bad))
        return 0 if bad else 1
    extra = lint_light_refs(rf_files)
    if "--plant" in sys.argv:   # twin: the fail-open palette-mismatch skip back
        planted = re.sub(r"(skip\(st\.skipped_palette_mismatch,[^\n]*\n)\s*\+\+rep\.holes;[^\n]*\n", r"\1", src, count=1)
        if planted == src:
            print("FAIL the plant did not apply")
            return 1
        src = planted
    bad = lint(src) + ([] if "--plant" in sys.argv else extra)
    if "--plant" in sys.argv:
        print(("PASS planted fail-open skip caught: " if bad else "FAIL planted fail-open skip NOT caught: ") + "; ".join(bad))
        return 0 if bad else 1
    for b in bad:
        print("FAIL " + b)
    print(("FAIL" if bad else "PASS") + " nrbf_hole_lint (%d problem(s))" % len(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
