"""Per 60-frame window of a run log: passes per frame and the carried-copies census (gpu_native_truth_carry_copies,
SDK branch s80-carry; windows over 40 passes a frame). Session 80.

    py -I tools\\native\\carry_windows.py LOG
"""
import re
import sys

text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
truth = [(m.start(), int(m.group(1)), int(m.group(2)), float(m.group(3))) for m in
         re.finditer(r'\[native\]\[truth\] frames (\d+)-(\d+): truth frames.*?per frame: passes ([0-9.]+)', text)]
carry = [(m.start(), m.groups()) for m in re.finditer(
    r"carried copies per frame: kept ([0-9.]+) \(past the frame's end ([0-9.]+); dropped read by nothing ([0-9.]+), "
    r"run later ([0-9.]+)\); renames by a full-screen draw ([0-9.]+)", text)]
print(f"{'frames':>13} {'passes':>6} {'kept':>6} {'crossed':>7} {'dropped':>7} {'run':>6} {'cover_ren':>9}")
for i, (pos, a, b, passes) in enumerate(truth):
    end = truth[i + 1][0] if i + 1 < len(truth) else len(text)
    c = next((g for p, g in carry if pos < p < end), None)
    if c and passes > 40:
        print(f'{a:6d}-{b:6d} {passes:6.1f} ' + ' '.join(f'{float(x):7.2f}' for x in c))
