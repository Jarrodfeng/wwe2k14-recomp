"""S111 CENSUSTEXT selftest: the props / mapb census lines in src/renderer_scene/live_maps.cpp are read by pm2_check.parse and frame_rig.mapb_census.
Takes the two format strings out of the source, fills each %u / %s, and runs both parsers on them (the shipped text must parse, with every number
read from its own place). Planted old-text lines (piece 43 assumed ... / piece 43 %u) must NOT parse: the check is seen failing on the old text.
Run: py -I tools/scene/census_text_selftest.py"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import pm2_check, frame_rig

SRC = open(os.path.join(ROOT, "src", "renderer_scene", "live_maps.cpp"), encoding="utf-8").read()
bad = 0


def check(ok, what):
    global bad
    print(("PASS  " if ok else "FAIL  ") + what)
    bad += 0 if ok else 1


def fmt_of(tag):
    m = re.search(r'snprintf\(\w+, sizeof \w+, "(\\n\[scene\]\[maps\]\[' + tag + r'\][^"]*)"', SRC)
    return m.group(1).replace("\\n", "").replace("%%", "%") if m else None


def fill(f):
    n = iter(range(1, 100))
    return re.sub(r"%u|%s", lambda m: str(next(n)) if m.group(0) == "%u" else "drawn", f)


props_f, mapb_f = fmt_of("props"), fmt_of("mapb")
check(props_f is not None and mapb_f is not None, "both format strings found in live_maps.cpp")
props_line, mapb_line = fill(props_f), fill(mapb_f)
pc = pm2_check.parse(props_line)["census"]
check(pc is not None and pc["objects"] == 1 and pc["no_model"] == 7 and pc["no_light"] == 9 and pc["why"] == "drawn", "pm2_check.parse reads the props line: %s" % pc)
mc = frame_rig.mapb_census(mapb_line)
check(mc is not None and mc["skn"] == 9 and mc["p43"] == 10 and mc["oof"] == 11, "frame_rig.mapb_census reads the mapb line: %s" % mc)
check("assumed" not in props_line and "counted hole" not in mapb_line, "no 'assumed' / 'counted hole' left in the two lines")
old_props = props_line.replace("categories 33 / 43 left to map B 8 (the props pass never draws them)", "piece 43 assumed 8 (the re-key condition is not recorded)")
old_mapb = mapb_line.replace("categories 33 / 43 at scene word 1 10", "piece 43 10")
check(old_props != props_line and pm2_check.parse(old_props)["census"] is None, "planted old props text is refused by the parser")
check(old_mapb != mapb_line and frame_rig.mapb_census(old_mapb) is None, "planted old mapb text is refused by the parser")
print("census_text_selftest: %s" % ("PASS" if not bad else "%d FAIL" % bad))
sys.exit(1 if bad else 0)
