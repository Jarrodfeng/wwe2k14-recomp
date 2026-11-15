#!/usr/bin/env python3
"""S102 lane K3: builds and runs src\\renderer_adapter\\skips_auto_test.cpp (GPU free, 0 game runs), its planted twins (each must FAIL), the two TEXT plants of skips_auto.h, then the wiring lints.
  py -I tools\\renderer\\skips_auto_unit.py [--root DIR]
The test: synthetic headroom traces through a closed-loop sim (steady keep-up, a sudden heavy stretch, a sawtooth, the bars and the dwell) and the frame-boundary gate.
Planted twins: 1 = no hysteresis (flaps, turns on while behind), 2 = turning on while behind (the good test ignores the queue and the skips), 5 = the vblank wait fed as busy (F2), 6 = rate skips
counted as skipped (F4), 7 = no learned on-minus-off cost / no quick-off block (F5, the limit cycle), 8 = windows spanning a pause (F7). Text plants (a twin that no longer applies FAILS as
DID NOT APPLY): `Allows()` follows `want` at once (the frame boundary is lost), `Allows()` ignores the switch (a run with the switch off would answer from `applied`).
Wiring lints (sources read as text): the switch is PerFrame default off in the table, the cvar is defined default false, live.cpp reads it every frame and feeds the controller, frame_stats.h
latches it once per guest frame after the refresh, DecideCall and the member cut ask Allows(), the census line is logged, the app arms the five skips only under the switch.
Exit 0 = pass, 1 = fail."""
import argparse, os, re, subprocess, sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ra = os.path.join(root, 'src', 'renderer_adapter')
    out = os.path.join(root, 'out', 'k3')
    os.makedirs(out, exist_ok=True)
    for need in (os.path.join(ra, 'skips_auto_test.cpp'), os.path.join(ra, 'skips_auto.h')):
        if not os.path.isfile(need):
            print('FAIL: input missing ' + need)
            return 1

    def build(src_dir, exe_name):
        exe = os.path.join(out, exe_name)
        b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-I', os.path.join(root, 'src'), '-I', src_dir, os.path.join(src_dir, 'skips_auto_test.cpp'), '-o', exe], capture_output=True, text=True)
        if b.returncode:
            print('FAIL: did not compile\n' + b.stdout + b.stderr)
            return None
        return exe

    exe = build(ra, 'skips_auto_test.exe')
    if not exe:
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode or 'all passed' not in r.stdout:
        print('FAIL: the unit test failed')
        return 1
    for plant in ('1', '2', '5', '6', '7', '8', '9', '10'):
        p = subprocess.run([exe, plant], capture_output=True, text=True)
        if p.returncode == 0 or 'FAIL' not in p.stdout:
            print('FAIL: planted twin %s was not caught (exit %d)' % (plant, p.returncode))
            return 1
        print('twin %s: caught (%d FAIL lines)' % (plant, p.stdout.count('FAIL ')))

    # text plants: a mutated copy of skips_auto.h in a scratch dir beside a copy of the test (the test includes "skips_auto.h" from its own dir first)
    with open(os.path.join(ra, 'skips_auto.h'), encoding='utf-8') as f:
        header = f.read()
    with open(os.path.join(ra, 'skips_auto_test.cpp'), encoding='utf-8') as f:
        test = f.read()
    allows = 'inline bool Allows() {\n  const bool enabled = g_enabled.load(std::memory_order_relaxed), applied = g_applied.load(std::memory_order_acquire);\n  return g_armed_by_k3.load(std::memory_order_relaxed) ? enabled && applied : !enabled || applied;\n}'
    chain_old = 'return g_warm.load(std::memory_order_relaxed) || AppliedAt(guest_frame) || AppliedAt(guest_frame - 1) || AppliedAt(guest_frame - 2);'
    plants = [
        ('the chains always run (the armed-off regression of ab11)', chain_old, 'return true;'),
        ('the chains stop one frame too early (a marker frame bound without them: holes)', chain_old, 'return g_warm.load(std::memory_order_relaxed) || AppliedAt(guest_frame);'),
        ('boundary lost (Allows follows want)', allows, allows.replace('enabled && applied : !enabled || applied', 'enabled && g_want.load(std::memory_order_relaxed) : !enabled || g_want.load(std::memory_order_relaxed)')),
        ('switch off still answers from applied (not armed)', allows, allows.replace(': !enabled || applied', ': applied')),
        ('armed run restores `!g_enabled ||` (F1: a mid-session switch-off turns every skip ON)', allows, allows.replace('? enabled && applied :', '? !enabled || applied :')),
    ]
    for i, (name, old, new) in enumerate(plants):
        d = os.path.join(out, 'plant%d' % i)
        os.makedirs(d, exist_ok=True)
        if old not in header:
            print('FAIL: text plant "%s" DID NOT APPLY (the line it replaces is gone)' % name)
            return 1
        planted = header.replace(old, new, 1)
        assert planted != header
        with open(os.path.join(d, 'skips_auto.h'), 'w', encoding='utf-8') as f:
            f.write(planted)
        with open(os.path.join(d, 'skips_auto_test.cpp'), 'w', encoding='utf-8') as f:
            f.write(test)
        pe = build(d, 'plant%d.exe' % i)
        if not pe:
            return 1
        p = subprocess.run([pe], capture_output=True, text=True)
        if p.returncode == 0:
            print('FAIL: text plant "%s" was not caught' % name)
            return 1
        print('text plant "%s": caught (%d FAIL lines)' % (name, p.stdout.count('FAIL ')))

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    sw, rd, lv = text('src/renderer_adapter/rdr_switches.h'), text('src/renderer_adapter/reader.h'), text('src/renderer_adapter/live.cpp')
    fs, dg, mt, app = text('src/frame_stats.h'), text('src/renderer_adapter/d3_guest_core.h'), text('src/scene_live/member_thin_hook.h'), text('src/wwe2k14_app.h')
    ft = text('src/renderer_adapter/live.cpp')
    bn = text('src/renderer_adapter/binder.h')
    bad = 0
    # review F6: every Message the SUBMIT side builds sets d3_chain (the default is "chains on": a read-back part that forgot it ran the chains on an armed-off run)
    def messages_ok(src):
        lines = src.split('\n')
        built = [i for i, l in enumerate(lines) if l.strip() == 'Message message;' and 'void LiveSink::' in '\n'.join(lines[max(0, i - 40):i])]
        return len(built) >= 3 and all(any('message.d3_chain = skips_auto::ChainFor(' in x for x in lines[i:i + 30]) for i in built)
    ok = messages_ok(lv)
    print(('ok   ' if ok else 'FAIL ') + 'live.cpp (F6): every Message the submit side builds (SubmitFrame, SubmitFrameData, DrawAndRead) sets d3_chain')
    bad += 0 if ok else 1
    dr = lv.replace('  message.d3_chain = skips_auto::ChainFor(guest_frame);   // S102 K3 review F6: a read-back part is a Message too (its default was "chains on")\n', '', 1)
    ok = dr != lv and not messages_ok(dr)
    print(('ok   ' if ok else 'FAIL ') + 'F6 twin: a read-back part that forgets d3_chain is caught' + ('' if dr != lv else ' (DID NOT APPLY)'))
    bad += 0 if ok else 1
    need = [
        (rd, 'skips_auto::NoteMarker();', 'reader.h (F2): the reader counts the markers it builds'),
        (lv, 'binder.D3NoteChain(message.d3_chain, message.guest_frame);', 'live.cpp (F1): the binder is told about every handled frame (the resume frame)'),
        (lv, 'binder.D3ChainsProven()', 'live.cpp (F7): the warm-up ends on the gates proven'),
        (lv, 'skips_ctl.WarmCheck()', 'live.cpp (F3): the warm-up checks what the chains cost before ON is applied'),
        (lv, '!note.skipped && skips_warm_seen', 'live.cpp (F7): only bound frames count toward the warm-up'),
        (bn, 'g_marker_off_chain.fetch_add', 'binder.h (F2): the marker-on-a-chain-off-frame counter'),
        (text('src/renderer_adapter/d3_binder.h'), 'not re-proved since the chains resumed', 'd3_binder.h (F1): Publish refuses a mark not re-proved since the chains resumed'),
        (text('src/scene_live/d3_arena_hook.h'), 'idle ? d3::CachePath::Unreadable', 'd3_arena_hook.h (F5): idle = no PredictPath'),
        (text('src/scene_live/d3_caster_hook.h'), 'idle ? d3::CachePath::Unreadable', 'd3_caster_hook.h (F5): idle = no PredictPath'),
        (text('src/scene_live/d3_ring_hook.h'), 'idle ? d3::CachePath::Unreadable', 'd3_ring_hook.h (F5): idle = no PredictPath'),
        (bn, 'd3_on_ = d3_chain_frame && d3::g_arena_d3', 'binder.h (lazy arming): the arena chain runs only on a frame the chains were wanted for'),
        (bn, 'd3c_on_ = d3_chain_frame && d3::g_caster_d3', 'binder.h (lazy arming): the caster chain likewise'),
        (bn, 'd3r_on_ = d3_chain_frame && d3::g_ring_d3', 'binder.h (lazy arming): the ring chain likewise'),
        (lv, 'binder.d3_chain_frame = message.d3_chain;', 'live.cpp: the binder is told per frame (carried by the message, decided at submit)'),
        (lv, 'message.d3_chain = skips_auto::ChainFor(guest_frame);', 'live.cpp: the submit side decides the chains for the frame'),
        (lv, 'if (d3_chain_now && message.d3_record) {', 'live.cpp: records are added only for a chain frame'),
        (lv, 'if (!lasting && d3_chain_now) binder.D3FrameSkipped(frame);', 'live.cpp: a skipped frame with the chains off takes no packets'),
        (lv, 'd3t::Line()', 'live.cpp logs the [rdr][d3_time] census line'),
        (lv, 'skip_wanted && waiting >= max_queued_now && !lasting_drawn_now ? 1u : 0u', 'live.cpp (F4): only a queue skip counts as skipped, never a rate skip'),
        (lv, 'note.busy_ns = skips_cpu.TakeNs();', 'live.cpp (F2): busy is the thread CPU time, not the wall time through the present'),
        (lv, 'rdr_sw::kSkipsAutoForce', 'live.cpp (F3a): the test-only force switch is read per frame'),
        (lv, 'skips_ctl.SceneChanged();', 'live.cpp (F5): a new announced arena is a scene change'),
        (sw, 'X(SkipsAutoForce, "wwe2k14_rdr_skips_auto_force", PerFrame, 0)', 'switch table (F3a): the force switch, per-frame, default 0'),
        (rd, 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_skips_auto_force, 0', 'reader.h defines the force switch (default 0)'),
        (app, 'skips_auto::SetArmedByK3(armed > 0);', 'wwe2k14_app.h (F1): the gate is told the skips were armed for the switch'),
        (mt, 'Mode::Off) : EffectiveMode(cut);', 'member_thin_hook.h (F3b): an automatic OFF is Mode::Off, not a census'),
        (sw, 'X(SkipsAuto, "wwe2k14_rdr_skips_auto", PerFrame, 0)', 'switch table: skips_auto per-frame, default off'),
        (rd, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skips_auto, false', 'reader.h defines skips_auto (default false)'),
        (lv, 'rdr_sw::BoolSeen(rdr_sw::kSkipsAuto, false, message.guest_frame)', 'live.cpp reads the switch every frame with the guest frame'),
        (lv, 'skips_auto::SetWant(apply);', 'live.cpp feeds the controller and publishes its decision'),
        (lv, 'skips_ctl.Reset();', 'live.cpp resets the controller when the switch goes off'),
        (lv, 'skips_auto::Line(skips_ctl.census()', 'live.cpp logs the [rdr][skips_auto] census line'),
        (fs, 'skips_auto::AtGuestFrame(rdr_sw::Bool(rdr_sw::kSkipsAuto), frame);', 'frame_stats.h latches the decision once per guest frame'),
        (dg, 'if (!in.wrestler && !skips_auto::Allows()) si.switch_on = false;', 'DecideCall: the caster / ring / arena calls ask Allows()'),
        (mt, 'const bool auto_off = !skips_auto::Allows();', 'member_thin_hook.h: the crowd member cut asks Allows()'),
        (app, 'wwe2k14_rdr_skips_auto") == "true"', 'wwe2k14_app.h arms the five skips only under the switch'),
    ]
    # the refresh comes BEFORE the latch in OnSwap (the latch reads the table value of THIS frame)
    i_ref, i_latch = fs.find('rdr_sw::RefreshFromCvars(frame);'), fs.find('skips_auto::AtGuestFrame(')
    ok = 0 <= i_ref < i_latch
    print(('ok   ' if ok else 'FAIL ') + 'frame_stats.h: the S0-4 refresh runs before the latch')
    bad += 0 if ok else 1
    # the state guard: the census / starved note is made only for a handled frame (after the draw path, before the released loop)
    i_note, i_rel = lv.find('skips_auto::FrameNote note;'), lv.find('for (uint32_t id : message.released) binder.ReleaseImage(id);')
    ok = 0 <= i_note < i_rel
    print(('ok   ' if ok else 'FAIL ') + 'live.cpp: the controller sees each handled frame after it was skipped or drawn')
    bad += 0 if ok else 1
    # F6: the latch runs before the frame number is published
    i_latch, i_pub = fs.find('skips_auto::AtGuestFrame('), fs.find('g_guest_frame.store(frame, std::memory_order_release);')
    ok = 0 <= i_latch < i_pub and '++g_guest_frame' not in fs
    print(('ok   ' if ok else 'FAIL ') + 'frame_stats.h (F6): the skips latch runs before the new guest frame number is published')
    bad += 0 if ok else 1

    # the member run: Allows() is asked ONCE, at the run's start (OnCrowdDrawBegin); a flip of g_applied inside a run changes nothing (the run's mode is in t_dec; DropMember and the CountRing bit never ask)
    def member_ok(src):
        a, b = src.find('inline Mode OnCrowdDrawBegin()'), src.find('inline void OnCrowdDrawRun(')
        return a >= 0 and b > a and src.count('skips_auto::Allows()') == 1 and a < src.find('skips_auto::Allows()') < b
    ok = member_ok(mt)
    print(('ok   ' if ok else 'FAIL ') + 'member_thin_hook.h: Allows() is asked once, inside OnCrowdDrawBegin (a run never changes its mind)')
    bad += 0 if ok else 1
    plant_mt = mt.replace('inline bool DropMember(uint32_t lr, bool tiled, bool floor_on) {', 'inline bool DropMember(uint32_t lr, bool tiled, bool floor_on) {\n  if (!skips_auto::Allows()) return false;', 1)
    ok = plant_mt != mt and not member_ok(plant_mt)
    print(('ok   ' if ok else 'FAIL ') + 'member run twin: Allows() asked again inside DropMember is caught' + ('' if plant_mt != mt else ' (DID NOT APPLY)'))
    bad += 0 if ok else 1
    for src, needle, what in need:
        ok = needle in src
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += not ok
    # review F5: the swap thread's armed-off decision, a micro-bench of the hook's own helpers (idle must not be slower than full)
    bench = os.path.join(root, 'tools', 'renderer', 'hook_path_bench.cpp')
    exe_b = os.path.join(root, 'out', 'k3', 'hook_path_bench.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-I', os.path.join(root, 'src'), '-I', ra, bench, '-o', exe_b], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: hook_path_bench did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe_b], capture_output=True, text=True)
    print(r.stdout.strip())
    m = re.search(r'full: ([\d.]+) ns a call.*\nidle: ([\d.]+) ns a call', r.stdout)
    ok = bool(m) and float(m.group(2)) <= float(m.group(1)) * 1.10
    print(('ok   ' if ok else 'FAIL ') + 'hook_path_bench (F5): the idle decision is not slower than the full one')
    bad += 0 if ok else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
