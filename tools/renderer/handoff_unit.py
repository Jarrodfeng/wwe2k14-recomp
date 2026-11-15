#!/usr/bin/env python3
"""S102 lane T8, offline, 0 game runs, no window: the in-memory frame hand-over (src\\renderer_adapter\\frame_handoff.h) against the byte path.
Builds src\\renderer_adapter\\handoff_test.cpp and runs it over every frame of Bryan's entrance and the Brock match case zips: what the binder reads from a LoadedFrame
(header, every table, strings, thin / D3 stamps, every constant bank) must be equal from the file's bytes (today), from Serialize -> ParseFrame, and from BindInto
(the hand-over). Then the five planted twins (one field not handed over each: origins, header flags, bank patches, strings, thin facts), each of which must be caught (exit 1).
Prints the time both paths cost on Bryan (direction only).
  py -I tools\\renderer\\handoff_unit.py [--root DIR]
Exit 0 = both captures pass and all five plants are caught."""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import handoff_lint

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
CAPS = [
    'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip',
]


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    out = os.path.join(root, 'out', 't8')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'handoff_test.exe')
    ra = os.path.join(root, 'src', 'renderer_adapter')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I', ra, '-I', os.path.join(root, 'src'),
                        os.path.join(ra, 'handoff_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL  did not compile\n' + b.stdout + b.stderr)
        return 1
    fails = 0
    for cap in CAPS:
        z = os.path.join(main_co, cap)
        if not os.path.isfile(z):
            print('FAIL  input missing: ' + z)
            return 1
        p = subprocess.run([exe, z], capture_output=True, text=True, timeout=1200)
        lines = p.stdout.strip().split('\n')
        res = [l for l in lines if l.startswith(('PASS', 'FAIL'))]
        print(('PASS  ' if p.returncode == 0 else 'FAIL  ') + os.path.basename(os.path.dirname(z)) + ': ' + (res[-1] if res else p.stdout.strip()))
        for l in lines:
            if l.startswith(('TIME', 'synthetic')):
                print('      ' + l)
        fails += p.returncode != 0
    z = os.path.join(main_co, CAPS[0])
    for plant, what in ((1, 'draw origins not handed over'), (2, 'header flags not handed over'), (3, 'bank patches not handed over'), (4, 'strings not handed over'), (5, 'thin facts not handed over'), (6, 'two sections written in the wrong order')):
        p = subprocess.run([exe, z, 'plant', str(plant)], capture_output=True, text=True, timeout=1200)
        caught = p.returncode == 1
        print(('PASS  ' if caught else 'FAIL  ') + 'planted twin %d (%s): %s' % (plant, what, 'caught' if caught else 'NOT caught (exit %d)' % p.returncode))
        fails += not caught
    # F2: the section lists / FrameTables / ParseFrame lint, clean, and two planted twins (a table dropped from the list; the reader's swap list missing a member)
    rd = lambda p: open(os.path.join(root, p), encoding='utf-8').read()
    ho, cr, rr = rd('src/renderer_adapter/frame_handoff.h'), rd('src/renderer_adapter/capture_reader.h'), rd('src/renderer_adapter/reader.h')
    clean = handoff_lint.lint(ho, cr, rr)
    print(('PASS  ' if not clean else 'FAIL  ') + 'lint: hand-over sections == ParseFrame case ids, every table is a FrameTables member' + ('' if not clean else ': ' + '; '.join(clean)))
    fails += bool(clean)
    plant_a = handoff_lint.lint(ho.replace('  X(kSecTextRuns, TextRun, text_runs, text_runs)', ''), cr, rr)
    print(('PASS  ' if plant_a else 'FAIL  ') + 'planted twin (text_runs dropped from the table): ' + ('caught: ' + plant_a[0] if plant_a else 'NOT caught'))
    fails += not plant_a
    plant_b = handoff_lint.lint(ho, cr, rr.replace('std::vector<cap::TextRun> text_runs;', 'int text_runs_gone;'))
    print(('PASS  ' if plant_b else 'FAIL  ') + 'planted twin (FrameTables loses text_runs): ' + ('caught: ' + plant_b[0] if plant_b else 'NOT caught'))
    fails += not plant_b
    # F1c: ab_verdict refuses a handoff_memory verdict while the in-memory count is 0 (the case ring forced bytes) or missing; other switches are untouched
    sys.path.insert(0, os.path.join(root, 'tools', 'bench'))
    import tempfile, ab_verdict
    def parsed(line):
        with tempfile.NamedTemporaryFile('w', suffix='.log', delete=False, encoding='utf-8') as t:
            t.write(line + '\n')
        try:
            return ab_verdict.parse_log(t.name)
        finally:
            os.unlink(t.name)
    def refused(name, line):
        try:
            ab_verdict.check_handoff(name, parsed(line))
            return False
        except ab_verdict.Fail:
            return True
    N = 'wwe2k14_rdr_handoff_memory'
    ok = (refused(N, '[rdr][live] x handoff_memory in_memory=0 ring_forced_bytes=600') and refused(N, '[rdr][live] no count here')
          and not refused(N, '[rdr][live] x handoff_memory in_memory=480 ring_forced_bytes=0') and not refused('wwe2k14_rdr_lazy_banks', '[rdr][live] handoff_memory in_memory=0'))
    print(('PASS  ' if ok else 'FAIL  ') + 'ab_verdict: handoff_memory with 0 (or no) frames in memory is refused, 480 is accepted, other switches unaffected')
    fails += not ok
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
