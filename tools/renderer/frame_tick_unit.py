#!/usr/bin/env python3
"""S103 landing 2, piece P6: builds and runs tools\\renderer\\frame_tick_unit.cpp (src\\renderer_game\\frame_tick.h, the scene record's frame boundary out of
the stream reader: no game, no GPU, 0 game runs), then its planted twins (each must exit 1), then the wiring lints of the game side (compiled, NOT run):
  - reader.h makes the boundary calls through frame_tick (SceneEnd / SceneLatch / SceneEmit / NoteStreamPost) and never calls rdr_scene::live::EndFrame,
    LatchFrame or EmitLatched itself; the display table lives in frame_tick (no g_display_curve left in the reader);
  - the present hook sub_82CB37C0 (half2_hooks.h) runs the reader's present OR frame_tick::StreamFreePresent, exactly one, before KoLatchAtPresent;
  - the gamma hook sub_82CBABB0 calls frame_tick::OnGammaRamp outside the ReaderOn() gate;
  - the release hook sub_82C3BB38 (tail_release_hook.h) finishes through frame_tick::FinishTailAtRelease, not a reader function;
  - nothing else in src calls the scene writer's boundary (EndFrame / LatchFrame / EmitLatched) outside frame_tick.h.
  py -I tools\\renderer\\frame_tick_unit.py [--root DIR]
Exit 0 = pass, 1 = fail. A twin of the lints: --selftest-lints runs them on planted copies of the pre-P6 sources (each must fail)."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'old_hook', 2: 'double_tick', 3: 'stale_frame', 4: 'curve_lost'}


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path, part_files  # a split file is read with its parts expanded (tools/split/read_unit.py)


def read(root, rel):
    return read_path(os.path.join(root, rel))


def hook_body(src, fn):
    i = src.find('REX_HOOK_RAW(%s) {' % fn)
    if i < 0:
        return None
    depth, j = 0, src.index('{', i)
    for k in range(j, len(src)):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    return None


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def lints(texts):
    """texts: rel path -> source. Returns the list of problems (empty = pass)."""
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)

    reader = strip_comments(texts['src/renderer_adapter/reader.h'])
    for fn in ('EndFrame', 'LatchFrame', 'EmitLatched'):
        need(re.search(r'rdr_scene::live::%s\s*\(' % fn, reader) is None, 'reader.h does not call rdr_scene::live::%s itself' % fn)
    for fn in ('SceneEnd', 'SceneLatch', 'SceneEmit', 'NoteStreamPost', 'TheCurve().TakePending', 'TheCurve().RepostIfKnown'):
        need(('frame_tick::%s' % fn) in reader, 'reader.h calls frame_tick::%s' % fn)
    need(re.search(r'\bg_display_curve(_pending|_known)?\b', reader) is None, 'reader.h keeps no display table of its own')
    m = re.search(r'inline void OnPresent\(PPCContext& ctx, uint8_t\* base\) \{(.*?)\n\}', reader, re.S)
    need(m is not None and m.group(1).count('frame_tick::SceneEnd(') == 1, 'reader.h OnPresent: exactly one SceneEnd')

    h2 = texts['src/half2_hooks.h']
    pres = hook_body(h2, 'sub_82CB37C0')
    need(pres is not None, 'half2_hooks.h has the present hook sub_82CB37C0')
    if pres:
        p = strip_comments(pres)
        # S103 P7 review: the hook is two branches (#if WWE2K14_STREAM() / #else / #endif, P9). Each is read alone: ON = the reader's present OR frame_tick's (one if / else, each called once),
        # OFF = frame_tick's present only (once, never the reader's).
        i_on = p.find('#if WWE2K14_STREAM()')
        i_else = p.find('#else', i_on + 1) if i_on >= 0 else -1
        i_end = p.find('#endif', i_else + 1) if i_else >= 0 else -1
        need(0 <= i_on < i_else < i_end, 'present hook: the boundary is split #if WWE2K14_STREAM() / #else / #endif')
        on_b = p[i_on:i_else] if 0 <= i_on < i_else else ''
        off_b = p[i_else:i_end] if 0 <= i_else < i_end else ''
        m = re.search(r'if \(wwe2k14::rdr_reader::ReaderOn\(\)\s*\) wwe2k14::rdr_reader::OnPresent\(ctx, base\);\s*else wwe2k14::frame_tick::StreamFreePresent\(base, ctx\.r3\.u32, ctx\.r4\.u32\);', on_b)
        need(m is not None, 'present hook (stream build): the reader\'s present OR frame_tick::StreamFreePresent (one if / else)')
        need(on_b.count('StreamFreePresent(') == 1 and on_b.count('rdr_reader::OnPresent(') == 1, 'present hook (stream build): StreamFreePresent( once and rdr_reader::OnPresent( once')
        need(off_b.count('StreamFreePresent(') == 1 and off_b.count('rdr_reader::OnPresent(') == 0, 'present hook (no stream): StreamFreePresent( once, no reader present')
        k = p.find('KoLatchAtPresent')
        need(m is not None and k > i_on + m.end() - 1, 'present hook: KoLatchAtPresent after the boundary')
        need(p.find('StreamFreePresent') < p.find('__imp__sub_82CB37C0'), 'present hook: the boundary before the original (as the reader\'s was)')
    gam = hook_body(h2, 'sub_82CBABB0')
    need(gam is not None, 'half2_hooks.h has the gamma hook sub_82CBABB0')
    if gam:
        g = strip_comments(gam)
        i = g.find('frame_tick::OnGammaRamp(')
        line = g[g.rfind('\n', 0, i) + 1:g.find('\n', i)] if i >= 0 else ''
        need(i >= 0 and 'ReaderOn' not in line, 'gamma hook: frame_tick::OnGammaRamp outside the ReaderOn() gate')

    tr = hook_body(texts['src/tail_release_hook.h'], 'sub_82C3BB38')
    need(tr is not None and 'frame_tick::FinishTailAtRelease()' in tr and 'rdr_reader::TailFinishAt' not in strip_comments(tr),
         'release hook: frame_tick::FinishTailAtRelease, no reader call')

    for rel, s in texts.items():
        if not rel.startswith('src/') or rel in ('src/renderer_game/frame_tick.h',) or rel.startswith('src/renderer_scene/live_scene'):
            continue
        s2 = strip_comments(s)
        for fn in ('EndFrame', 'LatchFrame', 'EmitLatched'):
            if re.search(r'rdr_scene::live::%s\s*\(' % fn, s2) or re.search(r'\blive::%s\s*\(' % fn, s2):
                need(False, '%s calls the scene writer\'s %s outside frame_tick.h' % (rel, fn))
    return bad


def load_src(root):
    texts = {}
    parts = {os.path.realpath(str(x)) for x in part_files(os.path.join(root, 'src'))}   # a split parent is read with its parts expanded: a part is not read again
    for dp, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if f.endswith(('.h', '.cpp', '.inc')):
                p = os.path.join(dp, f)
                if os.path.realpath(p) in parts:
                    continue
                texts[os.path.relpath(p, root).replace('\\', '/')] = read_path(p)
    return texts


def selftest_lints(root):
    """Planted copies of the pre-P6 wiring: each must fail the lints."""
    base = load_src(root)
    twins = {}
    t = dict(base)   # the old present hook: the reader only
    t['src/half2_hooks.h'] = t['src/half2_hooks.h'].replace('\n  else wwe2k14::frame_tick::StreamFreePresent(base, ctx.r3.u32, ctx.r4.u32);', '')
    twins['hook_reader_only'] = t
    thumb = "  if (wwe2k14::readbacks_start::On()) wwe2k14::thumb_off::OnPresent(ctx, base);   // S111 RBSWITCH: the start value\n"   # S103 RBC (thumb_defer_off.h): the no-stream branch also posts the replay thumbnails
    off_line = "  wwe2k14::frame_tick::StreamFreePresent(base, ctx.r3.u32, ctx.r4.u32);\n" + thumb + "#endif\n  rex::graphics::CommandProcessor* cp = Cp();"
    t = dict(base)   # the no-stream branch loses its boundary (a build with no stream would post no tick)
    t['src/half2_hooks.h'] = t['src/half2_hooks.h'].replace('\r\n', '\n').replace(off_line, thumb + "#endif\n  rex::graphics::CommandProcessor* cp = Cp();")
    twins['hook_off_branch_empty'] = t
    t = dict(base)   # a second call in the no-stream branch (two ticks per present)
    t['src/half2_hooks.h'] = t['src/half2_hooks.h'].replace('\r\n', '\n').replace(off_line, off_line.replace('#endif', off_line.split('\n')[0] + '\n#endif', 1))
    twins['hook_off_branch_twice'] = t
    t = dict(base)   # the reader calling the writer itself again
    t['src/renderer_adapter/reader.h'] = t['src/renderer_adapter/reader.h'].replace('wwe2k14::frame_tick::SceneEnd(guest_frame,', 'rdr_scene::live::EndFrame(guest_frame,', 1)
    twins['reader_endframe'] = t
    t = dict(base)   # the release hook calling the reader
    t['src/tail_release_hook.h'] = t['src/tail_release_hook.h'].replace('wwe2k14::frame_tick::FinishTailAtRelease();', 'wwe2k14::rdr_reader::TailFinishAt(rdr_adapter::tail::kAtRelease);')
    twins['release_reader'] = t
    t = dict(base)   # the gamma table behind the reader gate
    t['src/half2_hooks.h'] = t['src/half2_hooks.h'].replace('  wwe2k14::frame_tick::OnGammaRamp(ctx.r4.u32, base);', '  if (wwe2k14::rdr_reader::ReaderOn()) wwe2k14::frame_tick::OnGammaRamp(ctx.r4.u32, base);')
    twins['gamma_gated'] = t
    bad = 0
    for name, tx in twins.items():
        if tx == base:
            print('FAIL: lint twin %s did not change the sources (the planted edit no longer applies)' % name)
            bad += 1
            continue
        print('-- lint twin %s' % name)
        if not lints(tx):
            print('FAIL: lint twin %s was not caught' % name)
            bad += 1
        else:
            print('lint twin %s: caught' % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'p6')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'frame_tick_unit.exe')
    if not os.path.exists(CLANG):
        print('FAIL: no compiler at ' + CLANG)
        return 1
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-pthread', '-o', exe, os.path.join(root, 'tools', 'renderer', 'frame_tick_unit.cpp')],
                       capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for n, name in PLANTS.items():
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=120)
        caught = p.returncode == 1 and 'FAIL' in p.stdout
        print('twin %d %s: %s' % (n, name, 'caught' if caught else 'NOT CAUGHT'))
        if not caught:
            print(p.stdout[-1500:])
            return 1
    print('-- lints on this tree')
    bad = lints(load_src(root))
    print('lints: %d failed' % len(bad))
    if bad:
        return 1
    tw = selftest_lints(root)
    print('lint twins: %d not caught' % tw)
    return 1 if tw else 0


if __name__ == '__main__':
    sys.exit(main())
