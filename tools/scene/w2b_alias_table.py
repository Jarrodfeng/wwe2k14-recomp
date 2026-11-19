"""S103 W2B: the node-name alias table of sub_82BD24C8 (the game's "find the owner's node by name"), decoded from the player's own image.
Answers: which (from, to) name pairs does the game's name lookup apply before it gives -1 (docs\\agents\\s103\\W2_ADV.md row 1b: 42 pairs at 0x8392DDC0, stride 32, to "null" = -1)?
How: a symbolic pass over the straight-line listing of sub_82BD24C8 (the local string-address stores, then the table fill: `bl sub_82C35118(dest, src)` x 81), then the strings are read from out\\harness\\index\\image.bin.
Run from the MAIN checkout (where generated\\ and out\\ live), read-only on the game's files, writes only to stdout:
  py tools\\scene\\w2b_alias_table.py            -> the pairs, one per line, and a C++ initializer
  py tools\\scene\\w2b_alias_table.py --listing FILE   (a saved `py tools\\analysis\\list_function.py 82BD24C8`)
Nothing from the game is committed: tools\\scene\\w2b_alias_check.py compares this output with src\\assets\\node_alias.h. The tool exits 1 and prints '?' when any slot is unresolved (never a silent 'null').
"""
import re
import struct
import subprocess
import sys

IMG = r'out/harness/index/image.bin'
BASE = 0x82000000


def listing(path):
    if path:
        return open(path, encoding='utf-8').read().splitlines()
    return subprocess.run([sys.executable, 'tools/analysis/list_function.py', '82BD24C8'], capture_output=True, text=True, check=True).stdout.splitlines()


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def main():
    path = sys.argv[sys.argv.index('--listing') + 1] if '--listing' in sys.argv else None
    reg = {}
    stack = {}
    stack64 = {}     # 64-bit std / ld on (r1): a register saved across the table fill (r17 = "Bip R Forearm" for r_kote)
    stores = {}      # std rS,off(rB) of a loaded 8-byte memory chunk: dest address -> source address (a 16-byte string copy is two of them)
    calls = []
    for line in listing(path):
        m = re.match(r'0x([0-9A-F]{8})\s+(\w+)\s*(.*)', line.strip())
        if not m:
            continue
        addr, op, args = int(m.group(1), 16), m.group(2), [a.strip() for a in m.group(3).split(',')]
        def R(x):
            return int(x[1:]) if re.fullmatch(r'r\d+', x) else None
        if op == 'lis':
            reg[R(args[0])] = s32(int(args[1], 0) << 16) & 0xFFFFFFFF
        elif op == 'addi':
            b = R(args[1])
            reg[R(args[0])] = ((reg.get(b) if b else 0) + int(args[2], 0)) & 0xFFFFFFFF if (b is None or b == 0 or reg.get(b) is not None) else None
        elif op == 'li':
            reg[R(args[0])] = int(args[1], 0) & 0xFFFFFFFF
        elif op == 'ori':
            reg[R(args[0])] = (reg.get(R(args[1])) | int(args[2], 0)) & 0xFFFFFFFF if reg.get(R(args[1])) is not None else None
        elif op == 'mr':
            reg[R(args[0])] = reg.get(R(args[1]))
        elif op in ('stw', 'lwz') and args[1].endswith('(r1)'):
            off = int(args[1].split('(')[0])
            if op == 'stw':
                stack[off] = reg.get(R(args[0]))
            else:
                reg[R(args[0])] = stack.get(off)
        elif op == 'bl' and args[0].lower() == '0x82c35118':
            calls.append((addr, reg.get(3), reg.get(4)))
            reg[3] = None
        elif op in ('std', 'ld') and args[1].endswith('(r1)'):
            off = int(args[1].split('(')[0])
            if op == 'std':
                stack64[off] = reg.get(R(args[0]))
            else:
                reg[R(args[0])] = stack64.get(off)
        elif op == 'ld':   # ld rD,off(rB): a chunk of memory at a known address (the "null" buffer copies); a token that the next std carries
            base = R(args[1].split('(')[1].rstrip(')'))
            off = int(args[1].split('(')[0])
            bv = reg.get(base)
            reg[R(args[0])] = ('mem', bv + off) if isinstance(bv, int) else None
        elif op == 'std':
            base = R(args[1].split('(')[1].rstrip(')'))
            off = int(args[1].split('(')[0])
            bv, sv = reg.get(base), reg.get(R(args[0]))
            if isinstance(bv, int) and isinstance(sv, tuple):
                stores[(bv + off) & 0xFFFFFFFF] = sv[1]
        elif op == 'rotlwi' and args[2] == '0':
            reg[R(args[0])] = reg.get(R(args[1]))
        elif op in ('lwz', 'ld', 'lbz', 'add', 'rlwinm', 'rotlwi', 'clrlwi', 'extsb', 'sub'):
            reg[R(args[0])] = None
        elif op == 'bl' or op == 'bctrl':
            for k in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                reg[k] = None
    if "--debug" in sys.argv:
        for c in calls[-8:] + calls[:2]: print([hex(x) if x else x for x in c])
    img = open(IMG, 'rb').read()

    def cstr(a):
        o = a - BASE
        e = img.index(b'\0', o)
        return img[o:e].decode('latin-1')

    pairs = {}
    unresolved = []
    for addr, dst, src in calls:
        if dst is None or not isinstance(dst, int) or not (0x8392DDC0 <= dst < 0x8392DDC0 + 32 * 84):
            continue
        slot = (dst - 0x8392DDC0) // 32
        half = ((dst - 0x8392DDC0) % 32) // 16
        pairs.setdefault(slot, [None, None])[half] = cstr(src) if isinstance(src, int) else '?'
    # the 16-byte copies done with ld / std pairs (not through sub_82C35118): a copy of memory at SRC; src == the "null" buffer 0x8392DDB0 is the string "null" (its first call, 0x82BD2A68, copies "null" there)
    for d in sorted(stores):
        if d + 8 in stores and stores[d + 8] == stores[d] + 8 and 0x8392DDC0 <= d < 0x8392DDC0 + 32 * 84:
            slot, half = (d - 0x8392DDC0) // 32, ((d - 0x8392DDC0) % 32) // 16
            src = stores[d]
            pairs.setdefault(slot, [None, None])[half] = 'null' if src == 0x8392DDB0 else '?'
    for slot in range(42):
        for half in (0, 1):
            if pairs.get(slot, [None, None])[half] in (None, '?'):
                unresolved.append((slot, half))
    if unresolved:
        print('// UNRESOLVED slots (slot, half):', unresolved)
        for slot in sorted(pairs):
            print(f'  {{"{pairs[slot][0] or "?"}", "{pairs[slot][1] or "?"}"}},  // slot {slot}')
        sys.exit(1)
    print(f'// source: sub_82BD24C8 table fill 0x82BD2A80-0x82BD2E88, table 0x8392DDC0, stride 32 (from 16 bytes, to 16 bytes); {len(pairs)} pairs decoded from {len(calls)} copies + {len(stores) // 2} ld/std copies of the null buffer')
    for slot in sorted(pairs):
        print(f'  {{"{pairs[slot][0]}", "{pairs[slot][1]}"}},  // slot {slot}')


main()
