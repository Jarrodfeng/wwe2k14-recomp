"""Post-codegen check of every function hint (manifest inline + config/hints_*.toml) against generated code.
For each hint H: it must be registered; its predecessor P (registered fn just below H) should end exactly at H
(no uncovered gap) with an unconditional transfer (blr / b / bctr). Flags anything else for manual review.
Usage: postcheck_hints.py [only-this-file-substring]"""
import bisect
import glob
import os
import re
import sys

ROOT = r"C:\Users\Jarrod\wwe2k14-recomp"
GEN = os.path.join(ROOT, "generated", "default")
only = sys.argv[1] if len(sys.argv) > 1 else ""

hints = {}
for path in [os.path.join(ROOT, "wwe2k14_manifest.toml")] + sorted(glob.glob(os.path.join(ROOT, "config", "hints_*.toml"))):
    if only and only not in path:
        continue
    for m in re.finditer(r"^(0x[0-9A-Fa-f]{8})\s*=", open(path, encoding="utf-8").read(), re.M):
        hints[int(m.group(1), 16)] = os.path.basename(path)

reg = open(os.path.join(GEN, "wwe2k14_register.cpp"), encoding="utf-8").read()
name_at = {int(a, 16): n for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)", reg)}
starts = sorted(name_at)
info = {}
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        chunk = text[m.end(): end if end != -1 else len(text)]
        ins = [l.strip()[3:] for l in chunk.splitlines() if l.strip().startswith("// ")]
        labels = [int(x, 16) for x in re.findall(r"^\s*loc_([0-9A-Fa-f]+):", chunk, re.M)]
        info[m.group(1)] = (ins, labels, "switch (" in chunk)

UNCOND = re.compile(r"^(blr|bctr|b 0x[0-9a-f]+)$")
ok, flagged = 0, []
for h, src in sorted(hints.items()):
    if h not in name_at:
        flagged.append((h, src, "NOT REGISTERED after codegen"))
        continue
    i = bisect.bisect_left(starts, h)
    p = starts[i - 1]
    ins, labels, has_switch = info.get(name_at[p], ([], [], False))
    p_end = p + 4 * len(ins)
    last = ins[-1] if ins else ""
    max_label = max(labels) if labels else 0
    if p_end == h and UNCOND.match(last):
        ok += 1
    elif has_switch and max_label < h and UNCOND.match(last):
        ok += 1  # inline jump table makes the instruction count short; last instruction still unconditional
    else:
        why = []
        if p_end != h:
            why.append(f"predecessor {name_at[p]} ends 0x{p_end:08X} (gap 0x{h - p_end:X})" if p_end < h else
                       f"predecessor {name_at[p]} OVERLAPS (ends 0x{p_end:08X})")
        if not UNCOND.match(last):
            why.append(f"predecessor last instr '{last}'")
        flagged.append((h, src, "; ".join(why)))

print(f"{len(hints)} hints checked: {ok} clean, {len(flagged)} flagged")
for h, src, why in flagged:
    print(f"  0x{h:08X} [{src}] {why}")
