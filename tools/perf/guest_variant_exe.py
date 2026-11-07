"""Build a game exe whose chosen generated guest-code files are compiled with other flags (Session 95, guest codegen A/B).
Answers: "what does compile target X / PGO do to the recompiled game code's speed, build time, exe size and link time?",
without codegen and without touching out\\build\\win-amd64-release. Reads the main build's build.ninja (the exact defines, includes
and flags of the recompiled files), changes only the flags below, compiles the chosen generated\\default\\wwe2k14_recomp.N.cpp
files (+ a variant precompiled header) into <out>\\obj and links <out>\\exe\\wwe2k14.exe with the main build's other objects,
exactly like tools\\owned\\fast_game_exe.ps1 does for main.cpp. Generated code is read in place and never copied or committed (rule 1).
Run it under the BUILD slot of the run lock:
    pwsh -NoProfile -File tools\\runlock.ps1 run -Build -Name NAME -TimeoutMinutes 60 -- py tools\\perf\\guest_variant_exe.py --name v3 --variant v3 --tus-file out\\s95\\gcpu\\tus.txt --jobs 8

  --variant   base (the main flags again: the control), v3 (AVX2 BMI BMI2 FMA MOVBE LZCNT POPCNT + -ffp-contract=off), v3nc (v3 without
              the contract flag: shows the flag is needed), pgogen / pgouse (clang IR-free front-end PGO, needs --profdata for use;
              add +v3 to combine: pgogen+v3, pgouse+v3)
  --tus / --tus-file   comma list of generated file numbers; 'all' = every generated recomp file
  --out       default out\\s95\\cg\\<name> (outside any worktree; the stack variant writes rewritten generated C++ to <out>\\src: gitignored under out\\, never commit it)
  --wt        a worktree: src\\main.cpp and our headers come from it, compiled with their own PCH (flags from the main build)
  --overlay   a folder searched before every other include folder of both targets (a changed SDK header ahead of the installed SDK)
  --dlls      rex DLLs from this folder (a fast_build stage folder) instead of the build folder's
  --runtime-lib  this rexruntime.lib instead of the build folder's (game code that uses something new in an SDK worktree's rexruntime; give that worktree's DLLs with --dlls)
  --build-dir the build folder to read and link from (default out\\build\\win-amd64-release; the release uses out\\release\\build)
  --exe-name  the exe's file name in <out>\\exe (default wwe2k14.exe; the launcher's choice for x86-64-v3 is wwe2k14_v3.exe)
  --keep-guest  reuse the guest objects already in <out>\\obj: only --wt's main.cpp compiles, then the link (a change in our code only)
  plain (a variant part): the generated code's REX_PHYS_HOST_OFFSET is 0 (Session 95, flat guest memory), for generated code from
              before the SDK template change; the exe needs the flat-memory SDK's DLLs (--dlls) and xmemory.h (--overlay)
Prints per-step wall times, object sizes, exe size. Last line: VARIANT_EXE <path>.
"""
import argparse
import concurrent.futures as cf
import glob
import os
import re
import shutil
import subprocess
import sys
import time

R = r'C:\Users\Jarrod\wwe2k14-recomp'
B = os.path.join(R, 'out', 'build', 'win-amd64-release')
CLANG = r'C:\PROGRA~1\LLVM\bin\CLANG_~1.EXE'
V3 = ['-mavx2', '-mfma', '-mbmi', '-mbmi2', '-mmovbe', '-mlzcnt', '-mpopcnt']
FLAGSETS = {
    'base': [],
    'v3': V3 + ['-ffp-contract=off'],
    'v3nc': V3,
    # clang front-end instrumentation; the profile is keyed by function name + CFG hash, so it survives other edits
    'pgogen': ['-fprofile-instr-generate'],
    # r1-relative accesses (the guest stack, 0x70000000-0x7F000000 by xboxkrnl_memory.cpp:653) skip the 0xE0000000 check and volatile;
    # needs the source rewrite below and the macros in the PCH. Plain accesses through char-typed memory of other types: no TBAA.
    'stack': ['-fno-strict-aliasing'],
    # Session 95: the PCH gets PLAIN_H below; no flag of its own
    'plain': [],
}
PLAIN_H = '#undef REX_PHYS_HOST_OFFSET\n#define REX_PHYS_HOST_OFFSET(addr) 0u\n'
STACK_H = ''.join(f'#define REX_SLOAD_U{b}(x) {s}(*(u{b}*)(base + (u32)(x)))\n'
                  f'#define REX_SSTORE_U{b}(x, y) (*(u{b}*)(base + (u32)(x)) = {s}(y))\n'
                  for b, s in ((8, ''), (16, '__builtin_bswap16'), (32, '__builtin_bswap32'), (64, '__builtin_bswap64')))
