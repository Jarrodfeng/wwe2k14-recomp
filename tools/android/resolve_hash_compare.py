# Session 74 (Undertaker's white marks with readback_resolve_batch): compares the [resolve_stats] lines of two runs
# (route tools\input_scripts\android\s74_taker_resolve_hashes.txt: every read-back resolve's bytes hashed) destination by
# destination. Phone frames drift between runs, so each destination's hashes are compared in order, not by frame.
#   py tools\android\resolve_hash_compare.py out\android\runs\085 out\android\runs\086
import re
import sys
from collections import OrderedDict
from pathlib import Path

LINE = re.compile(r"\[resolve_stats\] resolve (\d+) dest 0x([0-9A-F]+) (\d+) bytes: non-zero bytes ([\d.]+)%.*?"
                  r"color_info 0x([0-9A-F]+) surface_info 0x([0-9A-F]+), hash ([0-9A-F]+), frame (\d+)")


def run_log(run_dir):
    logs = sorted(Path(run_dir, "logs").glob("wwe2k14_*.log"), key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
    return logs[-1]


def read(run_dir):
    per_dest = OrderedDict()
    for line in run_log(run_dir).open(encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if m:
            per_dest.setdefault(m.group(2), []).append(
                dict(size=int(m.group(3)), nonzero=float(m.group(4)), color=m.group(5), surface=m.group(6),
                     hash=m.group(7), frame=int(m.group(8))))
    return per_dest


def main():
    a, b = read(sys.argv[1]), read(sys.argv[2])
    print(f"{sys.argv[1]}: {sum(map(len, a.values()))} read-back resolves, {len(a)} destinations")
    print(f"{sys.argv[2]}: {sum(map(len, b.values()))} read-back resolves, {len(b)} destinations")
    same = differ = 0
    for dest in list(a) + [d for d in b if d not in a]:
        ha, hb = [e["hash"] for e in a.get(dest, [])], [e["hash"] for e in b.get(dest, [])]
        if ha == hb:
            same += 1
            continue
        differ += 1
        e = (a.get(dest) or b.get(dest))[0]
        print(f"DIFFER 0x{dest} {e['size']} bytes color_info 0x{e['color']} surface 0x{e['surface']}: "
              f"A {len(ha)} copies, B {len(hb)}; final A {ha[-1] if ha else '-'} B {hb[-1] if hb else '-'}")
        for side, entries in (("A", a.get(dest, [])), ("B", b.get(dest, []))):
            print(f"   {side}: " + " ".join(f"{x['hash'][:8]}@{x['frame']}({x['nonzero']:.0f}%)" for x in entries[:12])
                  + (" ..." if len(entries) > 12 else ""))
    print(f"destinations with the same hashes in order: {same}, different: {differ}")


if __name__ == "__main__":
    main()
