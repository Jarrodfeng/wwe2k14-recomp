#!/usr/bin/env python3
r"""S103 lane RBC (stream-free GPU read-backs): the offline proof. 0 game runs, 0 Surface runs.

  py -I tools\readback\rbc_check.py --part unit|wiring [--root DIR]

  unit    tools\readback\rbc_unit.cpp: the tiled guest writer (8888 128x128 and 1280x720, tiled / linear, endian, the swizzle's channel order), the box scale, the site table;
          then four planted twins on the same code built with -DRBC_PLANTS (the wrong swap, a skipped endian swap, the tiled bit ignored, a nearest scale): each must make the unit exit 1.
  wiring  text lints: the OFF branch of the lock hook calls readback_ours::OnLock (and the stream build still calls OnTextureLock), the include is gated to the stream-free build, the switch
          wwe2k14_rdr_readbacks exists with default true, the stream build's WriteCopyBack is still in reader.h (not moved), and the site table lists every lock return RBK.md names.
Exit 0 = pass."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text)
    if not ok:
        FAILS.append(text)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def scratch():
    d = os.path.join(os.environ.get('TEMP', ROOT), 'rbc_check')
    os.makedirs(d, exist_ok=True)
    return d


PLANTS = [(1, 'the wrong swap (the fetch swizzle is ignored)'), (2, 'the endian swap is skipped'), (3, 'the tiled bit is ignored'), (4, 'the scale is a nearest pick, not a box mean'),
          (5, 'the site guard (size, packed mips, writable pages) is skipped'), (6, 'a site with no 3D scene writes black anyway'),
          (7, 'the scene freshness ignores the frame age (a stale arena is handed out)'), (8, 'a refused scene copy is never retried'),
          (9, 'a screenshot refused while a scene copy holds the key fails instead of waiting'),
          (10, 'RBH: R8 (the render thread) waits for a scene when it has no R9 picture'), (11, 'RBH: R8 ignores the game\'s crop rect (always the whole picture)'),
          (12, 'RBH: R9 with no scene leaves the memory as it was (garbage into the reel save)'),
          (13, 'RBH F1: R8 ignores the destination rect (stretches the crop over the whole 128x128 instead of the letterbox)')]


def part_unit():
    d = scratch()
    src = os.path.join(ROOT, 'tools', 'readback', 'rbc_unit.cpp')
    inc = ['-I', os.path.join(ROOT, 'src')]
    exe = os.path.join(d, 'rbc_unit.exe')
    rc, text = run([CLANG, '-std=c++20'] + inc + [src, '-o', exe])
    say(rc == 0, 'rbc_unit builds' + ('' if rc == 0 else ': ' + text[-400:]))
    if rc != 0:
        return
    rc, text = run([exe])
    say(rc == 0 and 'rbc_unit: PASS' in text, 'rbc_unit: the writer and the site table behave (%d lines ok)' % text.count('ok  '))
    pexe = os.path.join(d, 'rbc_unit_plants.exe')
    rc, text = run([CLANG, '-std=c++20', '-DRBC_PLANTS'] + inc + [src, '-o', pexe])
    say(rc == 0, 'rbc_unit builds with the planted twins' + ('' if rc == 0 else ': ' + text[-400:]))
    if rc != 0:
        return
    rc, text = run([pexe, '--plant', '0'])
    say(rc == 0, 'the plant build with no plant selected passes (the twins are off by default)')
    for n, name in PLANTS:
        rc, text = run([pexe, '--plant', str(n)])
        say(rc == 1 and 'FAIL' in text, 'planted twin %d (%s) is caught by the unit (exit %d)' % (n, name, rc))


def scene_text(host_cpp):
    """The scene read-back functions of the host (RequestScene .. just before CaptureShown), as text."""
    i = host_cpp.find('// ---- S103 RBC: scene read-backs')
    j = host_cpp.find('bool SceneHost::CaptureShown(', i)
    return host_cpp[i:j] if 0 <= i < j else ''


def scene_lints(host_cpp):
    """Items 2 / 3 as text rules: the scene capture reads `out` (last_out_) of an OURS frame (out_ok_), never the shown picture F, and applies NO display curve. Returns the list of broken rules."""
    t = scene_text(host_cpp)
    bad = []
    if not t:
        return ['the scene read-back block is missing']
    if 'Decide(false, SceneFresh(guest_frame))' not in t or 'if (SceneFresh(last_handled_frame_) &&' not in t:
        bad.append('QueueScene / ServeSceneSync do not ask SceneFresh with their own frame')
    if 'last_out_' not in t or 'SceneFresh(' not in t:
        bad.append('does not read last_out_ through SceneFresh (an OURS frame within 2 guest frames)')
    if 'Shown()' in t or 'f_,' in t or 'ref.image = f_' in t:
        bad.append('reads the shown picture / F (2D rows and the HUD would be in it)')
    if 'curve' in t:
        bad.append('touches the display curve (the ramp would be applied twice)')
    return bad


def part_wiring():
    tx = read('src/d3d_textures.h')
    i = tx.find('REX_HOOK_RAW(sub_82CA7EB8)')
    body = tx[i:tx.find('\n}\n', i)] if i >= 0 else ''
    say('#if WWE2K14_STREAM()' in body and 'OnTextureLock(ctx, base)' in body and '#else' in body and 'readback_ours::OnLock(ctx, base)' in body and body.index('OnTextureLock') < body.index('#else') < body.index('OnLock'),
        'the lock hook: the stream build calls OnTextureLock, the stream-free build calls readback_ours::OnLock (the #else branch)')
    say(re.search(r'#else\s*\n#include "renderer_game/readback_ours.h"', tx) is not None, 'readback_ours.h is included only in the stream-free build (the #else of the thumb_defer include)')
    ro = read('src/renderer_game/readback_ours.h')
    say('REXCVAR_DEFINE_BOOL(wwe2k14_rdr_readbacks, true,' in ro, 'the switch wwe2k14_rdr_readbacks exists with default true')
    say('if (!REXCVAR_GET(wwe2k14_rdr_readbacks)) return;' in ro, 'the switch off = the lock hook does nothing (today\'s behaviour)')
    rd = read('src/renderer_adapter/reader.h')
    say('inline bool WriteCopyBack(' in rd, 'the stream build\'s WriteCopyBack is still in reader.h (not moved)')
    for lr in ('0x8274FADC', '0x822A635C', '0x8341B75C', '0x83327B20', '0x8334398C', '0x82295A2C', '0x8228F3FC'):
        say(lr in ro, 'the site table names the lock return %s' % lr)
    th = read('src/renderer_game/thumb_scope_hook.h')
    say('REX_HOOK_RAW(sub_8274FF48)' in th and 'Ld32(base, obj + 24) == 2' in th and '__imp__sub_8274FF48(ctx, base)' in th, 'the thumbnail scope hook: sub_8274FF48, [r3+24] == 2, the original always runs')
    say('renderer_game/thumb_scope_hook.h' in tx, 'd3d_textures.h includes the scope hook (both builds)')
    tp = read('src/scene_live/quads2d_tap.h')
    i = tp.find('inline void OnOp19(')
    j = tp.find('OnOp19Site(', i)
    k = tp.find('thumb_scope::NoteRestore', i)
    say(0 < i < k < j, 'quads2d_tap OnOp19: the frame-restore site is counted habit_thumb_restore BEFORE the fallback census notes it (no hole, no unknown_site)')
    core = read('src/renderer_scene/quads2d_core.h')
    say('{0x82BC32B8u, 0x82BC3098u, SiteClass::Leaf2D, kQkNone}' in core, 'quads2d_core.h keeps site 0x82BC32B8 as Leaf2D (a hole anywhere but the thumbnail scope)')
    host = read('src/renderer_host/scene_host.cpp')
    say(scene_lints(host) == [], 'scene_host.cpp scene read-backs: out of the newest OURS 3D frame, never F, no display curve' + ('' if scene_lints(host) == [] else ': ' + '; '.join(scene_lints(host))))
    twins = [('the capture reads the shown picture', host.replace('ref.image = last_out_;', 'ref.image = Shown();', 1)),
             ('the capture applies the curve', host.replace('// ---- S103 RBC: scene read-backs', '// ---- S103 RBC: scene read-backs\n// curve_set_', 1)),
             ('the capture is served from a stale frame (SceneFresh dropped)', host.replace('SceneFresh(', 'true || SceneFresh(', 40))]
    for name, mutated in twins:
        say(mutated != host and scene_lints(mutated) != [], 'planted twin (%s) is caught by the scene lint' % name)
    hs = read('src/renderer_host/host_start.cpp')
    say('g_scene_request_owner = ' in hs and 'g_scene_capture_owner = ' in hs and 'RequestScene' in hs and 'CaptureScene' in hs, 'host_start.cpp sets the scene owners next to g_shown_frame_owner')
    ro_text = read('src/renderer_game/readback_ours.h')
    i = ro_text.find('if (site.kind == Kind::Thumb)')
    j = ro_text.find('// R2:', i)
    say(0 < i < j and 'g_scene_capture_owner' not in ro_text[i:j] and 'g_scene_request_owner' not in ro_text[i:j], 'readback_ours.h: the R1 branch of the lock never captures (it only counts; the deferral is thumb_defer_off.h)')
    # RBH: R8 runs on the render thread (op-68 callback): neither its core (ServeThumb) nor its game-side body may ask for a scene; R9 (worker job) does, and drops the old reel first.
    def reel_rules(t):
        a, b = t.find('inline void ServeReelThumb('), t.find("// The OFF build's lock body")
        c, d = t.find('inline readback_tile::Why ServeThumb('), t.find('}  // namespace wwe2k14::readback_ours')
        bad = []
        if not (0 < a < b and 0 < c < d):
            return ['the R8 functions are missing']
        for name, seg in (('ServeReelThumb', t[a:b]), ('ServeThumb', t[c:d])):
            if re.search(r'g_scene_capture_owner|g_scene_request_owner|CaptureScene|RequestScene|wait_for|\.wait\(|sleep', seg):
                bad.append('%s asks for a scene or waits (R8 runs on the render thread)' % name)
        if 'ReelDrop();' not in t[t.find('inline readback_tile::Why ServeReelPictureCore('):t.find('inline readback_tile::Why ServeThumb(')]:
            bad.append('ServeReelPictureCore does not drop the previous reel picture')
        return bad
    say(reel_rules(ro_text) == [], 'readback_ours.h: R8 (ServeThumb / ServeReelThumb) never captures or waits, R9 drops the old reel picture' + ('' if reel_rules(ro_text) == [] else ': ' + '; '.join(reel_rules(ro_text))))
    mut = ro_text.replace('  const bool four_three = Ld32(base, 0x8392DC3C) != 0;', '  const bool four_three = Ld32(base, 0x8392DC3C) != 0;\n  std::vector<uint8_t> x; wwe2k14::g_scene_capture_owner(x, src_w, src_h, 500, nullptr);', 1)
    say(mut != ro_text and reel_rules(mut) != [], 'planted twin (R8 waits for a scene on the render thread) is caught by the reel lint')
    say(re.search(r'ReelPicture \|\| site.kind == Kind::ReelThumb\) \{[^}]*wwe2k14_rdr_reel_readbacks', ro_text) is not None and 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_reel_readbacks, true,' in ro_text, 'the reel read-backs have their own switch wwe2k14_rdr_reel_readbacks, default true')
    td = read('src/renderer_game/thumb_defer_off.h')
    say('g_scene_capture_owner' not in td and 'CaptureScene' not in td and 'g_scene_request_owner(' in td, 'thumb_defer_off.h: R1 only ever posts a request (RequestScene), it never waits for a picture')
    say(all(('REX_HOOK_RAW(sub_%s)' % a) in td for a in ('8274FA70', '8274FC60', '82750580')), 'thumb_defer_off.h hooks the job, the file writer and the destructor')
    hh = read('src/half2_hooks.h')
    say('thumb_off::OnPresent(ctx, base)' in hh, 'the stream-free present hook delivers landed thumbnails (thumb_off::OnPresent)')
    say(re.search(r'#if !WWE2K14_STREAM\(\)[^\n]*\n#include "renderer_game/thumb_scope_hook.h"', tx) is not None, 'd3d_textures.h includes the thumbnail scope hook only in the stream-free build')
    say(re.search(r'#if !WWE2K14_STREAM\(\)[^\n]*\n#include "../renderer_game/thumb_scope.h"', tp) is not None and re.search(r'#if !WWE2K14_STREAM\(\)[^\n]*\n\s*if \(wwe2k14::thumb_scope::Active\(\)', tp) is not None, 'quads2d_tap.h: the thumbnail scope include and its early return are stream-free only')
    say('scene_rules::Fresh(' in read('src/renderer_host/scene_host.h') and 'scene_rules::Decide(' in host and 'scene_rules::Retry(' in host and 'scene_rules::ShotRefusalIsWait(' in host and 'last_out_frame_ = j' in host,
        'scene_host: the scene read-backs use the pure rules (fresh within 2 frames, shot priority, retries) and stamp last_out_frame_ where last_out_ is set')
    say('VirtualQuery(' not in ro_text and 'GetPhysicalHeap' in ro_text and 'kMemoryProtectWrite' in ro_text, 'readback_ours.h: the writable check is the physical heap\'s own protect table (every platform), not VirtualQuery')
    say('restoreGuestBits(saved.fpscr.csr)' in td and 'ScaleBox(rgba.data(), pw, ph, kSize, kSize' in td and 'Why::TimedOut' in td, 'thumb_defer_off.h: fpscr restored before ctx = saved, the 128x128 scale in the host callback, timeouts have their own census reason')
    say('REXLOG' in ro and 'readback_unserved' in ro, 'the census names readback_unserved{lr}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--part', required=True, choices=['unit', 'wiring'])
    ap.add_argument('--root', default=None)
    a = ap.parse_args()
    global ROOT
    if a.root:
        ROOT = os.path.abspath(a.root)
    {'unit': part_unit, 'wiring': part_wiring}[a.part]()
    print('rbc_check %s: %s' % (a.part, 'FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
