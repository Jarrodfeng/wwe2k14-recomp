"""List a recompiled function's PPC instructions with their guest addresses, from the PPC comments in the
local generated code (generated/default, never committed). Labels (loc_XXXXXXXX) are checked against the
computed addresses, so a mismatch is reported instead of silently giving wrong addresses.

Usage: py tools/analysis/list_function.py 82CE3868 [max_lines]
"""
import glob
import re
import sys

name = 'sub_' + sys.argv[1].upper().removeprefix('0X').removeprefix('SUB_')
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
start = int(name[4:], 16)

for path in glob.glob(r'generated\default\wwe2k14_recomp.*.cpp'):
    text = open(path, encoding='utf-8').read()
    marker = f'DEFINE_REX_FUNC({name})'
    i = text.find(marker)
    if i < 0:
        continue
    body = text[i:text.find('\n}\n', i)]
    address, shown, bad_labels = start, 0, 0
    for line in body.splitlines()[1:]:
        stripped = line.strip()
        label = re.match(r'loc_([0-9A-F]{8}):', stripped)
        if label:
            if int(label.group(1), 16) != address:
                # Usually an inline jump table (data words without PPC comments). Labels are authoritative.
                bad_labels += 1
                print(f'!! label {label.group(0)} but computed 0x{address:08X}; re-syncing to the label')
                address = int(label.group(1), 16)
            if shown < limit:
                print(f'           {label.group(0)}')
            continue
        if stripped.startswith('// '):
            if shown < limit:
                print(f'0x{address:08X}  {stripped[3:]}')
                shown += 1
            address += 4
    print(f'-- {name}: {(address - start) // 4} instructions, ends 0x{address:08X}, label mismatches: {bad_labels}')
    break
else:
    sys.exit(f'{name} not found in generated code')
