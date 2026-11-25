#!/usr/bin/env python3
"""S103 landing 2, piece P7 H1: builds and runs tools\\renderer\\host_core_unit.cpp (src\\renderer_host\\host_core.h: the scene host's mailbox, thread loop and frame decisions, and the
present size frame_tick's Tick carries: no game, no GPU, 0 game runs), then its planted twins (each must exit 1), then the wiring lints:
  - the host's Start / AttachToGame are reachable only from the OFF branch (#if !WWE2K14_KEEP_STREAM) of the app: in a stream build the host is compiled and never started;
  - the poster registers on frame_tick::g_post only in host_start.cpp (never from a stream-build path);
  - scene_host.cpp includes no stream header and no game / SDK header (the headless rig builds it);
  - host_start.cpp defines StartForGame / StopForGame only under `#if !WWE2K14_STREAM()` (an ON call fails to link); the test seam is named only in scene_host.{h,cpp};
  - every player-visible change callback the stream path registers (reader.h) has the same registration in host_start.cpp (photo mode's gpu_hide_hud);
  - the census is honest: while the host draws pass-Unknown rows, fallback_census.h kWired[PassUnknown] is true.
  py -I tools\\renderer\\host_core_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'draw_fault_as_miss', 2: 'no_record_black', 3: 'poster_draws', 4: 'oldest_wins', 5: 'curve_lost', 6: 'hold_without_last3d', 7: 'take_after_stop', 8: 'idle_never', 9: 'repeat_ignores_holds', 10: 'no_row_cache', 11: 'tween_ignores_waiting', 12: 'tween_ignores_cut', 13: 'tween_ignores_cold', 14: 'tween_ignores_scene_change', 15: 'draw_unfed', 16: 'menu_hook_is_3d', 17: 'any_stamp_is_3d', 18: 'rate_draws_all', 19: 'rate_debt_unpaid', 20: 'rate_carry_lost', 21: 'rate_debt_unbounded', 22: 'rate_contentless_pays', 23: 'feed_counts_calls', 24: 'repeat_raw_key', 25: 'key_wiring_dropped', 26: 'hidden_drawn', 27: 'stage_word_ignored', 28: 'chars_ignored', 31: 'a4_stamp_kept', 32: 'a4_no_k_guard', 35: 'scene_on_f_hold', 37: 'no_scene_sticky', 38: 'load_follow_any_pac',
          39: 'start_no_wait', 40: 'start_old_build', 41: 'start_stall_holds', 42: 'start_script_waits', 43: 'start_match_in_entrance', 44: 'start_arm_kept', 45: 'start_story_class_only', 46: 'start_card_down_holds', 47: 'start_stale_host_shut',
          48: 'start_card_pending_only', 49: 'start_bodies_no_restamp', 50: 'start_bodies_card_down', 51: 'start_bodies_kept'}   # 29, 30, 33, 34 retired with the S105 A3 camera gate (S110 ENTRBLACK); 39-48 S110 ENTRWAIT2; 49-51 S110 ENTRBODIES


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def if_depth_of(text, pos):
    """The preprocessor conditions open at `pos`: a list of the `#if` lines (an #else flips the top one)."""
    stack = []
    for ln in text[:pos].split('\n'):
        t = ln.strip()
        if re.match(r'#\s*if', t):
            stack.append(t)
        elif re.match(r'#\s*else', t) and stack:
            stack[-1] = '!(' + stack[-1] + ')'
        elif re.match(r'#\s*endif', t) and stack:
            stack.pop()
    return stack


def lints(texts):
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)

    start_calls = 0
    for rel, s in texts.items():
        if not rel.startswith('src/') or rel.startswith('src/renderer_host/'):
            continue
        c = strip_comments(s)
        for m in re.finditer(r'scene_host::(AttachToGame|StartForGame)\s*\(', c):
            start_calls += 1
            # the code with comments blanked keeps positions only if we scan the original: redo on the original text with the same match offset by searching the original
            conds = if_depth_of(s, s.find(m.group(0)))
            ok = any(re.fullmatch(r'#\s*if\s+!\s*(WWE2K14_KEEP_STREAM|WWE2K14_STREAM\(\)).*', x) or re.fullmatch(r'!\(#\s*if\s+WWE2K14_KEEP_STREAM.*\)', x) for x in conds)
            need(ok, '%s: scene_host::%s is reached only under #if !WWE2K14_KEEP_STREAM' % (rel, m.group(1)))
    need(start_calls >= 1, 'the app starts the host (%d call site(s) outside src\\renderer_host)' % start_calls)
    for rel, s in texts.items():
        if rel.startswith('src/') and not rel.startswith('src/renderer_host/') and re.search(r'frame_tick::g_post\s*\.\s*store', strip_comments(s)):
            need(False, '%s registers a frame_tick poster outside the host' % rel)
    for rel, s2 in texts.items():
        if rel.startswith('src/') and rel not in ('src/renderer_host/scene_host.h', 'src/renderer_host/scene_host.cpp') and re.search(r'\bTestSeam\b|SetTestSeam', strip_comments(s2)):
            need(False, '%s uses the scene host test seam (rig only)' % rel)
    hs = strip_comments(texts['src/renderer_host/host_start.cpp'])
    need(hs.count('g_post.store(') >= 1, 'host_start.cpp registers the poster')
    hs_raw = texts['src/renderer_host/host_start.cpp']
    for fn in ('StartForGame', 'StopForGame'):
        m = re.search(r'^(?:bool|void)\s+' + fn + r'\s*\(\s*\)\s*\{', hs_raw, re.M)
        conds = if_depth_of(hs_raw, m.start()) if m else []
        need(m is not None and any(re.fullmatch(r'#\s*if\s+!\s*WWE2K14_STREAM\(\).*', x) for x in conds), 'host_start.cpp: %s is defined only under #if !WWE2K14_STREAM() (an ON call fails to link)' % fn)
    hh = texts['src/renderer_host/host_start.h']
    m = re.search(r'bool\s+StartForGame', hh)
    need(m is not None and any(re.fullmatch(r'#\s*if\s+!\s*WWE2K14_STREAM\(\).*', x) for x in if_depth_of(hh, m.start())), 'host_start.h: StartForGame is declared only under #if !WWE2K14_STREAM()')
    # a player-visible callback the stream path registers needs its twin here (the host is the only renderer in an OFF build)
    stream_cbs = set(re.findall(r'RegisterChangeCallback\(\s*"([^"]+)"', strip_comments(texts['src/renderer_adapter/reader.h'])))
    need(bool(stream_cbs), 'reader.h registers change callbacks (%s)' % ', '.join(sorted(stream_cbs)))
    for cb in sorted(stream_cbs):
        need(re.search(r'RegisterChangeCallback\(\s*"%s"' % re.escape(cb), hs) is not None, 'host_start.cpp registers the callback "%s" the stream path registers (reader.h)' % cb)
    # S103 HN B1: the stream-free start must store the presence switches (bits 10 crowd / 11 particles are never stamped while presence::g_crowd / g_particles stay false: LiveSink::Start never runs here)
    ss = strip_comments(texts.get('src/scene_live/scene_start_switches.h', ''))
    need('presence::g_crowd.store(' in ss and 'presence::g_particles.store(' in ss and 'hold::ApplyHold(' in ss, 'scene_start_switches.h applies presence::g_crowd / g_particles and the hold mask')
    # S103 SPB: both starts call start::ApplyStartSettings() (src/start_settings.cpp), which applies the presence switches through scene_start_switches.h
    need('start::ApplyStartSettings(' in hs, 'host_start.cpp (the OFF init) applies the scene start switches (presence::g_crowd / g_particles) through the shared helper')
    need('live::start::Apply(' in strip_comments(texts.get('src/start_settings.cpp', '')), 'start_settings.cpp applies the presence switches through scene_start_switches.h (the one helper)')
    need('start::ApplyStartSettings(' in strip_comments(texts['src/renderer_adapter/live.cpp']), 'live.cpp applies the same switches through the same helper (not a copy)')
    need('RegisterQuitFlush' in hs and 'UnregisterQuitFlush' in hs, 'host_start.cpp: the SDK quit flush is registered (the app leaves with a hard exit) and unregistered')
    # S103 (ANS gaps 1 and 2): the stream-free start answers a failed renderer start like the stream path (the player's message and the exit), and the launcher's 30 fps mode paces the host
    # S103 CVO (15d15f9ed): host_start.cpp no longer includes start_failure_show.h (its headers define cvars, one definition per exe): StartFailed calls the sink the app sets, and the app's sink calls ShowStartFailure.
    app_code = strip_comments(texts['src/wwe2k14_app.h'])
    need(re.search(r'void\s+StartFailed\(.*?\[host\] the scene host could not start.*?g_start_failure_sink\(error,\s*card\).*?host->Start\([^)]*\)\s*\)\s*\{\s*StartFailed\(', hs_raw, re.S) is not None,
         'host_start.cpp: a failed host start logs `[host] the scene host could not start` and calls the start-failure sink (g_start_failure_sink)')
    need(re.search(r'g_start_failure_sink\s*=\s*\[[^\]]*\]\([^)]*\)\s*\{[^}]*ShowStartFailure\(', app_code, re.S) is not None,
         'wwe2k14_app.h: the start-failure sink calls ShowStartFailure (the stream path\'s message / exit)')
    need('REXCVAR_GET(wwe2k14_rdr_test_fail_start)' in hs and 'IsDevBuild()' in hs, 'host_start.cpp: --wwe2k14_rdr_test_fail_start (+dev only) fails the start on purpose, as the stream path')
    need(re.search(r'requested\s*==\s*30\)\s*\{.*?draw_vblanks\s*=\s*2', hs, re.S) is not None, 'host_start.cpp: the launcher\'s 30 fps (step_hz 30) sets HostOptions::draw_vblanks = 2')
    need('StartFailedNoWindow' in app_code and re.search(r'void\s+StartFailed\(.*?g_start_failure_sink\(', hs_raw, re.S) is not None, 'the app takes the start-failure path when there is no game window (StartFailedNoWindow -> StartFailed -> the sink)')
    need(re.search(r'rate_\.Skip\(\s*opts_\.draw_vblanks', strip_comments(texts['src/renderer_host/scene_host.cpp'])) is not None, 'scene_host.cpp: Handle skips the ticks the 30 fps pacing does not draw (rate_.Skip)')
    # the census honesty rule: a drawer that draws pass-Unknown rows must have the census wired for pass_unknown
    sc = strip_comments(texts['src/renderer_host/scene_host.cpp'])
    fc = strip_comments(texts['src/renderer_game/fallback_census.h'])
    mw = re.search(r'kWired\[[^\]]*\]\s*=\s*\{([^}]*)\}', fc)
    wired = [x.strip() for x in mw.group(1).split(',')] if mw else []
    kinds = re.search(r'enum class Kind[^{]*\{([^}]*)\}', fc)
    names = [x.strip().split('=')[0].strip() for x in kinds.group(1).split(',')] if kinds else []
    pu = names.index('PassUnknown') if 'PassUnknown' in names else -1
    draws_unknown = 'kQpUnknown' in sc or 'kQpUnknown' in strip_comments(texts['src/renderer_host/row_fate.h'])   # S105 A4B: FateOf moved to row_fate.h
    need(pu >= 0 and pu < len(wired) and (wired[pu] == 'true' or not draws_unknown), 'fallback_census.h: kWired[PassUnknown] is true while the host draws pass-Unknown rows (%s)' % (wired[pu] if 0 <= pu < len(wired) else '?'))
    need('NotePassUnknown(' in sc, 'scene_host.cpp notes pass_unknown rows in the census (NotePassUnknown)')
    need(re.search(r'draw_unknown_pass\s*=\s*false', texts['src/renderer_host/scene_host.h']) is not None and 'draw_unknown_pass' not in strip_comments(texts['src/renderer_host/host_start.cpp']),
         'HostOptions::draw_unknown_pass defaults false and the game (host_start.cpp) never sets it (the rig only)')
    sh_ = texts['src/renderer_host/scene_host.cpp'] + texts['src/renderer_host/scene_host.h']
    for pat, what in ((r'#include\s*[<"][^>"]*rex/', 'no REX / SDK include'), (r'#include\s*"[^"]*renderer_adapter/(reader|binder|live|capture)[a-z_]*\.h"', 'no stream header'),
                      (r'#include\s*"[^"]*frame_tick\.h"', 'no frame_tick include (the poster lives in host_start.cpp)')):
        need(re.search(pat, sh_) is None, 'scene_host.{h,cpp}: ' + what)
    # S103 FLAT F2: the stream-free stamp rule is wired. The particle node hook asks scene_skip::Ours(Hook::Particles) only on NoteNode's answer, under #if !WWE2K14_STREAM(); NoteNode returns the rule.
    sn_raw = texts['src/scene_live/scene_skip_nodes.h']
    m = re.search(r'if\s*\(\s*ask\s*&&\s*wwe2k14::scene_skip::Ours\(\s*::scene_skip::Hook::Particles\s*\)', sn_raw)
    need(m is not None, 'scene_skip_nodes.h: the sub_82254E30 hook asks scene_skip::Ours(Hook::Particles) only when `ask` (the stamp rule) says so')
    ma = re.search(r'const bool ask = asks_scene;', sn_raw)
    need(ma is not None and any(re.fullmatch(r'#\s*if\s+!\s*WWE2K14_STREAM\(\).*', x) for x in if_depth_of(sn_raw, ma.start())), 'scene_skip_nodes.h: `ask` follows the rule only under #if !WWE2K14_STREAM() (the stream build asks as before)')
    need(re.search(r'asks_scene\s*=\s*wwe2k14::scene_particle_glue::NoteNode\(', strip_comments(sn_raw)) is not None, 'scene_skip_nodes.h: `asks_scene` is NoteNode\'s result')
    pg = strip_comments(texts['src/scene_live/particle_glue.h'])
    need(re.search(r'inline\s+bool\s+NoteNode\(.*?return\s+scene_stamp_rule::ParticleCallAsksScene\(', pg, re.S) is not None, 'particle_glue.h: NoteNode returns scene_stamp_rule::ParticleCallAsksScene(...)')
    # F1: the flat test reads a Scene3D stamp through scene_kind::StampMakes3D in the OFF build only
    ld_raw = texts['src/renderer_scene/live_scene_draw.cpp']
    mf = re.search(r'scene_kind::StampMakes3D\(', ld_raw)
    need(mf is not None and any(re.fullmatch(r'#\s*if\s+!\s*WWE2K14_STREAM\(\).*', x) for x in if_depth_of(ld_raw, mf.start())), 'live_scene_draw.cpp: FlatFacts3D counts a Scene3D stamp through scene_kind::StampMakes3D under #if !WWE2K14_STREAM()')
    # S110 ENTRBODIES review: DrawLive draws the frame its BuildFrame made AFTER PublishStartWait; the held start's crowd prefetch (LiveCrowd::PrefetchArena -> SetArena) releases the old arena's crowd
    # models when the arena differs (Release: WaitIdle, then ReleaseModel), so it must never run on a frame that is about to draw: DrawLive says drew = true and StartCrowdIn returns before the prefetch.
    ldc = strip_comments(ld_raw)
    need(re.search(r'PublishStartWait\(rec\.header\.guest_frame,\s*true,\s*true\)', ldc) is not None, 'live_scene_draw.cpp: DrawLive publishes with drew = true (the held start\'s crowd prefetch never switches the arena under a frame about to draw)')
    need(re.search(r'bool\s+LiveSceneDraw::StartCrowdIn\(bool\s+drew\)\s*\{.*?if\s*\(\s*drew\s*\|\|.*?PrefetchArena', ldc, re.S) is not None, 'live_scene_draw.cpp: StartCrowdIn returns before PrefetchArena on a drawing frame')
    return bad


