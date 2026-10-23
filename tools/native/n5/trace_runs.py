"""Session 48 (Half 2, N5a): sums a --wwe2k14_trace log of the register-run writer 0x82CBD428 (r3 device, r4 the
64-bit run mask, r5 the first register, r6 the shadow block): per first register, the shadow offsets from the
device seen, the union of the masks and the callers. Usage: py tools\native\n5\trace_runs.py LOG"""
import collections, re, sys

pat = re.compile(r"\[trace\] 0x82CBD428 #\d+ from (0x[0-9A-F]+) r3=(0x[0-9A-F]+) r4=(0x[0-9A-F]+) r5=(0x[0-9A-F]+) "
                 r"r6=(0x[0-9A-F]+)")
offs, masks, callers, devs = (collections.defaultdict(set), collections.defaultdict(int),
                              collections.defaultdict(collections.Counter), set())
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    caller, dev, mask, r5, r6 = (int(x, 16) for x in m.groups())
    devs.add(dev)
    offs[r5].add(r6 - dev)
    masks[r5] |= mask
    callers[r5][caller] += 1
for r in sorted(offs):
    bits = [i for i in range(64) if masks[r] >> (63 - i) & 1]
    print(f"0x{r:04X}: shadow device+{sorted(offs[r])} mask bits (MSB=0) {bits[0]}..{bits[-1]} ({len(bits)} set) "
          f"-> registers 0x{r + bits[0]:04X}..0x{r + bits[-1]:04X}; callers "
          + ", ".join(f"0x{c:08X} x{n}" for c, n in callers[r].most_common()))
print("devices:", ", ".join(f"0x{d:08X}" for d in sorted(devs)))
