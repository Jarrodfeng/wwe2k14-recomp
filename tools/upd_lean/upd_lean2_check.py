#!/usr/bin/env python3
"""S102 lane U3 gate check (offline, 0 game runs): wwe2k14_rdr_upd_lean bit 16 (round 2 of our work off the game's update thread).

  py -I tools\\upd_lean\\upd_lean2_check.py [--root DIR] [--build-only]

Builds tools\\upd_lean\\upd_lean2_test.cpp (+ src\\scene_live\\wrestler_contrib.cpp, src\\renderer_scene\\record_io.cpp) against the SHIPPED upd_lean.h, arena_world_hook.h, guest_word.h and
wrestler_contrib, with -DUPD_LEAN_PLANTS, and runs it: the host-write page marks come out bit-equal with the report memo (bit 32) on and with the reader's clears announced; a writer / reader stress
leaves no page clear on stale data; the in-line guest word read equals the SafeCopy read (and faults the same way); the wrestlers' emitted records and counters are byte-equal with the in-place consumer
palette flipped on and off; the arena worlds and stats are byte-equal with bit 16 flipped on / off (every call, every 97 calls, never off); each arm also proves the lever TOOK. Then the planted faults
(`test plant N`: 9 epoch ignored, 10 a report past the memo range skipped, 11 the in-place palette stale, 12 the flat index keyed by object alone, 1 / 3 the marks dropped) must each exit 1 with a FAIL
line. Then the glue lints (reader.h, upd_lean.h, wrestler_hook.h, model_hooks.h, fp_probe.h, arena_world_hook.h, rdr_switches.h), each with planted twins (a text mutation of the lint's input that must
make it fail; a mutation that does not apply is its own failure). The game-unit syntax check is `upd_lean_check.py --syntax` (U2's, main.cpp + live.cpp as the game compiles them).
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = [11]
SRCS = ['tools/upd_lean/upd_lean2_test.cpp', 'src/scene_live/wrestler_contrib.cpp', 'src/renderer_scene/record_io.cpp']


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def read(root, rel):
    p = os.path.join(root, rel.replace('/', os.sep))
    if not os.path.isfile(p):
        raise FileNotFoundError(p)
    return open(p, encoding='utf-8').read()


def build(root):
    if not os.path.isfile(CLANG):
        return None, fail('%s not found' % CLANG)
    srcs = []
    for s in SRCS:
        p = os.path.join(root, s.replace('/', os.sep))
        if not os.path.isfile(p):
            return None, fail('missing ' + p)
        srcs.append(p)
    out_dir = os.path.join(root, 'out', 'u3')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'upd_lean2_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    # S102 main: the compile ran only when an old exe existed (indentation); a fresh tree failed with an UnboundLocalError
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DUPD_LEAN_PLANTS', '-DNOMINMAX', '-fuse-ld=lld', '-o', exe] + srcs,
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return None, fail('upd_lean2_test did not compile\n' + b.stdout + b.stderr)
    return exe, 0


# (name, file, [snippets that must be in the file], [(first, then): `first` must come BEFORE `then`]). The planted twin removes ONE snippet from a copy of the text.
LINTS = [
    ('rdr_switches.h: still PerFrame, default off', 'src/renderer_adapter/rdr_switches.h', ['X(UpdLean, "wwe2k14_rdr_upd_lean", PerFrame, 0)'], []),
    ('wrestler_hook.h: the guest memory is resolved once behind bit 16; the word read stays the SafeCopy one',
     'src/scene_live/wrestler_hook.h',
     ['inline rex::memory::Memory* GuestMem()', 'if (wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry2)) {', 'auto* host = GuestMem()->TranslateVirtual<const uint8_t*>(guest);',
      'if (!ReadGuest(guest, &raw, 4)) return false;'], []),
    ('model_hooks.h / fp_probe.h: the in-place flag reaches the contributor',
     'src/scene_live/model_hooks.h', ['bool census = true, bool in_place = false)', 'w.SubmitConsumer(model, want, scratch.data(), frame, in_place);', 'w.SubmitConsumer(model, count, scratch.data(), frame, in_place);'], []),
    ('fp_probe.h: bit 16 is passed', 'src/fp_probe.h', ['wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry2));'], []),
    ('wrestler_contrib.cpp: the in-place put declines without touching anything',
     'src/scene_live/wrestler_contrib.cpp',
     ['if (in_place && PutConsumerInPlace(skel, frame, skin, count)) return;', 'if (pc == cur_.end() || known_.find(skel) == known_.end()) return false;',
      'Known* k = KnownFor(skel);   // the same one call Put makes', 'x.skin.assign(skin, skin + count);'],
     [('if (pc == cur_.end() || known_.find(skel) == known_.end()) return false;', 'Known* k = KnownFor(skel);   // the same one call Put makes')]),
    ('arena_world_hook.h: the frame buffers are swapped behind bit 16',
     'src/scene_live/arena_world_hook.h',
     ['std::swap(ring_[closed_count_ % kRing], cur_);', 'std::swap(out, b);', 'wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry2)'], []),
]


def run_lints(root):
    for name, rel, needles, orders in LINTS:
        try:
            text = read(root, rel)
        except FileNotFoundError as e:
            return fail('lint cannot read its input: %s' % e)
        for n in needles:
            if n not in text:
                return fail('lint "%s": %s lacks: %s' % (name, rel, n))
        for a, b in orders:
            ia, ib = text.find(a), text.find(b)
            if ia < 0 or ib < 0 or ia > ib:
                return fail('lint "%s": %s must come before %s' % (name, a, b))
        for n in needles:   # the planted twin: remove each needle from a copy; the lint must then fail (and the removal must apply)
            planted = text.replace(n, '')
            if planted == text:
                return fail('lint plant DID NOT APPLY: %s / %s' % (rel, n))
            if all(m in planted for m in needles):
                return fail('lint "%s" is blind to the removal of: %s' % (name, n))
        for a, b in orders:   # the order twin: swap the two snippets' first occurrences' places by moving `a` after `b`
            planted = text.replace(a, '', 1)
            ib = planted.find(b)
            planted = planted[:ib + len(b)] + a + planted[ib + len(b):]
            if planted == text:
                return fail('lint order plant DID NOT APPLY: %s' % rel)
            if planted.find(a) < planted.find(b):
                return fail('lint "%s" order twin did not reorder' % name)
        print('PASS lint: %s (%d snippets, %d order rules, planted twins caught)' % (name, len(needles), len(orders)))
    return 0



# ---- the SEH class (U3 review, blocker 1) ------------------------------------------------------------------------------------------------------------------------------
# The game target (main.cpp and every header it includes) is NOT built with async exceptions: clang's __try then catches only a fault inside a CALLEE of the try body. So every __try body must
# call through a `(*volatile name)` function pointer (SafeCopy's pattern) and may not touch guest memory itself. probe_plugin.h belongs to the probes target (built with -fasync-exceptions,
# CMakeLists.txt:393) and platform/compat.h defines __try away: both allowed.
SEH_ALLOW = ('src/probe_plugin.h', 'src/platform/compat.h')


def try_bodies(text):
    for m in re.finditer(r'__try\s*\{', text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        yield text[:m.start()], text[m.end():i - 1], text.count('\n', 0, m.start()) + 1


def seh_offenders(files):
    bad = []
    for rel, text in files:
        if rel in SEH_ALLOW:
            continue
        for before, body, line in try_bodies(text):
            names = set(re.findall(r'\(\s*\*\s*volatile\s+(\w+)\s*\)', before[-1500:]))
            calls = set(re.findall(r'(\w+)\s*\(', body)) - {'GetExceptionCode'}
            direct = re.search(r'\b(memcmp|memcpy|memmove|reinterpret_cast|bswap)\b|\*\s*\w+\s*;|\*\s*\(', body)
            if not (names & calls) or direct:
                bad.append('%s:%d' % (rel, line))
    return bad


def run_seh_lint(root):
    files = []
    for dp, _, fns in os.walk(os.path.join(root, 'src')):
        for fn in fns:
            if fn.endswith(('.h', '.cpp')):
                p = os.path.join(dp, fn)
                files.append((os.path.relpath(p, root).replace(os.sep, '/'), open(p, encoding='utf-8', errors='replace').read()))
    if not any('__try' in t for _, t in files):
        return fail('SEH lint read no __try at all (a lint that reads nothing proves nothing)')
    bad = seh_offenders(files)
    if bad:
        return fail('__try bodies that touch memory themselves or call no volatile function pointer (a fault there is NOT caught in the game build): ' + ', '.join(bad))
    n = sum(len(list(try_bodies(t))) for rel, t in files if rel not in SEH_ALLOW)
    # planted twins: the dropped LoadRaw body, and CompareGuest as it was (an inline memcmp)
    twin1 = [('src/x.h', 'inline bool L(const uint8_t* h, uint32_t& r) {\n  __try {\n    r = *reinterpret_cast<const volatile uint32_t*>(h);\n  } __except (1) { return false; }\n  return true;\n}\n')]
    twin2 = [('src/y.h', 'static int (*volatile cmp)(const void*, const void*, size_t) = std::memcmp;\n__try {\n  return std::memcmp(h, r, n);\n} __except (1) { return -1; }\n')]
    good = [('src/z.h', 'static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;\n__try {\n  copy(dst, src, n);\n} __except (1) { return false; }\n')]
    if not seh_offenders(twin1) or not seh_offenders(twin2) or seh_offenders(good):
        return fail('SEH lint planted twins: an inline volatile load / an inline memcmp must be flagged and the SafeCopy pattern must not')
    print('PASS lint: %d __try bodies under src, every one calls through a volatile function pointer and touches no memory itself; planted twins (inline volatile load, inline memcmp) caught, SafeCopy not' % n)
    return 0


def game_flags(root):
    """The FLAGS the game target compiles main.cpp with, parsed from the main checkout's build.ninja (no PCH, no debug info)."""
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    main = os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip()))) if out.returncode == 0 else None
    ninja = os.path.join(main or '', 'out', 'build', 'win-amd64-release', 'build.ninja')
    if not os.path.isfile(ninja):
        return None, fail('build.ninja not found (%s): the SEH repro needs the game target flags' % ninja)
    lines = open(ninja, encoding='utf-8', errors='replace').read().split('\n')
    for i, l in enumerate(lines):
        if l.startswith('build CMakeFiles/wwe2k14.dir/src/main.cpp.obj'):
            for k in range(i + 1, i + 8):
                if lines[k].strip().startswith('FLAGS ='):
                    toks = lines[k].split('=', 1)[1].split()
                    keep, j = [], 0
                    while j < len(toks):
                        t = toks[j]
                        if t == '-Xclang':
                            if j + 1 < len(toks) and toks[j + 1].startswith('--dependent-lib'):
                                keep += [t, toks[j + 1]]
                            j += 2
                            continue
                        if not t.startswith('-g'):
                            keep.append(t)
                        j += 1
                    return keep, 0
    return None, fail('main.cpp FLAGS not found in ' + ninja)


