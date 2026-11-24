#!/usr/bin/env python3
"""S102 lane U2 check (offline, 0 game runs): wwe2k14_rdr_upd_lean (our work off the game's update thread).

  py -I tools\\upd_lean\\upd_lean_check.py [--root DIR] [--syntax] [--build-only]

Default: builds tools\\upd_lean\\upd_lean_test.cpp against the SHIPPED upd_lean.h, origin_intern.h, light_state_hook.h, arena_world_hook.h, prov_map.h (with
-DUPD_LEAN_PLANTS) and runs it: the host-write page marks, the draw-origin tags, the frame's light states / refs / hashes and the arena worlds come out byte-identical
with the switch off and on; each arm also proves the lever TOOK (a lever that takes 0 frames does nothing). Then the 7 planted faults (`test plant N`) must each make it
exit 1 with a FAIL line. Then the glue lints on reader.h / fp_probe.h / model_hooks.h / light_state_glue.h / arena_world_glue.h / scene_crowd.h / rdr_switches.h, each with
a planted twin (a text mutation of the lint's input that must make it fail; a mutation that does not apply is its own failure).
--syntax: clang -fsyntax-only of main.cpp and live.cpp with the main build's flags (the game compiles Windows headers fview / rplay do not).
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']
PLANTS = [1, 3, 4, 5, 6, 7, 8]


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


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
    src = os.path.join(root, 'tools', 'upd_lean', 'upd_lean_test.cpp')
    if not os.path.isfile(src):
        return None, fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 'u2')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'upd_lean_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DUPD_LEAN_PLANTS', '-DNOMINMAX', '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return None, fail('upd_lean_test did not compile\n' + b.stdout + b.stderr)
    return exe, 0


# (name, file, [required snippets]) : every snippet must be in the file. The planted twin removes ONE snippet from a copy of the text.
LINTS = [
    ('reader.h: the switch is read every frame and the marks use the lean report',
     'src/renderer_adapter/reader.h',
     ['rdr_sw::IntSeen(rdr_sw::kUpdLean, 0, guest_frame)', 'wwe2k14::upd_lean::g_census.store(SourceCensusOn()', 'wwe2k14::upd_lean::MarkPagesLean(g_written_pages, g_written_pages_geo, g_notified_pages, g_notified_pages_geo',
      '[rdr][upd_lean]', 'explicit_off']),
    ('prov_map.h: an explicit =false is honoured', 'src/renderer_adapter/prov_map.h', ['if (explicit_off) return capture_frames_at_start;']),
    ('rdr_switches.h: PerFrame, default off', 'src/renderer_adapter/rdr_switches.h', ['X(UpdLean, "wwe2k14_rdr_upd_lean", PerFrame, 0)']),
    ('scene_crowd.h: the op-21 early exit keeps BOTH crowd sites', 'src/scene_crowd.h',
     ['wwe2k14::upd_lean::On(wwe2k14::upd_lean::kCrowdHooks) && uint32_t(ctx.lr) != kOp21Return && uint32_t(ctx.lr) != kCardOp21Return']),
    ('model_hooks.h / fp_probe.h: the wrestler census flag', 'src/scene_live/model_hooks.h', ['if (census) w.NoteEntryUnknownModel(model, palette_ptr);']),
    ('fp_probe.h: the flag is passed', 'src/fp_probe.h', ['!wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry),']),   # S102 U3: kEntry2 follows on the next line
    ('light_state_glue.h: the fast path is behind the entry bit', 'src/scene_live/light_state_glue.h',
     ['wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry)', 'OnModelCallLean(', 'CompareGuest(a, ref, n)']),
    ('arena_world_glue.h: the memo is behind the entry bit', 'src/scene_live/arena_world_glue.h', ['wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry));']),
    ('origin_intern.h: Intern is behind the origin bit', 'src/renderer_adapter/origin_intern.h',
     ['if (wwe2k14::upd_lean::On(wwe2k14::upd_lean::kOriginFront)) return InternFast(k);', 'if (c.owner != serial_ && !UPD_PLANT(4))']),
]


def run_lints(root):
    for name, rel, needles in LINTS:
        try:
            text = read(root, rel)
        except FileNotFoundError as e:
            return fail('lint cannot read its input: %s' % e)
        for n in needles:
            if n not in text:
                return fail('lint "%s": %s lacks: %s' % (name, rel, n))
        # the planted twin: remove each needle from a copy; the lint must then fail (and the removal must apply)
        for n in needles:
            planted = text.replace(n, '')
            if planted == text:
                return fail('lint plant DID NOT APPLY: %s / %s' % (rel, n))
            if all(m in planted for m in needles):
                return fail('lint "%s" is blind to the removal of: %s' % (name, n))
        print('PASS lint: %s (%d snippets, %d planted twins caught)' % (name, len(needles), len(needles)))
    # the kCrowdHooks lint also must find the early exit BEFORE the cvar read it skips
    t = read(root, 'src/scene_crowd.h')
    a, b = t.find('kCrowdHooks'), t.find('int mode = REXCVAR_GET(wwe2k14_scene_crowd);')
    if a < 0 or b < 0 or a > b:
        return fail('lint: the op-21 early exit must come before the cvar read in scene_crowd.h')
    print('PASS lint: the op-21 early exit sits before the cvar read and the Cp() query')
    return 0


def run_test(root, build_only=False):
    exe, rc = build(root)
    if rc:
        return rc
    if build_only:
        print('built ' + exe)
        return 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=900)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('upd_lean_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 12:
        return fail('expected >= 12 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    for needle in ('marks.equal_census', 'marks.contended_cheaper', 'marks.stress_no_stale', 'origin.roundtrip', 'origin.took', 'lights.equal', 'lights.took', 'arena.equal', 'arena.took', 'prov.explicit_off'):
        if needle not in text:
            return fail('the arm "%s" did not run' % needle)
    caught = []
    for n in PLANTS:
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=900)
        nfail = len(re.findall(r'^FAIL ', p.stdout, re.M))
        if p.returncode != 1 or nfail < 1:
            return fail('planted fault %d was NOT caught (exit %d, %d FAIL lines)' % (n, p.returncode, nfail))
        caught.append('%d (%d FAIL)' % (n, nfail))
    print('PASS planted twins: all %d caught: plant %s' % (len(PLANTS), ', '.join(caught)))
    if run_lints(root):
        return 1
    print('PASS: upd_lean_check: %d checks, %d planted twins caught, %d lints, exit 0' % (passes, len(PLANTS), len(LINTS) + 1))
    return 0


def run_syntax(root):
    main = main_checkout(root)
    if not main or not os.path.isdir(os.path.join(main, 'generated', 'default')):
        return fail('main checkout with generated\\default not found (%s): the game units cannot be syntax-checked' % main)
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
    print("PASS: %d game translation units compile (-fsyntax-only, the main build's flags)" % len(TUS))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--build-only', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return run_syntax(root) if a.syntax else run_test(root, a.build_only)


if __name__ == '__main__':
    sys.exit(main())
