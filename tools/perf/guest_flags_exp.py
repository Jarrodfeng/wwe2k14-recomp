"""What compile flags and access macros would do to the generated code's instruction count (Session 95). Compiles a few
generated files with the main build's flags and with each variant into out\\s95\\gcpu\\exp (gitignored; never the main build
folder), counts instructions per guest function and prints totals, weighted by the profile (guest_cpu.py --json).
Nothing runs; this is static. Generated code stays under out\\ and generated\\ only (rule 1).

    pwsh -NoProfile -File tools\\runlock.ps1 run -Build -Name gcpu_exp -TimeoutMinutes 15 -- py tools\\perf\\guest_flags_exp.py --tus 203,89,26 --json out\\s95\\gcpu\\match.json --json out\\s95\\gcpu\\bryan.json
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import subprocess
from collections import Counter

R = r'C:\Users\Jarrod\wwe2k14-recomp'
EXP = os.path.join(R, 'out', 's95', 'gcpu', 'exp')
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
BASE = ['-O3', '-DNDEBUG', '-std=gnu++23', '-D_DLL', '-D_MT', '-Xclang', '--dependent-lib=msvcrt', '-DNOMINMAX',
        '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN',
        f'-I{R}', f'-I{R}/src', f'-I{R}/generated/default', '-isystem', f'{R}/thirdparty/rexglue-sdk-vulkan/include',
        '-isystem', f'{R}/thirdparty/rexglue-sdk-vulkan/include/dxc', '-include', f'{R}/generated/default/wwe2k14_pch.h']
SSE = ['-msse4.1']
EHA = ['-fasync-exceptions']
V3 = ['-mavx2', '-mfma', '-mbmi', '-mbmi2', '-mmovbe', '-mlzcnt', '-mpopcnt', '-ffp-contract=off']
MOVBE = ['-msse4.1', '-mmovbe']
OFF0 = '#undef REX_PHYS_HOST_OFFSET\n#define REX_PHYS_HOST_OFFSET(addr) 0u\n'
BRANCH = ('#undef REX_PHYS_HOST_OFFSET\n#define REX_PHYS_HOST_OFFSET(addr) '
          '(__builtin_expect((u32)(addr) >= 0xE0000000u, 0) ? 0x1000u : 0u)\n')
WIDTHS = ((8, ''), (16, '__builtin_bswap16'), (32, '__builtin_bswap32'), (64, '__builtin_bswap64'))
NONVOL = ''.join(f'#undef REX_LOAD_U{b}\n#define REX_LOAD_U{b}(x) {s}(*(u{b}*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))\n'
                 f'#undef REX_STORE_U{b}\n#define REX_STORE_U{b}(x, y) (*(u{b}*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = {s}(y))\n'
                 for b, s in WIDTHS)
STACKV = ''.join(f'#define REX_SLOAD_U{b}(x) {s}(*(volatile u{b}*)(base + (u32)(x)))\n'
                 f'#define REX_SSTORE_U{b}(x, y) (*(volatile u{b}*)(base + (u32)(x)) = {s}(y))\n'
                 for b, s in WIDTHS)
STACKN = STACKV.replace('volatile ', '')
# relaxed atomics: the same x86 instructions as volatile, but the optimiser may keep other values across them
RELAXED = ''.join(f'#undef REX_LOAD_U{b}\n#define REX_LOAD_U{b}(x) {s}(__atomic_load_n((u{b}*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)), __ATOMIC_RELAXED))\n'
                  f'#undef REX_STORE_U{b}\n#define REX_STORE_U{b}(x, y) __atomic_store_n((u{b}*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)), {s}(y), __ATOMIC_RELAXED)\n'
                  for b, s in WIDTHS)
# name: (flags, macro header text or None, rewrite r1-relative accesses?)
VARIANTS = {
    'base': (SSE + EHA, None, False),
    'noeha': (SSE, None, False),
    'movbe': (MOVBE + EHA, None, False),
    'v3': (V3 + EHA, None, False),
    'branch': (SSE + EHA, BRANCH, False),
    'stack': (SSE + EHA, STACKV, True),
    'stack_nv': (SSE + EHA, STACKN, True),
    'off0': (SSE + EHA, OFF0, False),
    'nonvol': (SSE + EHA, NONVOL, False),
    'off0_v3': (V3 + EHA, OFF0, False),
    'all_safe': (V3 + EHA, BRANCH + STACKN, True),
    'all_max': (V3, OFF0 + NONVOL, False),
    'relaxed': (SSE + EHA, RELAXED, False),
    'route_v3': (V3 + EHA, OFF0 + STACKN, True),  # no guest address >= 0xE0000000, stack accesses plain, AVX2 + movbe
    'route_sse': (MOVBE + EHA, OFF0 + STACKN, True),
}
ACCESS = re.compile(r'REX_(LOAD|STORE)_U(8|16|32|64)\(ctx\.r1\.u32')


def compile_one(tu, name):
    flags, header, rewrite = VARIANTS[name]
    src = os.path.join(R, 'generated', 'default', f'wwe2k14_recomp.{tu}.cpp')
    d = os.path.join(EXP, name)
    os.makedirs(d, exist_ok=True)
    args = [CLANG] + BASE + flags
    if header is not None:
        h = os.path.join(d, 'variant.h')
        with open(h, 'w') as f:
            f.write(header)
        args += ['-include', h]
    if rewrite:
        with open(src, encoding='utf-8', errors='replace') as f:
            text = f.read()
        src = os.path.join(d, f'tu{tu}.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(ACCESS.sub(lambda m: f'REX_S{m.group(1)}_U{m.group(2)}(ctx.r1.u32', text))
    obj = os.path.join(d, f'tu{tu}.obj')
    r = subprocess.run(args + ['-c', src, '-o', obj], capture_output=True, text=True, errors='replace')
    if rewrite:
        os.remove(src)
    if r.returncode:
        return name, tu, None, r.stderr[-600:]
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--no-leading-addr', obj], capture_output=True, text=True,
                         errors='replace').stdout
    per, cur = {}, None
    for line in out.splitlines():
        if line.endswith('>:'):
            m = re.match(r'^(?:[0-9a-f]+ )?<(?:__imp__)?(sub_[0-9A-F]{8})>:', line)
            cur = per.setdefault(m.group(1), Counter()) if m else None
            continue
        p = line.split()
        if cur is None or not p or p[0].endswith(':'):
            continue
        cur['n'] += 1
        op = p[0]
        if op.startswith('movbe'):
            cur['movbe'] += 1
        elif op.startswith('bswap'):
            cur['bswap'] += 1
        elif op == 'cmpl' and ('0xe0000000' in line or '0xdfffffff' in line):
            cur['e0'] += 1
    os.remove(obj)
    return name, tu, per, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tus', default='203')
    ap.add_argument('--variants', default=','.join(VARIANTS))
    ap.add_argument('--json', action='append', default=[])
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    tus = a.tus.split(',')
    names = a.variants.split(',')
    res = {n: {} for n in names}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name, tu, per, err in ex.map(lambda p: compile_one(*p), [(t, n) for n in names for t in tus]):
            if per is None:
                print(f'FAIL {name} tu{tu}: {err}')
                continue
            res[name].update(per)
    weights = []
    for p in a.json:
        with open(p, encoding='utf-8') as f:
            weights.append(json.load(f)['self'])
    base = res[names[0]]
    print(f'{"variant":<10} {"funcs":>6} {"insns":>9} {"vs base":>8} {"movbe":>7} {"bswap":>7} {"e0cmp":>7}' +
          ''.join(f' {"wtd" + str(i):>8}' for i in range(len(weights))))
    for n in names:
        per = res[n]
        common = [f for f in per if f in base]
        tot = sum(per[f]['n'] for f in common)
        b = sum(base[f]['n'] for f in common)
        line = (f'{n:<10} {len(common):6d} {tot:9d} {100.0 * tot / max(1, b) - 100:+7.1f}% '
                f'{sum(per[f]["movbe"] for f in common):7d} {sum(per[f]["bswap"] for f in common):7d} '
                f'{sum(per[f]["e0"] for f in common):7d}')
        for w in weights:
            s = sum(c for f, c in w.items() if f in per and f in base and base[f]['n'])
            v = sum(c * per[f]['n'] / base[f]['n'] for f, c in w.items() if f in per and f in base and base[f]['n'])
            line += f' {100.0 * v / max(1, s) - 100:+7.1f}%'
        print(line)
    for w in weights:
        print('samples covered by these files:', sum(c for f, c in w.items() if f in base), 'of', sum(w.values()))


if __name__ == '__main__':
    main()
