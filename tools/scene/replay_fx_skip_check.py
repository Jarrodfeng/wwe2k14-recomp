#!/usr/bin/env python3
"""S110 TABLEFIRE check: builds tools\\scene\\replay_fx_skip_unit.cpp against src\\replay_fx_skip_core.h (synthetic guest memory, 0 game runs, no game, no SDK),
runs it, then builds each planted twin (a text-edited copy of the core; each must fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\replay_fx_skip_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
# twin name: (text in the core, replacement, the check that must fail)
TWINS = {
    'from_last': ('uint32_t f = (last + 1) % cap;', 'uint32_t f = last % cap;', 'skip.one_record'),
    'type_at_16': ('inline constexpr uint32_t kEvSize = 24, kEvType = 21,', 'inline constexpr uint32_t kEvSize = 24, kEvType = 16,', 'skip.one_record'),
    'newest_is_next': ('const uint32_t newest = (next % cap + cap - 1) % cap;', 'const uint32_t newest = next % cap;', 'playedout.next_slot'),
    'chain_by_cap': ('if (++steps > kPool || i >= kPool)', 'if (++steps > cap || i >= kPool)', 'chain.pool_bound'),
    'gate_removed': ('  if (!skipped) return w;', '', 'gate.clear'),
    'begin_no_clear': ('void OnBegin() { skipped.store(false); }', 'void OnBegin() {}', 'gate.begin_clears'),
}


def build(root, out, core_dir, exe):
    flags = ['-std=c++20', '-Wall', '-Wextra', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', '-I', core_dir]
    src = os.path.join(root, 'tools', 'scene', 'replay_fx_skip_unit.cpp')
    return subprocess.run([CLANG] + flags + [src, '-o', exe], capture_output=True, text=True, cwd=out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's110', 'tablefire')
    os.makedirs(out, exist_ok=True)

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    exe = os.path.join(out, 'replay_fx_skip_unit.exe')
    b = build(root, out, os.path.join(root, 'src'), exe)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    core = text('src/replay_fx_skip_core.h')
    for name, (old, new, check) in TWINS.items():
        if core.count(old) != 1:
            print('FAIL: twin %s: the core line to plant is not there exactly once: %s' % (name, old))
            bad += 1
            continue
        d = os.path.join(out, 'twin_' + name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'replay_fx_skip_core.h'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(core.replace(old, new))
        texe = os.path.join(d, 'unit.exe')
        tb = build(root, d, d, texe)
        if tb.returncode:
            print('FAIL: twin %s did not compile\n%s%s' % (name, tb.stdout, tb.stderr))
            bad += 1
            continue
        p = subprocess.run([texe], capture_output=True, text=True, cwd=d)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(check in ln for ln in failed)
        print('twin %-12s %s (%s)' % (name, 'caught' if ok else 'NOT CAUGHT', check))
        bad += 0 if ok else 1
    need = [
        ('src/wwe2k14_app.h', '#include "replay_fx_skip.h"', 'the app includes the hook'),
        ('src/replay_fx_skip.h', 'REX_HOOK_RAW(sub_8271AA48)', 'the hook on the PRTI track end method'),
        ('docs/GAME_BUG_FIXES.md', '| GBF-3 |', 'the fix has its GBF-3 row'),
        ('src/replay_fx_skip.h', 'GBF-3', 'the site comment points to GBF-3'),
        ('src/replay_fx_skip.h', 'const bool skipped = g_gate.TakeAtEnd();\n    const Walk w = SkippedStops(mem, ctx.r3.u32, skipped);   // read before the original',
         'the walk runs before the original, gated on the skip (S110 BUSWAIT split the gate read into `skipped`; FIRETABLE fix 2 re-pinned it)'),
        ('src/replay_fx_skip.h', 'REX_HOOK_RAW(sub_826071F8)', 'the skip hook sets the gate'),
        ('src/replay_fx_skip.h', 'REX_HOOK_RAW(sub_8271A630)', 'begin clears the gate (the hook on the begin method)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    if '#include' in core and any(x in core for x in ('<rex/', 'ppc/context')):
        print('FAIL: lint: the core includes a game / SDK header')
        bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
