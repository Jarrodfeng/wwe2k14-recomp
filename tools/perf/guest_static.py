"""What the compiled guest code is made of, weighted by where the profile says the CPU goes (Session 95). For every guest
function in the map: instructions, and how many of them are the memory-access tax (the REX_PHYS_HOST_OFFSET check: xor,
cmp $0xE0000000, setae, shl $12), byte swaps, vector / scalar float, MXCSR writes, calls. Weighted by the samples of
guest_cpu.py --json (each function's own samples spread evenly over its instructions: an estimate, not a measured count).

    py tools\\perf\\guest_static.py EXE MAP [--json scene.json ...] [--top 30] [--out functions.json]
llvm-objdump (C:\\Program Files\\LLVM\\bin) reads the whole exe once (~1 min).
"""
import argparse
import bisect
import json
import re
import subprocess
from collections import Counter

OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
SYM = re.compile(r'^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+(.*)$')
NAME = re.compile(r'^(?:__imp__)?(sub_[0-9A-F]{8})$')
VEC = ('mulps', 'addps', 'subps', 'divps', 'maxps', 'minps', 'shufps', 'pshufb', 'pshufd', 'rcpps', 'rsqrtps', 'sqrtps',
       'cmpeqps', 'cmpleps', 'cmpltps', 'blendps', 'dpps', 'cvtdq2ps', 'cvttps2dq', 'unpcklps', 'unpckhps', 'insertps',
       'vfmadd', 'vmulps', 'vaddps', 'vsubps', 'vpshufb', 'vshufps')
SCALAR = ('addsd', 'subsd', 'mulsd', 'divsd', 'sqrtsd', 'cvtss2sd', 'cvtsd2ss', 'addss', 'subss', 'mulss', 'divss',
          'ucomisd', 'ucomiss', 'cvttsd2si', 'cvtsi2sd')


def functions(path):
    """[(start address, name)] for every symbol of the code section, sorted; guest names keep sub_XXXXXXXX."""
    syms = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYM.match(line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1), m.group(3)))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('map')
    ap.add_argument('--json', action='append', default=[])
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    syms = functions(a.map)
    starts = [s[0] for s in syms]
    guest = {}
    per = {}
    for i, (addr, name, obj) in enumerate(syms):
        m = NAME.match(name)
        if m and 'recomp' in obj:
            guest[i] = m.group(1)
    p = subprocess.Popen([OBJDUMP, '-d', '--no-show-raw-insn', a.exe], stdout=subprocess.PIPE, text=True, bufsize=1 << 20,
                         errors='replace')
    idx = -1
    nxt = 0
    cur = None
    for line in p.stdout:
        if len(line) < 12 or line[0] not in '0123456789abcdef' and line[1] not in '0123456789abcdef':
            continue
        c = line.find(':')
        if c < 8:
            continue
        try:
            addr = int(line[:c].strip(), 16)
        except ValueError:
            continue
        if addr >= nxt:
            i = bisect.bisect_right(starts, addr) - 1
            nxt = starts[i + 1] if i + 1 < len(starts) else 1 << 62
            g = guest.get(i)
            cur = per.setdefault(g, Counter()) if g else None
        if cur is None:
            continue
        parts = line[c + 1:].split()
        if not parts:
            continue
        op = parts[0]
        rest = line[c + 1:]
        cur['insns'] += 1
        if op == 'cmpl' and ('0xe0000000' in rest or '0xdfffffff' in rest):
            cur['e0'] += 1
        elif op.startswith('bswap'):
            cur['bswap'] += 1
        elif op == 'ldmxcsr':
            cur['ldmxcsr'] += 1
        elif op == 'stmxcsr':
            cur['stmxcsr'] += 1
        elif op.startswith('call'):
            cur['call'] += 1
        elif op in VEC:
            cur['vec'] += 1
        elif op in SCALAR:
            cur['scalar'] += 1
    p.wait()
    total = Counter()
    for c in per.values():
        total.update(c)
    print('static, all guest functions:', dict(total))
    print(f"  E0 check = {100.0 * 4 * total['e0'] / total['insns']:.1f}% of instructions (4 per access), "
          f"bswap {100.0 * total['bswap'] / total['insns']:.1f}%")
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            json.dump(per, f)
    for path in a.json:
        scene = json.load(open(path, encoding='utf-8'))
        w = Counter()
        n = 0
        for fn, samples in scene['self'].items():
            c = per.get(fn)
            if not c or not c['insns']:
                continue
            n += samples
            for k, v in c.items():
                w[k] += samples * v / c['insns']
        print(f'\n{path}: guest self samples {n} of {scene["total_busy"]} busy; profile-weighted share of guest instructions:')
        print(f"  E0 check {100.0 * 4 * w['e0'] / n:.1f}%  bswap {100.0 * w['bswap'] / n:.1f}%  vector math {100.0 * w['vec'] / n:.1f}%  "
              f"scalar float {100.0 * w['scalar'] / n:.1f}%  calls {100.0 * w['call'] / n:.1f}%  ldmxcsr {100.0 * w['ldmxcsr'] / n:.2f}%")
        print(f'  {"self":>5} {"insns":>6} {"e0":>5} {"bswap":>5} {"vec":>5} {"scal":>5} {"call":>4} {"ldmx":>4}  function')
        for fn, samples in sorted(scene['self'].items(), key=lambda x: -x[1])[:a.top]:
            c = per.get(fn, Counter())
            print(f'  {samples:5d} {c["insns"]:6d} {c["e0"]:5d} {c["bswap"]:5d} {c["vec"]:5d} {c["scalar"]:5d} {c["call"]:4d} {c["ldmxcsr"]:4d}  {fn}')


if __name__ == '__main__':
    main()
