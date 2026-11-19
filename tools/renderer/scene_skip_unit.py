#!/usr/bin/env python3
"""S103 lane H: builds and runs tools\\renderer\\scene_skip_unit.cpp (the scene skips' decision wiring: GPU free, no game, 0 game runs), then its planted twins (each must exit 1), then the
wiring lints of the game-side hooks (compiled, NOT run: the lints read the sources and fail loudly on a missing piece).
  py -I tools\\renderer\\scene_skip_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'

# function -> (the file that holds its ONE hook, the Hook id it asks first)
HOOKS = {
    'sub_82C04DF0': ('src/draw_origin.h', 'Model'),
    'sub_82C13A80': ('src/scene_live/scene_skip_nodes.h', 'ModelRecord'),
    'sub_82519508': ('src/scene_live/crowd_hooks.h', 'Crowd'),
    'sub_827FD698': ('src/scene_live/bake_skip_hook.h', 'CardBake'),
    'sub_825D0438': ('src/scene_live/scene_skip_nodes.h', 'RingColour'),
    'sub_825D01F0': ('src/scene_live/scene_skip_nodes.h', 'RingColour2'),
    'sub_827E9740': ('src/scene_live/own_node_hook.h', 'ShadowModule'),
    'sub_827EA7B8': ('src/scene_live/own_node_hook.h', 'RimMap'),
    'sub_827EF070': ('src/scene_live/scene_skip_nodes.h', 'Plane'),
    'sub_827EBE78': ('src/scene_live/scene_skip_nodes.h', 'PreLight'),
    'sub_8279ACD8': ('src/scene_live/scene_skip_nodes.h', 'Sweat'),
    'sub_82254E30': ('src/scene_live/scene_skip_nodes.h', 'Particles'),
    'sub_82C1E840': ('src/scene_live/scene_skip_nodes.h', 'Post'),
    'sub_8276EAA0': ('src/scene_crowd.h', 'FloorSeats'),
    'sub_82C1F728': ('src/scene_live/scene_skip_nodes.h', 'FillColorDepth'),   # S103 T
    'sub_82C0F528': ('src/scene_live/trace_census_hook.h', 'FlashPass'),       # S103 C3 (lane T's census hook, the one hook on this function)
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'h')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'scene_skip_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-pthread', '-o', exe, os.path.join(root, 'tools', 'renderer', 'scene_skip_unit.cpp')], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for plant in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11):
        p = subprocess.run([exe, str(plant)], capture_output=True, text=True)
        if p.returncode == 0:
            print('FAIL: planted twin %d was not caught' % plant)
            return 1
        print('twin %d: caught' % plant)

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return f.read()

    bad = 0

    def need(ok, what):
        nonlocal bad
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += 0 if ok else 1

    # every function takes ONE hook (S101); each hook asks Ours(Hook::X) before anything else
    srcs = {}
    for dp, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if f.endswith(('.h', '.cpp')):
                p = os.path.join(dp, f)
                srcs[os.path.relpath(p, root).replace('\\', '/')] = open(p, encoding='utf-8', errors='replace').read()
    for fn, (path, hook) in HOOKS.items():
        hits = sorted(p for p, s in srcs.items() if re.search(r'^\s*REX_HOOK(_RAW)?\(%s\)' % fn, s, re.M))
        need(hits == [path], 'exactly one hook on %s (%s)' % (fn, ', '.join(hits)))
        s = srcs[path]
        i = s.index('REX_HOOK_RAW(%s) {' % fn)
        body = s[i:i + 1500]   # S103 WC: the raw hook on sub_82C04DF0 grew by the draw-camera call and its comments
        j = body.find('Ours(')
        k = body.find('__imp__%s' % fn)
        pre = body[:j] if j >= 0 else body
        # nothing but the opening lines (a using, the post hook's census line) may stand before the scene test; the original is never called before it
        need(j >= 0 and 'Hook::%s' % hook in body[j:j + 80] and (k < 0 or j < k), '%s: scene_skip::Ours(Hook::%s) comes before the original' % (fn, hook))
        seg = body[:body.find(chr(10), j)] if j >= 0 else body
        need('scene_frame::Decide' not in seg and 'DecideNow' not in seg and 'DecideNode' not in seg, '%s: the hook does not decide on its own' % fn)
    # no scene hook file decides itself
    for p in ('src/scene_live/scene_skip.h', 'src/scene_live/scene_skip_nodes.h'):
        need('DecideNow' not in text(p) and 'scene_frame::Decide(' not in text(p), '%s never calls Decide / DecideNow (only scene_frame::Ours through scene_skip_core.h)' % p)
    # the cvar is defined once, and the clock is the hook clock
    cv = [p for p, s in srcs.items() if re.search(r'REXCVAR_DEFINE_\w+\(\s*wwe2k14_rdr_scene\s*,', s)]   # ALL of src (game_syntax compiles main.cpp only: review B1)
    # S103 P9: with WWE2K14_KEEP_STREAM OFF live.cpp is not built, so renderer_game/owner_cvars_off.cpp (built only then, under #if !WWE2K14_STREAM()) defines it instead: one definition per configuration
    need(sorted(cv) == ['src/renderer_adapter/live.cpp', 'src/renderer_game/owner_cvars_off.cpp'], 'the cvar wwe2k14_rdr_scene is defined once per configuration: live.cpp (stream kept) and owner_cvars_off.cpp (stream left out) (%s)' % ', '.join(cv))
    # L6: the two definitions are exclusive: the OFF unit's definition sits inside `#if !WWE2K14_STREAM()`, the unit is built only when OFF, and live.cpp is dropped when OFF
    off_txt = text('src/renderer_game/owner_cvars_off.cpp')
    m_if = off_txt.find('#if !WWE2K14_STREAM()')
    m_def = off_txt.find('REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene')
    m_end = off_txt.find('#endif', m_if) if m_if >= 0 else -1
    need(0 <= m_if < m_def < m_end, 'owner_cvars_off.cpp defines wwe2k14_rdr_scene inside #if !WWE2K14_STREAM() (never in a configuration where live.cpp defines it)')
    need('src/renderer_game/owner_cvars_off.cpp' in text('tools/scene/keep_stream_off_sources.txt'), 'owner_cvars_off.cpp is listed in keep_stream_off_sources.txt (built only when the stream is left out)')
    need('src/renderer_adapter/live.cpp' in text('tools/scene/keep_stream_sources.txt'), 'live.cpp is listed in keep_stream_sources.txt (dropped when the stream is left out)')
    need('REXCVAR_DECLARE(int32_t, wwe2k14_rdr_scene)' in text('src/scene_live/scene_skip.h'), 'scene_skip.h only DECLARES it')
    need('g_guest_frame' in text('src/scene_live/scene_skip.h') and 'g_clock.store' in text('src/scene_live/scene_skip.h'), 'the frame clock is wwe2k14::g_guest_frame, installed at start')
    # the hook / stream offset the render side relies on
    d3 = text('src/renderer_adapter/d3_arena.h')
    m = re.search(r'kHookToStreamOffset\s*=\s*(\d+)', d3)
    sf = re.search(r'kHookToStream\s*=\s*(\d+)', text('src/renderer_game/scene_frame.h'))
    need(m is not None and sf is not None and int(m.group(1)) == int(sf.group(1)) == 1, 'd3_arena.h kHookToStreamOffset == scene_frame.h kHookToStream == 1')
    need('kHookToStreamOffset =' not in text('src/renderer_game/scene_skip_core.h'), 'scene_skip_core.h keeps no offset constant of its own (review B2)')
    need('SnapshotMode' in text('src/frame_stats.h'), 'FrameStats::OnSwap takes the mode snapshot (review F3)')
    # every Hook enum entry is asked by some hook
    core = text('src/renderer_game/scene_skip_core.h')
    enum = re.search(r'enum class Hook : uint8_t \{(.*?)Count', core, re.S).group(1)
    names = re.findall(r'^\s*(\w+)\s*(?:=\s*\d+)?,', enum, re.M)
    used = ' '.join(srcs[p] for p in set(v[0] for v in HOOKS.values()))
    for n in names:
        need('Hook::%s' % n in used, 'Hook::%s is asked by a hook' % n)
    need(len(names) == len(HOOKS), 'the enum and the hook table agree (%d hooks)' % len(names))
    # the double check's classes: the 3D scene classes exist in names.h
    nm = text('src/renderer_adapter/names.h')
    for c in ('ModelPart', 'Crowd', 'Particle', 'Shadow', 'Post'):
        need(re.search(r'\b%s\b' % c, nm) is not None, 'names.h DrawClass::%s exists (SCENE_SKIPS.md section 5)' % c)
    # the origin table holds the three 'other' submitters
    ot = text('src/renderer_adapter/origin_table.h')
    for addr in ('0x82799B60u', '0x8279A328u', '0x82C20AB8u'):
        need(addr in ot, 'origin_table.h has the scene submitter %s' % addr)
    print('lints: %d failed' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