def load_src(root):
    texts = {}
    for dp, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if f.endswith(('.h', '.cpp', '.inc')):
                p = os.path.join(dp, f)
                texts[os.path.relpath(p, root).replace('\\', '/')] = open(p, encoding='utf-8', errors='replace').read()
    return texts


def selftest_lints(root):
    base = load_src(root)
    bad = 0
    t = dict(base)   # the host started from an ON path: the start call outside the OFF branch
    app = t['src/wwe2k14_app.h']
    # S103 CVO (15d15f9ed): the sink assignment (a few lines) now sits between the comments and the start call: lines up to the call that are not preprocessor lines are kept.
    m = re.search(r'#if\s+!\s*(?:WWE2K14_KEEP_STREAM|WWE2K14_STREAM\(\))[^\n]*\n((?:(?!\s*#)[^\n]*\n)*?[^\n]*scene_host::(?:AttachToGame|StartForGame)[^\n]*)\n', app)
    if not m:
        print('FAIL: lint twin start_in_on: the start call was not found in the app')
        return 1
    t['src/wwe2k14_app.h'] = app.replace(m.group(0), m.group(1) + '\n')
    print('-- lint twin start_in_on')
    if not lints(t):
        print('FAIL: lint twin start_in_on was not caught')
        bad += 1
    else:
        print('lint twin start_in_on: caught')
    twins = {}
    hs = base['src/renderer_host/host_start.cpp']
    t = dict(base)   # StartForGame defined for every build (an ON call would link)
    t['src/renderer_host/host_start.cpp'] = hs.replace('#if !WWE2K14_STREAM()\n// OFF builds only', '// OFF builds only')
    twins['start_unwrapped'] = t
    t = dict(base)   # photo mode's HUD hide left out of the host
    t['src/renderer_host/host_start.cpp'] = hs.replace('RegisterChangeCallback("gpu_hide_hud"', 'RegisterChangeCallback("gpu_hide_hud_x"')
    twins['no_hide_hud'] = t
    t = dict(base)   # a pass-Unknown drawer with a census that says n/a
    t['src/renderer_game/fallback_census.h'] = base['src/renderer_game/fallback_census.h'].replace('true, true, true, true, true, true, true, true}', 'true, true, true, true, true, true, false, true}')
    twins['census_unwired'] = t
    t = dict(base)   # the seam named outside scene_host.{h,cpp}
    t['src/renderer_host/host_start.cpp'] = hs + '\nvoid Oops() { TestSeam s; (void)s; }\n'
    twins['seam_in_start'] = t
    t = dict(base)   # S103 twin: a failed host start with no player message (the black screen)
    t['src/wwe2k14_app.h'] = base['src/wwe2k14_app.h'].replace('wwe2k14::rdr_reader::ShowStartFailure(', '(void)(')
    twins['start_failure_silent'] = t
    t = dict(base)   # S103 CVO twin: StartFailed no longer calls the sink (the app's message never shows)
    t['src/renderer_host/host_start.cpp'] = hs.replace('g_start_failure_sink(error, card);', ';')
    twins['start_sink_not_called'] = t
    t = dict(base)   # S103 twin: no game window = silent again
    t['src/wwe2k14_app.h'] = base['src/wwe2k14_app.h'].replace('StartFailedNoWindow', 'StartedNoWindow')
    twins['no_window_silent'] = t
    t = dict(base)   # S103 twin: the 30 fps mode not wired to the host
    t['src/renderer_host/host_start.cpp'] = hs.replace('o.draw_vblanks = 2;', 'o.draw_vblanks = 0;')
    twins['rate_not_wired'] = t
    t = dict(base)   # S103 twin: the host never skips a tick
    t['src/renderer_host/scene_host.cpp'] = base['src/renderer_host/scene_host.cpp'].replace('rate_.Skip(opts_.draw_vblanks', 'rate_.SkipNever(opts_.draw_vblanks')
    twins['rate_not_applied'] = t
    t = dict(base)   # B1 twin: the OFF init without the presence switches (the first-run bug: bits 10 / 11 never stamped)
    t['src/renderer_host/host_start.cpp'] = hs.replace('start::ApplyStartSettings(', 'start::ApplyStartSettingsX(')
    twins['no_presence_init'] = t
    sn = base['src/scene_live/scene_skip_nodes.h']
    t = dict(base)   # F2 twin: the particle hook asks again on every call
    t['src/scene_live/scene_skip_nodes.h'] = sn.replace('if (ask && wwe2k14::scene_skip::Ours(', 'if (wwe2k14::scene_skip::Ours(')
    twins['particles_ask_always'] = t
    t = dict(base)   # F2 twin: NoteNode no longer returns the rule
    t['src/scene_live/particle_glue.h'] = base['src/scene_live/particle_glue.h'].replace('return scene_stamp_rule::ParticleCallAsksScene(', 'return true || scene_stamp_rule::ParticleCallAsksScene_(')
    twins['notenode_not_rule'] = t
    t = dict(base)   # F1 twin: any stamp makes the frame 3D again
    t['src/renderer_scene/live_scene_draw.cpp'] = base['src/renderer_scene/live_scene_draw.cpp'].replace('scene_kind::StampMakes3D(', 'scene_kind::StampMakes3D_(')
    twins['stamp_any'] = t
    t = dict(base)   # S110 ENTRBODIES review twin: DrawLive publishes like Warm again (the crowd prefetch may release the arena its frame is about to draw)
    t['src/renderer_scene/live_scene_draw.cpp'] = base['src/renderer_scene/live_scene_draw.cpp'].replace('PublishStartWait(rec.header.guest_frame, true, true)', 'PublishStartWait(rec.header.guest_frame)')
    twins['crowd_prefetch_while_drawing'] = t
    for name, tx in twins.items():
        if tx == base:
            print('FAIL: lint twin %s did not change the sources (the planted edit no longer applies)' % name)
            bad += 1
            continue
        print('-- lint twin ' + name)
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
    out = os.path.join(root, 'out', 's103', 'p7h1')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'host_core_unit.exe')
    if not os.path.exists(CLANG):
        print('FAIL: no compiler at ' + CLANG)
        return 1
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-pthread', '-DWWE2K14_KEEP_STREAM=0', '-o', exe, os.path.join(root, 'tools', 'renderer', 'host_core_unit.cpp')], capture_output=True, text=True)
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
