"""Session 73: classify run_fuzz.ps1's x64 / arm64 differences. A line differs either only in NaN bit patterns (both
sides a NaN at every differing word: sign / payload, harmless for the game) or really (a number on one side). Prints
the counts per test and the real differences."""
import collections
import os

OUT = r'C:\Users\Jarrod\wwe2k14-recomp\out\android\cpu_semantics\fuzz'


def nan(w):
    v = int(w, 16)
    if len(w) == 8:
        return (v & 0x7F800000) == 0x7F800000 and (v & 0x007FFFFF) != 0
    if len(w) == 16:
        return (v & 0x7FF0000000000000) == 0x7FF0000000000000 and (v & 0x000FFFFFFFFFFFFF) != 0
    return False


a = open(os.path.join(OUT, 'x64.txt')).read().splitlines()
b = open(os.path.join(OUT, 'arm64.txt')).read().splitlines()
nan_only = collections.Counter()
real = collections.defaultdict(list)
for la, lb in zip(a, b):
    if la == lb:
        continue
    ta, tb = la.split(), lb.split()
    name = ta[0]
    words = [(x, y) for x, y in zip(ta[2:], tb[2:]) if x != y]
    if all(nan(x) and nan(y) for x, y in words):
        nan_only[name] += 1
    else:
        real[name].append((la, lb, [(x, y) for x, y in words if not (nan(x) and nan(y))]))
print('NaN bits only:', sum(nan_only.values()), 'lines in', len(nan_only), 'tests')
print('real differences:', {k: len(v) for k, v in sorted(real.items())})
for k, v in sorted(real.items()):
    print('==', k)
    for la, lb, w in v[:4]:
        print('  x64  :', la)
        print('  arm64:', lb)
        print('  words:', w)