SEH_TU = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <excpt.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// the dropped LoadRaw: a plain load inside __try
static bool Inline(const uint8_t* h, uint32_t& r) {
  __try { r = *reinterpret_cast<const volatile uint32_t*>(h); } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
  return true;
}
// SafeCopy's pattern (wrestler_hook.h)
static bool Safe(const uint8_t* h, uint32_t& r) {
  static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
  __try { copy(&r, h, 4); } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
  return true;
}
int main(int argc, char** argv) {
  void* bad = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
  uint32_t r = 0;
  const bool ok = (argc > 1 && argv[1][0] == 'i') ? Inline(static_cast<const uint8_t*>(bad), r) : Safe(static_cast<const uint8_t*>(bad), r);
  std::printf("read of a NOACCESS page returned %d\n", ok ? 1 : 0);
  return ok ? 3 : 0;
}
"""


def run_seh_repro(root):
    flags, rc = game_flags(root)
    if rc:
        return rc
    d = os.path.join(root, 'out', 'u3')
    os.makedirs(d, exist_ok=True)
    src, exe = os.path.join(d, 'seh_repro.cpp'), os.path.join(d, 'seh_repro.exe')
    open(src, 'w').write(SEH_TU)
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG] + flags + ['-DNOMINMAX', '-fuse-ld=lld', '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('SEH repro did not compile with the game flags %s\n%s' % (' '.join(flags), b.stdout + b.stderr))
    s = subprocess.run([exe, 's'], capture_output=True, text=True, timeout=60)
    i = subprocess.run([exe, 'i'], capture_output=True, text=True, timeout=60)
    if s.returncode != 0 or 'returned 0' not in s.stdout:
        return fail('the SafeCopy pattern did not survive a NOACCESS page with the game flags (exit %d)' % s.returncode)
    if i.returncode == 0 or i.returncode == 3:
        return fail('the planted inline load did NOT crash with the game flags (exit %d): the repro no longer shows the class it guards' % i.returncode)
    print('PASS seh repro: with the game target flags (%s) the SafeCopy pattern returns false on a NOACCESS page (exit 0) and the inline volatile load dies (exit 0x%X)' % (' '.join(flags), i.returncode & 0xFFFFFFFF))
    return 0


def run_test(root, build_only=False):
    exe, rc = build(root)
    if rc:
        return rc
    if build_only:
        print('built ' + exe)
        return 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=1800)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('upd_lean2_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 4:
        return fail('expected >= 4 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    for needle in ('consumer.equal', 'consumer.took', 'arena.swap_equal', 'arena.swap_took'):
        if not re.search(r'^PASS  ' + re.escape(needle) + r'\b', text, re.M):
            return fail('the arm "%s" did not run / did not pass' % needle)
    caught = []
    for n in PLANTS:
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=1800)
        nfail = len(re.findall(r'^FAIL ', p.stdout, re.M))
        if p.returncode != 1 or nfail < 1:
            return fail('planted fault %d was NOT caught (exit %d, %d FAIL lines)' % (n, p.returncode, nfail))
        caught.append('%d (%d FAIL)' % (n, nfail))
    print('PASS planted twins: all %d caught: plant %s' % (len(PLANTS), ', '.join(caught)))
    if run_lints(root) or run_seh_lint(root) or run_seh_repro(root):
        return 1
    print('PASS: upd_lean2_check: %d checks, %d planted twins caught, %d lints, exit 0' % (passes, len(PLANTS), len(LINTS)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--build-only', action='store_true')
    a = ap.parse_args()
    return run_test(os.path.abspath(a.root), a.build_only)


if __name__ == '__main__':
    sys.exit(main())