# constant offsets only: an indexed ctx.r1.u32 + ctx.rN.u32 is not provably on the stack. Unbuilt/unrun variant: dropping volatile may let LLVM
# hoist a load out of a guest spin loop on a stack flag another thread sets, so a HANG is a possible result of it.
ACCESS = re.compile(r'REX_(LOAD|STORE)_U(8|16|32|64)\(ctx\.r1\.u32(?=(?: \+ \d+)?\s*[,)])')


def statement(lines, prefix):
    i = 0
    while i < len(lines) and not lines[i].startswith(prefix):
        i += 1
    if i >= len(lines):
        raise SystemExit(f'no {prefix!r} in build.ninja')
    v = {'_line': lines[i]}
    j = i + 1
    while j < len(lines) and lines[j].startswith('  '):
        k = lines[j].find(' = ')
        if k > 0:
            v[lines[j][:k].strip()] = lines[j][k + 3:]
        j += 1
    return v


def flags_for(variant, profdata):
    out = []
    for part in variant.split('+'):
        if part == 'pgouse':
            if not profdata:
                raise SystemExit('pgouse needs --profdata')
            out += [f'-fprofile-instr-use={profdata}', '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-missing']
        elif part in FLAGSETS:
            out += FLAGSETS[part]
        else:
            raise SystemExit(f'unknown variant part {part}')
    return out


