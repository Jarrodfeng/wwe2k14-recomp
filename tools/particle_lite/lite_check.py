#!/usr/bin/env python3
"""S102 lane T18 gate check (offline, 0 game runs, 0 Surface runs): wwe2k14_rdr_particle_lite (src\\renderer_adapter\\particle_lite.h + the lines marked "S102 T18" in reader.h).

  py -I tools\\particle_lite\\lite_check.py [--root DIR] [--part test|wiring|all]

test:   exports two real captures' op sequences (Brock's entrance = the pyro burst, Bryan's entrance; tools\\particle_lite\\lite_trace.py, from the MAIN checkout's out\\ in place), builds
        tools\\particle_lite\\particle_lite_test.cpp against the SHIPPED particle_lite.h (-DT18_PLANTS), and requires per trace: exit 0, "all checks passed", the lite stream equal to the full
        stream on every field of every draw, taken > 0, the burst frames' take count printed; the three mutated traces pass with the real gate; and the planted twins each FAIL the
        stream-equality line (1 texture change does not break the run, 2 a different shader merged, 3 an op between draws does not end the run, 4 a bank delta dropped, 5 slot records not
        copied; a plant that no longer applies is its own failure).
wiring: reader.h / rdr_switches.h / particle_lite.h still have the shape the test's model transcribes (each pattern below), the gate sits after EmitTargets (which can push an op), the per-draw
        work that must stay (PushBanks, BuildGeometry, the bool / loop compare) is not behind the lite test, the constants equal particle_runs.h's; and each requirement removed from a copy of the
        text must be reported by name (a plant that no longer applies is its own failure: DID NOT APPLY).
A check that cannot read its input fails loudly (missing capture, missing clang, no result line)."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
HERE = os.path.dirname(os.path.abspath(__file__))
BURST = '210:248'  # capture frames of Brock's pyro burst (R15: capture frames 210..248 of out\lane86\brock\capture)
STREAM = 'FAIL lite stream == full stream'
# (plant, mutation the plant mishandles, extra args, the check line that must name it)
PLANTS = [(1, 'state', [], STREAM), (2, 'shader', [], STREAM), (3, 'gap', [], STREAM), (4, None, [], STREAM), (5, 'tex', [], STREAM),
          (6, 'tex_unmarked', [], STREAM),            # F1: the slot snapshot ignored
          (7, None, ['--bundle', 'default'], 'FAIL thin query count'),  # F6: the thin query skipped after a draw that DID query
          (8, 'z', [], STREAM),                        # F2: draw entries ignored
          (9, 'header', [], STREAM),                   # F3: the snapshot ignores the header part
          (10, None, [], 'FAIL C7 hole'),
          (1, 'state', ['--marks', 'live'], STREAM)]   # live marks: ignoring the state compare under set marks              # the C7 by_marks entry counter ignored
MUTATIONS = ('tex', 'shader', 'gap', 'state', 'tex_unmarked', 'header', 'z')


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def run(cmd, timeout=900):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def part_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    main = main_checkout(root)
    if not main:
        return fail('no git common dir: the captures live in the main checkout')
    caps = [('brock', os.path.join(main, 'out', 'lane86', 'brock', 'capture')),
            ('bryan', os.path.join(main, 'out', 's101', 'runs', '20261112_122700_s100_owner_bryan_entrance', 'case_20261112_122719_969_0_pc_run_exit.zip'))]
    for name, path in caps:
        if not os.path.exists(path):
            return fail('capture %s missing: %s' % (name, path))
    out_dir = os.path.join(root, 'out', 't18')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'particle_lite_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-o', exe, os.path.join(root, 'tools', 'particle_lite', 'particle_lite_test.cpp')])
    if b.returncode or not os.path.isfile(exe):
        return fail('particle_lite_test did not compile\n' + b.stdout + b.stderr)
    bad = 0
    traces = {}
    for name, path in caps:
        trace = os.path.join(out_dir, name + '.txt')
        r = run([sys.executable, '-I', os.path.join(root, 'tools', 'particle_lite', 'lite_trace.py'), path, trace])
        print(r.stdout.strip())
        if r.returncode or not os.path.isfile(trace):
            return fail('lite_trace failed for %s\n%s%s' % (name, r.stdout, r.stderr))
        traces[name] = trace
    for name in ('brock', 'bryan'):
        for bundle, live in ((False, False), (True, False), (True, True)):  # arm 2: the default bundle (the thin-lite facts table on); arm 3: + live marks (every candidate carries them, PC run 7245)
            args = [exe, traces[name]] + (['--burst', BURST] if name == 'brock' else []) + (['--bundle', 'default'] if bundle else []) + (['--marks', 'live', '--bench'] if live else [])
            r = run(args)
            text = r.stdout
            print('--- %s (real gate, clean%s%s) ---' % (name, ', default bundle' if bundle else '', ', live marks' if live else ''))
            print(text.rstrip() if not bundle else '\n'.join(l for l in text.splitlines() if not l.startswith('PASS unit')))
            if r.returncode != 0 or 'all checks passed' not in text or re.search(r'^FAIL', text, re.M):
                print('FAIL: %s: the clean run did not pass (exit %d)' % (name, r.returncode))
                bad += 1
            m = re.search(r'taken (\d+) in (\d+) runs', text)
            if not m or int(m.group(1)) == 0:
                print('FAIL: %s: the lever took no draws on the real capture (a lever that takes 0 does nothing)' % name)
                bad += 1
            if live:
                m2 = re.search(r'taken (\d+) of (\d+), marked but equal (\d+)', text)
                if not m2 or int(m2.group(3)) == 0 or int(m2.group(3)) != int(m2.group(1)):
                    print('FAIL: %s: the live-mark arm took %s draws but counted marked-but-equal %s (every taken draw carries marks)' % (name, m2 and m2.group(1), m2 and m2.group(3)))
                    bad += 1
            if bundle and 'PASS thin query count' not in text:
                print('FAIL: %s: the bundle arm did not check the thin query count' % name)
                bad += 1
    for mut in MUTATIONS:
        for extra in ([], ['--marks', 'live']):
            r = run([exe, traces['brock'], '--mutate', mut, '--bundle', 'default'] + extra)
            ok = r.returncode == 0 and 'all checks passed' in r.stdout
            print('%s the real gate passes the %s-mutated trace%s (exit %d)' % ('PASS' if ok else 'FAIL', mut, ' (live marks)' if extra else '', r.returncode))
            bad += not ok
    for n, mut, extra, needle in PLANTS:
        args = [exe, traces['brock'], '--plant', str(n)] + (['--mutate', mut] if mut else []) + extra
        r = run(args)
        caught = r.returncode == 1 and needle in r.stdout
        print('%s planted twin %d%s: %s' % ('PASS' if caught else 'FAIL', n, ' (+ mutate %s)' % mut if mut else '', 'caught (exit 1, "%s" named it)' % needle if caught else 'NOT caught (exit %d)' % r.returncode))
        bad += not caught
    return 1 if bad else 0


# --- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
READER = os.path.join('src', 'renderer_adapter', 'reader.h')
SWITCHES = os.path.join('src', 'renderer_adapter', 'rdr_switches.h')
LITE = os.path.join('src', 'renderer_adapter', 'particle_lite.h')
RUNS = os.path.join('src', 'renderer_adapter', 'particle_runs.h')

REQ = [  # (name, file, regex that must match)
    ('candidate filter: hard yParticle pair', READER, r'vs->original_hash == pl::kVs && ps->hash == pl::kPs'),
    ('candidate filter: inline draws only', READER, r'pl::g_on\.load\(std::memory_order_relaxed\) && a\.inline_vertices && ps'),
    ('candidate filter: the recorder call sites', READER, r'pk\.call_site == pl::kSpriteSite \|\| pk\.call_site == pl::kQuadSite'),
    ('candidate filter: the origin op is the replayed op', READER, r'pk\.op == rdr_adapter::origin::t_replay_op'),
    ('key: origin site / pass / object / part / widget / text / run', READER, r'pl_key\.site = pk\.call_site.*pl_key\.pass_site = pk\.pass_site.*pl_key\.object = pk\.object.*pl_key\.part = pk\.part.*pl_key\.widget = pk\.widget.*pl_key\.text = pk\.text.*pl_key\.run = pk\.run'),
    ('key: shader objects, declaration, primitive', READER, r'pl_key\.vs_object = vs_object.*pl_key\.ps_object = ps_object.*pl_key\.decl = db::Ld32\(base, dev \+ kDevDecl\).*pl_key\.prim = uint32_t\(a\.prim\)'),
    ('key: the shader infos', READER, r'pl_key\.vs = vs, pl_key\.ps = ps;'),
    ('key: the strides', READER, r'std::memcpy\(pl_key\.strides, strides, sizeof\(strides\)\)'),
    ('excluded: not recording', READER, r'const bool excluded = !Recording\(\)'),
    ('excluded: wide', READER, r'const bool excluded = [^;]*g_wide\.on\(\)'),
    ('excluded: jitter', READER, r'const bool excluded = [^;]*g_jitter\.on'),
    ('excluded: trace', READER, r'const bool excluded = [^;]*TraceOn\(\)'),
    ('verify: a taken draw runs the full path under marks verify', READER, r'pl_lite = pl_taken && !REXCVAR_GET\(wwe2k14_rdr_debug_marks_verify\);'),
    ('verify: the lite copies are compared and a miss counted', READER, r'if \(!same\) \{\s*const uint64_t n = \+\+pl_gate->census->misses;'),
    ('decide: marks, generation, draw count, op count, draw entries, slot snapshot', READER, r'pl_gate->Decide\(\s*pl_key, a\.unmarked, t\.generation, t\.draws\.size\(\), t\.ops\.size\(\), excluded, pl::Entries\(\),.*?\[&\] \{ return PlSlotsSame\(base, dev, pl_gate->snap\); \}\)'),
    ('marks set: the state words are gathered and compared with the state cache', READER, r'GatherStateWords\(base, dev, a\.prim, ps_object != 0, ps, pl_words\);\s*pl_have_words = true;[^\n]*\n\s*return std::memcmp\(&pl_words, &sc\.words, sizeof\(pl_words\)\) == 0;'),
    ('marks set: the state cache must be this run (generation, entry counter)', READER, r'sc\.generation == t\.generation && pl::StateEntryOk\(sc\.entry, pl::Entries\(\)\) && sc\.words\.prim'),
    ('diagnosis: the marks of a marked candidate are logged behind the cvar', READER, r'if \(!a\.unmarked\) PlLogMarks\(pl_key\);'),
    ('diagnosis: the cvar exists', READER, r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_debug_particle_marks, 0,'),
    ('F2: every draw entry is counted at ReadMarks', READER, r'rdr_adapter::particle_lite::NoteEntry\(\);[^\n]*\n\s*const bool unmarked = !g_prev_draw_bound'),
    ('C7 hole: by_marks needs the entry counter', READER, r'a\.unmarked && pl::StateEntryOk\(sc\.entry, pl::Entries\(\)\)'),
    ('C7 hole: the state cache stores its draw entry', READER, r'g_state_cache\.entry = pl::Entries\(\);'),
    ('F1: the snapshot reads the GPU fetch copy (g_gpu_fetch words and object)', READER, r'std::memcpy\(o\.w, g_gpu_fetch\[slot\]\.words, sizeof\(o\.w\)\);\s*object = g_gpu_fetch\[slot\]\.object;'),
    ('F1/F3: the snapshot reads the texture header bytes', READER, r'std::memcpy\(o\.header, db::Host\(base, object \+ 28\), sizeof\(o\.header\)\)'),
    ('F1/F3: the snapshot reads the image flags', READER, r'o\.flags = uint8_t\(\(img\.packed \? 1 : 0\) \| \(img\.copy_dest \? 2 : 0\)'),
    ('F1/F3: the snapshot holds the registry epoch', READER, r'out\.epoch = g_reg\.epoch;'),
    ('F1: the live slots are compared with the snapshot', READER, r'return now\.Equal\(prev\);'),
    ('F1: the snapshot is stored at Remember from the draw\'s own slot list', READER, r'PlSnapSlots\(base, dev, t\.texture_slots\.data\(\) \+ d\.texture_first, d\.texture_count, pl_snap\);'),
    ('F7: nothing zero-filled on the off path (an optional key, five previous fields)', READER, r'std::optional<pl::Key> pl_key_opt;.*struct PlPrev \{\s*uint32_t fixed = 0, dynamic = 0, texture_first = 0, texture_count = 0, layout = 0;'),
    ('copy: fixed and dynamic from the previous draw', READER, r'd\.fixed = pl_prev\.fixed;\s*d\.dynamic = pl_prev\.dynamic;'),
    ('copy: texture slots re-appended', READER, r'lite_slots\[k\] = t\.texture_slots\[pl_prev\.texture_first \+ k\];\s*for \(uint32_t k = 0; k < n; \+\+k\) t\.texture_slots\.push_back\(lite_slots\[k\]\)'),
    ('copy: the layout', READER, r'd\.layout = pl_prev\.layout;'),
    ('copy: the origin record', READER, r'const cap::DrawOrigin lite_origin = t\.origins\[pl_gate->draw_index\];'),
    ('thin: skipped only after a no-query answer', READER, r'const bool pl_skip_thin = pl_lite && pl_origin_copy && pl_gate->SkipThin\(\);'),
    ('thin: ThinLiteDraw reports its early returns', READER, r'bool\* noquery = nullptr'),
    ('thin: Query clears the flag', READER, r'if \(noquery\) \*noquery = false;[^\n]*\n\s*return t\.lite\.Query'),
    ('remember: after the origin is pushed', READER, r'\n      pl_gate->Remember\(\*pl_key_opt, t\.generation, t\.draws\.size\(\) - 1, t\.ops\.size\(\), pl_thin_noquery, pl::Entries\(\), pl_snap\);'),
    ('stays per draw: the bank delta', READER, r'd\.vs_bank = PushBanks\(base, dev, vs, ps_object, d\.ps_bank\);'),
    ('stays per draw: the geometry build', READER, r'if \(!thin_lite && !\(REXCVAR_GET\(wwe2k14_rdr_debug_live_skip\) & 1\)\) \{\s*ScopedNs timer\(g_times\.geometry\);\s*built = BuildGeometry\('),
    ('switch read every frame with the guest frame', READER, r'particle_lite::g_on\.store\(rdr_sw::BoolSeen\(rdr_sw::kParticleLite, false, guest_frame\)'),
    ('census line logged', READER, r'particle_lite::LogCensus\(guest_frame'),
    ('cvar default off', READER, r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_particle_lite, false,'),
    ('switch table entry, PerFrame, default off', SWITCHES, r'X\(ParticleLite, "wwe2k14_rdr_particle_lite", PerFrame, 0\)'),
]


def read(root, rel):
    p = os.path.join(root, rel)
    if not os.path.isfile(p):
        raise IOError('missing ' + p)
    return open(p, encoding='utf-8', errors='replace', newline='').read().replace('\r\n', '\n')


def wiring_problems(texts):
    out = []
    for name, rel, rx in REQ:
        if not re.search(rx, texts[rel], re.S):
            out.append(name)
    r = texts[READER]
    emit, decide = r.find('  EmitTargets(base, dev);'), r.find('pl_gate->Decide(')
    if emit < 0 or decide < 0 or emit > decide:
        out.append('order: EmitTargets (it can push an op) runs before the gate decides')
    # the bank delta, the geometry build and the bool / loop compare must not sit behind the lite test
    for name, rx in (('PushBanks', r'\n[^\n]*\n[^\n]*\n[^\n]*d\.vs_bank = PushBanks'), ('BuildGeometry', r'\n[^\n]*\n[^\n]*\n[^\n]*built = BuildGeometry\('), ('bool / loop', r'\n[^\n]*\n[^\n]*std::memcmp\(raw, sc\.bool_raw')):
        m = re.search(rx, r)
        if not m or 'pl_lite' in m.group(0):
            out.append('%s must not be behind pl_lite' % name)
    if 'cap::Draw{}' in r[r.find('S102 T18 (wwe2k14_rdr_particle_lite)'):r.find('cap::DrawFixed fixed;')]:
        out.append('F7: a zero-filled cap::Draw on the off path')
    runs, lite = texts[RUNS], texts[LITE]
    for const, rx in (('kVs', r'kParticleVs = (0x[0-9A-Fa-f]+)ull'), ('kPs', r'kParticlePs = (0x[0-9A-Fa-f]+)ull'), ('kSpriteSite', r'kSpriteSite = (0x[0-9A-Fa-f]+)u'), ('kQuadSite', r'kQuadSite = (0x[0-9A-Fa-f]+)u')):
        a = re.search(rx, runs)
        b = re.search(r'%s = (0x[0-9A-Fa-f]+)u' % const if const.endswith('Site') else r'%s = (0x[0-9A-Fa-f]+)ull' % const, lite)
        if not a or not b or int(a.group(1), 16) != int(b.group(1), 16):
            out.append('constant %s differs from particle_runs.h' % const)
    return out


def part_wiring(root):
    try:
        texts = {rel: read(root, rel) for rel in (READER, SWITCHES, LITE, RUNS)}
    except IOError as e:
        return fail(str(e))
    bad = wiring_problems(texts)
    for b in bad:
        print('FAIL: wiring: ' + b)
    if not bad:
        print('PASS: wiring: %d requirements and the order / stay-per-draw / constants checks hold' % len(REQ))
    # planted twins: each requirement removed from a copy must be reported by name
    missed = 0
    for name, rel, rx in REQ:
        t = dict(texts)
        m = re.search(rx, t[rel], re.S)
        if not m:
            print('FAIL: wiring plant "%s": DID NOT APPLY (the pattern is not in %s)' % (name, rel))
            missed += 1
            continue
        t[rel] = t[rel][:m.start()] + t[rel][m.end():]
        if name not in wiring_problems(t):
            print('FAIL: wiring plant "%s": NOT caught' % name)
            missed += 1
    t = dict(texts)  # order plant: the decide call moved before EmitTargets
    t[READER] = t[READER].replace('  EmitTargets(base, dev);', '  // moved\n', 1) + '\n  EmitTargets(base, dev);\n'
    if 'order: EmitTargets (it can push an op) runs before the gate decides' not in wiring_problems(t):
        print('FAIL: wiring plant "order": NOT caught')
        missed += 1
    t = dict(texts)  # stay-per-draw plant: the bank delta behind the lite test
    t[READER] = t[READER].replace('      d.vs_bank = PushBanks(', '      if (!pl_lite) d.vs_bank = PushBanks(', 1)
    if 'PushBanks must not be behind pl_lite' not in wiring_problems(t):
        print('FAIL: wiring plant "PushBanks behind pl_lite": NOT caught (or did not apply)')
        missed += 1
    if not missed:
        print('PASS: wiring: %d planted twins (each requirement removed, the order, a bank delta behind the lite test) all reported by name' % (len(REQ) + 2))
    return 1 if (bad or missed) else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--part', default='all', choices=['test', 'wiring', 'all'])
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    rc = 0
    if a.part in ('wiring', 'all'):
        rc |= part_wiring(root)
    if a.part in ('test', 'all'):
        rc |= part_test(root)
    print('exit %d' % rc)
    return rc


if __name__ == '__main__':
    sys.exit(main())
