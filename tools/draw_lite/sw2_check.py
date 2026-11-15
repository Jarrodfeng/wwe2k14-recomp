r"""S102 lane SW2 gate checks (0 game runs).

  py -I tools\draw_lite\sw2_check.py --root . --part wiring|test

wiring: reader.h / particle_lite.h / rdr_switches.h hold the shape the model transcribes (each requirement removed from a copy must be reported by name; a text plant that does not apply fails
        "DID NOT APPLY").
test:   the SHIPPED gate (general mode) driven over Bryan's and Brock's whole op sequences (every draw a candidate; traces by tools\particle_lite\lite_trace.py): the lite stream equals the full stream on every
        field of every draw; mutated traces pass; planted twins 1-11 each FAIL (11 = the origin record copied when the origin key differs); a lever that takes 0 draws fails."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
STREAM = 'FAIL lite stream == full stream'
PLANTS = [(1, 'state', STREAM), (2, 'shader', STREAM), (3, 'gap', STREAM), (4, None, STREAM), (5, 'tex', STREAM), (6, 'tex_unmarked', STREAM), (7, None, 'FAIL thin query count'),
          (8, 'z', STREAM), (9, 'header', STREAM), (10, None, 'FAIL C7 hole'), (11, None, STREAM), (12, None, STREAM, 'bryan'), (13, 'cpu_write', 'FAIL EnsureContent calls')]
MUTATIONS = ('tex', 'shader', 'gap', 'state', 'tex_unmarked', 'header', 'z', 'cpu_write')
READER = os.path.join('src', 'renderer_adapter', 'reader.h')
LITE = os.path.join('src', 'renderer_adapter', 'particle_lite.h')
SW = os.path.join('src', 'renderer_adapter', 'rdr_switches.h')
REQ = [
    ('general candidate: every draw when the switch bit is on', READER, r'const bool pl_general = !pl_particle_try && pl::g_on_gen\.load'),
    ('general gate is its own Gate', READER, r'pl::Gate\* pl_gate = pl_general \? &pl::g_gate_gen : &pl::g_gate;'),
    ('general key leaves the origin out; an unresolved origin is flagged', READER, r'else pl_key\.origin_ok = false;'),
    ('origin copy only when origin_same', READER, r'pl_origin_copy = pl_taken && \(!pl_general \|\| \(pl_gate->origin_same && '),
    ('origin record copied only under pl_origin_copy', READER, r'if \(pl_lite && pl_origin_copy\) \{'),
    ('thin skip only under pl_origin_copy', READER, r'pl_lite && pl_origin_copy && pl_gate->SkipThin\(\)'),
    ('F2: a taken general draw calls EnsureContent for its copied slots', READER, r'EnsureContentChecked\(pl_gate->snap\.s\[k\]\.image, pl_gate->snap\.s\[k\]\.w\)'),
    ('F1: no origin copy for a glyph / widget / run draw', READER, r'pl_key\.run == 0 && pl_key\.widget == 0 && pl_key\.text == 0'),
    ('F1: the live verify compares the TextRun count', READER, r't\.text_runs\.size\(\) == pl_runs_before'),
    ('F3: op in the key', READER, r'pl_key\.op = pk\.op'),
    ('F3: op in OriginEqual', LITE, r'a\.op == b\.op && a\.site == b\.site'),
    ('state gather handed to the full path', READER, r'StateWords& words = pl_have_words \? pl_words : words_local;'),
    ('taken draw keeps the snapshot', READER, r'pl_gate->RememberTaken\('),
    ('switch read every frame at the present', READER, r'rdr_sw::IntSeen\(rdr_sw::kDrawLite, 0, guest_frame\)'),
    ('cvar defined', READER, r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_draw_lite, 0,'),
    ('switch PerFrame default 0', SW, r'X\(DrawLite, "wwe2k14_rdr_draw_lite", PerFrame, 0\)'),
    ('gate: state key compares shaders, decl, prim, strides', LITE, r'a\.vs_object == b\.vs_object && a\.ps_object == b\.ps_object && a\.decl == b\.decl && a\.prim == b\.prim && a\.vs == b\.vs && a\.ps == b\.ps'),
    ('gate: origin equality needs both resolved', LITE, r'a\.origin_ok && b\.origin_ok'),
]


def fail(m):
    print('FAIL: ' + m)
    return 1


def run(cmd, timeout=1500):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def main_checkout(root):
    o = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    return os.path.dirname(os.path.abspath(os.path.join(root, o.stdout.strip()))) if o.returncode == 0 else None


def read(root, rel):
    p = os.path.join(root, rel)
    if not os.path.isfile(p):
        raise SystemExit('FAIL: %s missing' % p)
    return open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')


def part_wiring(root):
    bad = 0
    texts = {}
    for name, f, rx in REQ:
        t = texts.setdefault(f, read(root, f))
        ok = re.search(rx, t) is not None
        print('%s requirement: %s' % ('PASS' if ok else 'FAIL', name))
        bad += not ok
        if ok:  # the plant: the requirement removed from a copy must be reported
            m = re.search(rx, t)
            planted = t[:m.start()] + '/*PLANTED*/' + t[m.end():]
            if planted == t:
                print('FAIL plant for "%s": DID NOT APPLY' % name)
                bad += 1
            elif re.search(rx, planted):
                print('FAIL plant for "%s": NOT caught' % name)
                bad += 1
    return 1 if bad else 0


def build(root, src, exe, extra=()):
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    if os.path.exists(exe):
        os.remove(exe)
    b = run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-o', exe, os.path.join(root, src)] + list(extra))
    return b.returncode == 0 and os.path.isfile(exe), b.stdout + b.stderr


def part_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    main = main_checkout(root)
    caps = [('brock', os.path.join(main, 'out', 'lane86', 'brock', 'capture')),
            ('bryan', os.path.join(main, 'out', 's101', 'runs', '20261112_122700_s100_owner_bryan_entrance', 'case_20261112_122719_969_0_pc_run_exit.zip'))]
    for n, p in caps:
        if not os.path.exists(p):
            return fail('capture %s missing: %s' % (n, p))
    od = os.path.join(root, 'out', 'sw2')
    exe = os.path.join(od, 'plt_gate.exe')
    ok, log = build(root, os.path.join('tools', 'particle_lite', 'particle_lite_test.cpp'), exe)
    if not ok:
        return fail('particle_lite_test did not compile\n' + log)
    traces = {}
    for n, p in caps:
        tr = os.path.join(od, n + '.txt')
        r = run([sys.executable, '-I', os.path.join(root, 'tools', 'particle_lite', 'lite_trace.py'), p, tr])
        print(r.stdout.strip())
        if r.returncode or not os.path.isfile(tr):
            return fail('lite_trace failed for %s\n%s' % (n, r.stdout + r.stderr))
        traces[n] = tr
    bad = 0
    for n in ('bryan', 'brock'):
        for live in (False, True):
            r = run([exe, traces[n], '--general', '--bundle', 'default'] + (['--marks', 'live'] if live else []))
            t = r.stdout
            clean = r.returncode == 0 and 'all checks passed' in t and not re.search(r'^FAIL', t, re.M)
            m = re.search(r'taken (\d+) in (\d+) runs', t)
            taken = int(m.group(1)) if m else 0
            print('%s %s general gate%s: lite stream == full stream; taken %d (exit %d)' % ('PASS' if clean and taken else 'FAIL', n, ' (live marks)' if live else '', taken, r.returncode))
            bad += not (clean and taken)
    for mut in MUTATIONS:
        r = run([exe, traces['brock'], '--general', '--bundle', 'default', '--mutate', mut])
        ok = r.returncode == 0 and 'all checks passed' in r.stdout
        print('%s the real gate passes the %s-mutated trace (exit %d)' % ('PASS' if ok else 'FAIL', mut, r.returncode))
        bad += not ok
    for pl in PLANTS:
        n, mut, needle = pl[:3]
        cap = pl[3] if len(pl) > 3 else 'brock'  # plant 12 (glyph origin copied) needs Bryan: Brock has no text draws
        r = run([exe, traces[cap], '--general', '--bundle', 'default', '--plant', str(n)] + (['--mutate', mut] if mut else []))
        caught = r.returncode == 1 and needle in r.stdout
        print('%s planted twin %d%s: %s' % ('PASS' if caught else 'FAIL', n, ' (+ mutate %s)' % mut if mut else '', 'caught' if caught else 'NOT caught (exit %d)' % r.returncode))
        bad += not caught
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--part', required=True, choices=['wiring', 'test'])
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return {'wiring': part_wiring, 'test': part_test}[a.part](root)


if __name__ == '__main__':
    sys.exit(main())