def run(cmd, rsp):
    with open(rsp, 'w') as f:
        f.write(cmd)
    t = time.time()
    r = subprocess.run([CLANG, '@' + rsp], capture_output=True, text=True, errors='replace')
    return r.returncode, time.time() - t, (r.stderr or '')[-700:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--variant', default='v3')
    ap.add_argument('--tus', default='')
    ap.add_argument('--tus-file', default='')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--profdata', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--no-link', action='store_true')
    ap.add_argument('--keep-pch', action='store_true')
    ap.add_argument('--wt', default='')
    ap.add_argument('--overlay', default='')
    ap.add_argument('--dlls', default='')
    ap.add_argument('--runtime-lib', default='')
    ap.add_argument('--rex-app', default='')  # (Session 95: the SDK's share source, built into the exe, not a DLL: this rex_app.cpp instead of the main build's object; needs --wt)
    ap.add_argument('--build-dir', default='')
    ap.add_argument('--exe-name', default='wwe2k14.exe')
    ap.add_argument('--keep-guest', action='store_true')
    a = ap.parse_args()
    global B
    if a.build_dir:
        B = os.path.abspath(a.build_dir)
    out = a.out or os.path.join(R, 'out', 's95', 'cg', a.name)
    obj = os.path.join(out, 'obj')
    exe = os.path.join(out, 'exe')
    os.makedirs(obj, exist_ok=True)
    os.makedirs(exe, exist_ok=True)
    fwd = lambda p: p.replace('\\', '/')
    tus = a.tus
    if a.tus_file:
        tus = open(a.tus_file).read().strip()
    allnums = sorted(int(re.search(r'recomp\.(\d+)\.cpp$', p).group(1)) for p in glob.glob(os.path.join(R, 'generated', 'default', 'wwe2k14_recomp.*.cpp')))
    nums = allnums if tus == 'all' else [int(x) for x in tus.split(',') if x.strip()]
    prof = fwd(os.path.abspath(a.profdata)) if a.profdata else ''
    extra = flags_for(a.variant, prof)

    ninja = open(os.path.join(B, 'build.ninja'), encoding='utf-8').read().splitlines()
    pch = statement(ninja, 'build CMakeFiles/wwe2k14_recomp.dir/cmake_pch.hxx.pch:')
    cc = statement(ninja, 'build CMakeFiles/wwe2k14_recomp.dir/generated/default/wwe2k14_recomp.26.cpp.obj:')
    ln = statement(ninja, 'build wwe2k14.exe:')
    mainhxx = fwd(os.path.join(B, 'CMakeFiles', 'wwe2k14_recomp.dir', 'cmake_pch.hxx'))

    def swap(flags):
        flags = flags.replace('-msse4.1', '-msse4.1 ' + ' '.join(extra), 1) if extra else flags
        return flags.replace(mainhxx, fwd(os.path.join(out, 'pch.hxx')))

    # the variant PCH: the same header the main build's PCH holds, compiled with the variant flags
    hxx = os.path.join(out, 'pch.hxx')
    shutil.copyfile(os.path.join(B, 'CMakeFiles', 'wwe2k14_recomp.dir', 'cmake_pch.hxx'), hxx)
    shutil.copyfile(os.path.join(B, 'CMakeFiles', 'wwe2k14_recomp.dir', 'cmake_pch.hxx.cxx'), os.path.join(out, 'pch.hxx.cxx'))
    stack = 'stack' in a.variant.split('+')
    plain = 'plain' in a.variant.split('+')
    if stack or plain:
        with open(hxx, 'a') as f:
            f.write('\n' + (STACK_H if stack else '') + (PLAIN_H if plain else ''))
    over = f'-I{fwd(os.path.abspath(a.overlay))} ' if a.overlay else ''
    pchfile = os.path.join(out, 'pch.hxx.pch')
    t0 = time.time()
    pflags = swap(pch['FLAGS'])
    if a.keep_guest:
        rc = 0
    else:
        rc, dt, err = run(' '.join([pch['DEFINES'], over + pch['INCLUDES'], pflags, '-o', f'"{fwd(pchfile)}"', '-c',
                                    f'"{fwd(os.path.join(out, "pch.hxx.cxx"))}"']), os.path.join(out, 'pch.rsp'))
        print(f'pch: rc {rc} {dt:.1f} s  extra flags: {" ".join(extra) or "(none)"}')
    if rc:
        print(err)
        return 3
    cflags = swap(cc['FLAGS']).replace(fwd(os.path.join(B, 'CMakeFiles', 'wwe2k14_recomp.dir', 'cmake_pch.hxx.pch')), fwd(pchfile))

    def one(n):
        o = os.path.join(obj, f'recomp.{n}.obj')
        if a.keep_guest:
            return n, 0, 0.0, '', os.path.getsize(o)
        src = os.path.join(R, 'generated', 'default', f'wwe2k14_recomp.{n}.cpp')
        if stack:
            os.makedirs(os.path.join(out, 'src'), exist_ok=True)
            text = open(src, encoding='utf-8', errors='replace').read()
            src = os.path.join(out, 'src', f'wwe2k14_recomp.{n}.cpp')
            with open(src, 'w', encoding='utf-8') as f:
                f.write(ACCESS.sub(lambda m: f'REX_S{m.group(1)}_U{m.group(2)}(ctx.r1.u32', text))
        src = fwd(src)
        rc, dt, err = run(' '.join([cc['DEFINES'], over + cc['INCLUDES'], cflags, '-o', f'"{fwd(o)}"', '-c', f'"{src}"']),
                          os.path.join(obj, f'recomp.{n}.rsp'))
        return n, rc, dt, err, (os.path.getsize(o) if rc == 0 else 0)

    t1 = time.time()
    done = 0
    sizes = {}
    cpu = 0.0
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for n, rc, dt, err, sz in ex.map(one, nums):
            done += 1
            cpu += dt
            sizes[n] = sz
            if rc:
                print(f'FAIL tu {n}: {err}')
                return 3
            if done % 5 == 0 or done == len(nums):
                print(f'  {done}/{len(nums)} compiled  {time.time() - t1:.0f} s elapsed', flush=True)
    wall = time.time() - t1
    print(f'compile: {len(nums)} files, wall {wall:.0f} s, summed compile time {cpu:.0f} s ({cpu / max(1, len(nums)):.0f} s/file), '
          f'objects {sum(sizes.values()) / 1e6:.0f} MB')
    # the main build's objects of the same files, for the size comparison
    ref = sum(os.path.getsize(os.path.join(B, 'CMakeFiles', 'wwe2k14_recomp.dir', 'generated', 'default', f'wwe2k14_recomp.{n}.cpp.obj')) for n in nums)
    print(f'main build objects of the same files: {ref / 1e6:.0f} MB')
    if a.no_link:
        return 0
    extra_objs = []
    main_obj = ''
    if a.wt:
        # src\main.cpp from the worktree, with a PCH of its own: the plain macro and --overlay reach our hooks too (Session 95)
        wt = fwd(os.path.abspath(a.wt))
        mp = statement(ninja, 'build CMakeFiles/wwe2k14.dir/cmake_pch.hxx.pch:')
        mc = statement(ninja, 'build CMakeFiles/wwe2k14.dir/src/main.cpp.obj:')
        mdir = os.path.join(B, 'CMakeFiles', 'wwe2k14.dir')
        mhxx_old = fwd(os.path.join(mdir, 'cmake_pch.hxx'))
        mhxx = os.path.join(out, 'main_pch.hxx')
        text = open(os.path.join(mdir, 'cmake_pch.hxx')).read().replace(fwd(R) + '/src/pch_main.h', wt + '/src/pch_main.h')
        with open(mhxx, 'w') as f:
            f.write(text + '\n' + (PLAIN_H if plain else ''))
        shutil.copyfile(os.path.join(mdir, 'cmake_pch.hxx.cxx'), os.path.join(out, 'main_pch.hxx.cxx'))
        minc = f'{over}-I{wt} -I{wt}/src ' + mc['INCLUDES']
        mpch = fwd(os.path.join(out, 'main_pch.hxx.pch'))
        t = time.time()
        rc, dt, err = run(' '.join([mp['DEFINES'], minc, mp['FLAGS'].replace(mhxx_old, fwd(mhxx)), '-o', f'"{mpch}"', '-c',
                                    f'"{fwd(os.path.join(out, "main_pch.hxx.cxx"))}"']), os.path.join(out, 'main_pch.rsp'))
        print(f'main pch: rc {rc} {dt:.1f} s')
        if rc:
            print(err)
            return 3
        mflags = mc['FLAGS'].replace(fwd(os.path.join(mdir, 'cmake_pch.hxx.pch')), mpch).replace(mhxx_old, fwd(mhxx))
        main_obj = fwd(os.path.join(obj, 'main.cpp.obj'))
        rc, dt, err = run(' '.join([mc['DEFINES'], minc, mflags, '-o', f'"{main_obj}"', '-c', f'"{wt}/src/main.cpp"']),
                          os.path.join(obj, 'main.rsp'))
        print(f'main.cpp: rc {rc} {dt:.1f} s')
        if rc:
            print(err)
            return 3
        rex_app_obj = ''
        rex_app_key = 'CMakeFiles/wwe2k14.dir/thirdparty/rexglue-sdk-vulkan/share/rexglue/rex_app.cpp.obj'
        if a.rex_app:
            rc_ = statement(ninja, f'build {rex_app_key}:')
            rex_app_obj = fwd(os.path.join(obj, 'rex_app.cpp.obj'))
            rflags = rc_['FLAGS'].replace(fwd(os.path.join(mdir, 'cmake_pch.hxx.pch')), mpch).replace(mhxx_old, fwd(mhxx))
            rc, dt, err = run(' '.join([rc_['DEFINES'], minc, rflags, '-o', f'"{rex_app_obj}"', '-c', f'"{fwd(os.path.abspath(a.rex_app))}"']),
                              os.path.join(obj, 'rex_app.rsp'))
            print(f'rex_app.cpp: rc {rc} {dt:.1f} s')
            if rc:
                print(err)
                return 3
    if 'pgogen' in a.variant:
        hook = os.path.join(out, 'pgo_exit_hook.obj')
        rc, dt, err = run(' '.join(['-O1', '-c', f'"{fwd(os.path.join(os.path.dirname(os.path.abspath(__file__)), "pgo_exit_hook.cpp"))}"',
                                    '-o', f'"{fwd(hook)}"']), os.path.join(out, 'hook.rsp'))
        if rc:
            print(err)
            return 3
        extra_objs.append(fwd(hook))

    inputs = (ln['_line'].split(':', 1)[1].strip().split(' | ')[0])
    items = [x for x in inputs.split() if x and x != 'CXX_EXECUTABLE_LINKER__wwe2k14_Release']
    mine = {f'CMakeFiles/wwe2k14_recomp.dir/generated/default/wwe2k14_recomp.{n}.cpp.obj': os.path.join(obj, f'recomp.{n}.obj') for n in nums}
    if main_obj:
        mine['CMakeFiles/wwe2k14.dir/src/main.cpp.obj'] = main_obj
        if rex_app_obj:
            mine[rex_app_key] = rex_app_obj
    objs = []
    for it in items:
        if it in mine:
            objs.append(fwd(mine[it]))
        elif re.match(r'^[A-Za-z]\$:', it):
            objs.append(it.replace('$:', ':'))
        else:
            objs.append(f'{fwd(B)}/{it}')
    lrsp = os.path.join(out, 'link.rsp')
    with open(lrsp, 'w') as f:
        libs = re.sub(r'\S*[/\\]rexruntime\.lib', a.runtime_lib.replace('\\', '/'), ln['LINK_LIBRARIES']) if a.runtime_lib else ln['LINK_LIBRARIES']
        f.write(' '.join(objs + extra_objs) + ' ' + ln.get('LINK_PATH', '') + ' ' + libs)
    lflags = ln['FLAGS'].split()
    mapold = f'/MAP:{fwd(B)}/wwe2k14.map'
    linkflags = ln['LINK_FLAGS'].replace(mapold, f'/MAP:{fwd(exe)}/{os.path.splitext(a.exe_name)[0]}.map').split()
    if 'pgogen' in a.variant:
        lflags += ['-fprofile-instr-generate']
    t2 = time.time()
    r = subprocess.run([CLANG, '-nostartfiles', '-nostdlib'] + lflags + linkflags + ['@' + lrsp, '-o', os.path.join(exe, a.exe_name),
                       '-Xlinker', '/MANIFEST:EMBED', '-Xlinker', f'/implib:{os.path.join(out, "wwe2k14.lib")}',
                       '-Xlinker', f'/pdb:{os.path.join(out, "wwe2k14.pdb")}', '-Xlinker', '/version:0.0'],
                       capture_output=True, text=True, errors='replace')
    if r.returncode:
        print((r.stderr or '')[-1500:])
        return 4
    print(f'link: {time.time() - t2:.0f} s')
    dlls = a.dlls or B
    for f in glob.glob(os.path.join(dlls, 'rex*.dll')) + glob.glob(os.path.join(dlls, 'rex*.pdb')):
        shutil.copy2(f, exe)
    for n in ('amd_fidelityfx_vk.dll', 'wwe2k14_launcher.exe', 'shader_list.txt'):
        p = os.path.join(B, n)
        if os.path.exists(p):
            shutil.copy2(p, exe)
    e = os.path.join(exe, a.exe_name)
    print(f'exe size {os.path.getsize(e) / 1e6:.1f} MB (main build exe {os.path.getsize(os.path.join(B, "wwe2k14.exe")) / 1e6:.1f} MB)')
    print(f'total wall {time.time() - t0:.0f} s')
    print(f'VARIANT_EXE {e}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
