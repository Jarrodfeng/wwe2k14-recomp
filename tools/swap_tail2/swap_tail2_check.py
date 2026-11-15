#!/usr/bin/env python3
"""S102 lane R25-A gate check (offline, 0 game runs): wwe2k14_rdr_swap_tail2 bit 1 (the reader join moved from the swap thread's hook to the timing thread).

  py -I tools\\swap_tail2\\swap_tail2_check.py [--root DIR] [--syntax]

Default: builds tools\\swap_tail2\\swap_tail2_test.cpp against the SHIPPED src\\renderer_adapter\\join_ticket.h and runs it (real threads: the guest poisons a slice the
moment it sees the fence; the jobs must have read the frame's data; outputs byte-equal off vs on; the lever took; a later blocked job does not hold an item up). Then
3 planted faults (the timing thread skips the wait; the ticket is taken before the frame's job is queued; the ticket is one short) must each exit 1 with a FAIL line.
Then source lints (the hooks take the ticket, the timing thread waits before Process, the switch is per-frame and default off) each with a planted twin.
--syntax: clang -fsyntax-only of main.cpp and live.cpp with the main build flags."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']
PLANTS = [1, 2, 3, 4, 5, 6]

LINTS = [
    ('owner_timing.h: the three hooks take the ticket, the timing thread waits before it answers', 'src/owner_timing.h',
     ['const uint64_t join = JoinReaderTicket();\n  Item it;\n  it.join = join;\n  it.fence = true;', 'const uint64_t join = JoinReaderTicket();\n  Item it;\n  it.join = join;\n  it.flip = ctx.r6.u32 == dn::kFlipScheduler;',
      'const uint64_t join = JoinReaderTicket();\n  Item it;\n  it.join = join;\n  it.flip = true;', 'JoinForItem(it, env);', 'rdr_reader::g_workers.WaitBelowUntil(it.join, [&] {', 'g_core.Turn(env);',
      'rdr_reader::g_workers.WaitAll();\n  NeedsProcessor(',
      'if (!st::On(st::kJoinLater)) {\n    JoinReader();']),
    ('reader.h: the switch is read every frame; only texture jobs take tickets; the pool is the shipped template', 'src/renderer_adapter/reader.h',
     ['rdr_sw::IntSeen(rdr_sw::kSwapTail2, 0, guest_frame)', 'struct ReaderWorkers : wwe2k14::join_ticket::Pool<ReaderJob>', 'Queue(raw, true);', 'g_workers.Queue(j, false);', 'Push(job, guest_reads);',
      'REXCVAR_DEFINE_INT32(wwe2k14_rdr_swap_tail2, 0,']),
    ('rdr_switches.h: PerFrame, default off', 'src/renderer_adapter/rdr_switches.h', ['X(SwapTail2, "wwe2k14_rdr_swap_tail2", PerFrame, 0)']),
    ('owner_timing_core.h: the item carries the ticket', 'src/owner_timing_core.h', ['uint64_t join = 0;']),
    ('join_ticket.h: a ticket covers the jobs queued so far only', 'src/renderer_adapter/join_ticket.h',
     ['return MinPendingLocked() ? next : 0;', 'return !m || m >= mark;', 'job->seq = guest_reads ? next++ : 0;', 'running_seq[slot] = j->seq;', 'running_seq[slot] = 0;']),
]


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def read(root, rel):
    p = os.path.join(root, rel.replace('/', os.sep))
    if not os.path.isfile(p):
        raise FileNotFoundError(p)
    return open(p, encoding='utf-8', newline='').read().replace('\r\n', '\n')


def run_lints(root):
    for name, rel, needles in LINTS:
        try:
            text = read(root, rel)
        except FileNotFoundError as e:
            return fail('lint cannot read its input: %s' % e)
        for n in needles:
            if n not in text:
                return fail('lint "%s": %s lacks: %s' % (name, rel, n))
        for n in needles:
            planted = text.replace(n, '')
            if planted == text:
                return fail('lint plant DID NOT APPLY: %s / %s' % (rel, n))
            if all(m in planted for m in needles):
                return fail('lint "%s" is blind to the removal of: %s' % (name, n))
        print('PASS lint: %s (%d snippets, %d planted twins caught)' % (name, len(needles), len(needles)))
    t = read(root, 'src/owner_timing.h')
    a, b = t.find('JoinForItem(it, env);  // S102 R25-A'), t.find('core.Process(env, it);')
    if a < 0 or b < 0 or a > b:
        return fail('lint: the timing thread must wait for the ticket BEFORE core.Process')
    print('PASS lint: JoinForItem comes before core.Process (the guest cannot see the answer first)')
    return 0


def run_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'swap_tail2', 'swap_tail2_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 'r25a')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'swap_tail2_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('swap_tail2_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('swap_tail2_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 6:
        return fail('expected >= 6 PASS lines, got %d' % passes)
    caught = []
    for n in PLANTS:
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=300)
        nfail = len(re.findall(r'^FAIL ', p.stdout, re.M))
        if p.returncode != 1 or nfail < 1:
            return fail('planted fault %d was NOT caught (exit %d, %d FAIL lines)' % (n, p.returncode, nfail))
        caught.append('%d (%d FAIL)' % (n, nfail))
    print('PASS planted twins: all %d caught: plant %s' % (len(PLANTS), ', '.join(caught)))
    if run_lints(root):
        return 1
    print('PASS: swap_tail2_check: %d checks, %d planted twins caught, %d lints, exit 0' % (passes, len(PLANTS), len(LINTS) + 1))
    return 0


def run_syntax(root):
    main = main_checkout(root)
    if not main or not os.path.isdir(os.path.join(main, 'generated', 'default')):
        return fail('main checkout with generated\\default not found (%s)' % main)
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    inc = ['-I' + wt, '-I' + wt + '/src', '-I' + m, '-I' + m + '/src', '-I' + m + '/generated/default', '-I' + m + '/thirdparty/streamline/sdk/include',
           '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include', '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include/dxc']
    bad = 0
    for tu in TUS:
        p = os.path.join(wt, tu)
        if not os.path.isfile(p):
            return fail('missing ' + p)
        r = subprocess.run([CLANG] + GAME_FLAGS + inc + [p], capture_output=True, text=True)
        print('%s %s' % ('ok  ' if r.returncode == 0 else 'FAIL', tu))
        if r.returncode:
            bad += 1
            print((r.stdout + r.stderr)[:1500])
    if bad:
        return fail('%d game unit(s) do not compile' % bad)
    print('PASS: %d game translation units compile (-fsyntax-only, the main build flags)' % len(TUS))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return run_syntax(root) if a.syntax else run_test(root)


if __name__ == '__main__':
    sys.exit(main())
