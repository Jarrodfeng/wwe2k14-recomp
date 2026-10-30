"""Session 73: classify the x64 / arm64 differences of ppc_semantics_test (run.ps1's outputs).

For each differing line, per result word: both NaN (only the NaN's bits differ), or a real difference (a number on
one side, another number or a NaN on the other). Prints counts per instruction and every real difference.
"""
import collections
import os
import struct

OUT = r'C:\Users\Jarrod\wwe2k14-recomp\out\android\cpu_semantics'


def is_nan(word):
    if len(word) == 8:
        v = int(word, 16)
        return (v & 0x7F800000) == 0x7F800000 and (v & 0x007FFFFF) != 0
    if len(word) == 16:
        v = int(word, 16)
        return (v & 0x7FF0000000000000) == 0x7FF0000000000000 and (v & 0x000FFFFFFFFFFFFF) != 0
    return False


a = open(os.path.join(OUT, 'x64.txt')).read().splitlines()
b = open(os.path.join(OUT, 'arm64.txt')).read().splitlines()
nan_only = collections.Counter()
real = collections.defaultdict(list)
for la, lb in zip(a, b):
    if la == lb:
        continue
    name = la.split()[0] if not la.startswith('mode') else ' '.join(la.split()[:2])
    ra, rb = la.split('->')[1].split(), lb.split('->')[1].split()
    if la.split('->')[0] != lb.split('->')[0]:
        real[name].append((la, lb))
        continue
    bad = [(x, y) for x, y in zip(ra, rb) if x != y and not (is_nan(x) and is_nan(y))]
    if bad:
        real[name].append((la, lb))
    else:
        nan_only[name] += 1
print('NaN bits only (both sides NaN):', dict(nan_only))
print('real differences:', {k: len(v) for k, v in real.items()})
for k, v in real.items():
    print('==', k)
    for la, lb in v[:6]:
        print('  x64  :', la)
        print('  arm64:', lb)
