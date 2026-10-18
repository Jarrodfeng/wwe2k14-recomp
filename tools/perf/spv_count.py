"""Session 20 item 5: static instruction counts of dumped SPIR-V shaders (--dump_shaders=DIR, SDK DUMPSPV).

    py tools\\perf\\spv_count.py DIR HASH [HASH ...]

For each ucode hash, every dumped translation (shader_<HASH>_<modification>.<vk|vk_fsi|vk_fbo>.bin.<vert|frag>):
instructions inside functions, split into classes. The dumps are game-derived: they stay under out\\ (local only).
"""
import collections
import pathlib
import struct
import sys

CLASSES = {
    'load/store/access': {59, 61, 62, 63, 65, 66},            # OpVariable, Load, Store, CopyMemory, AccessChain, InBounds
    'atomic': set(range(227, 243)),
    'image sample/fetch/read/write': set(range(87, 100)),
    'branch/label/merge': {246, 247, 248, 249, 250, 251, 252, 253},
    'interlock (FSI)': {5364, 5365},                           # OpBegin / OpEndInvocationInterlockEXT
    'phi/select': {245, 169},
    'call': {57},
}


def count(path):
    words = pathlib.Path(path).read_bytes()
    n = len(words) // 4
    w = struct.unpack(f'<{n}I', words[:n * 4])
    i = 5  # header
    in_func = False
    total = 0
    by = collections.Counter()
    while i < n:
        wc, op = w[i] >> 16, w[i] & 0xFFFF
        if wc == 0:
            break
        if op == 54:  # OpFunction
            in_func = True
        elif op == 56:  # OpFunctionEnd
            in_func = False
        elif in_func:
            total += 1
            cls = next((k for k, s in CLASSES.items() if op in s), 'other (arithmetic, conversions, ...)')
            by[cls] += 1
        i += wc
    return total, by


def main():
    d = pathlib.Path(sys.argv[1])
    for h in sys.argv[2:]:
        files = sorted(d.glob(f'shader_{h.upper()}_*.bin.*'))
        if not files:
            print(f'{h}: no dump')
        for f in files:
            total, by = count(f)
            parts = ', '.join(f'{k} {v}' for k, v in sorted(by.items(), key=lambda kv: -kv[1]))
            print(f'{f.name}: {total} instructions ({parts})')


if __name__ == '__main__':
    main()
