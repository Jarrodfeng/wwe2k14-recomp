#!/usr/bin/env python3
"""S105 SKB3: refuse a POSITIONAL init of rf::ModelCache::Missed (record_frame.h).
Why: SKB's fix round put `bool bulk` before `bool gates`; two positional inits kept their order and silently set the wrong field,
so every miss was "gating" and the loader was FIFO again. Designated initializers (`.stored = ..., .gates = ...`) cannot slip.
Use:  py -I tools/scene/missed_init_lint.py [--root DIR]      lint src/renderer_scene (exit 1 on a hit)
      py -I tools/scene/missed_init_lint.py --selftest        plants a positional init and requires the lint to flag it (and a designated one to pass)
      py -I tools/scene/missed_init_lint.py --compile-twin    S105 SKB3 fix L3: the STRUCTURAL guard. Takes the real `struct NoPositional` / `struct Missed` lines out of record_frame.h and
                                                              compiles 9 positional forms (each MUST fail to compile) and 6 designated / empty forms (each MUST compile) with
                                                              clang++ -fsyntax-only. Needs clang++ on PATH (tools/env.ps1); missing = exit 2.
"""
import re, sys, pathlib, shutil, subprocess, tempfile, os

# `missed_.push_back({X` / `missed_.emplace_back({X` / `Missed{X` / `Missed m = {X`: X must be `.` (designated) or `}` (empty).
PAT = re.compile(r'(?:missed_\s*\.\s*(?:push_back|emplace_back)\s*\(\s*\{|(?<!struct )\bMissed\s*\{|(?<!struct )\bMissed\s+\w+\s*=?\s*\{)\s*([^\s}])')
# `missed_.emplace_back(a, b, ..)`: the C++20 parenthesised aggregate init (a designated init cannot be written there at all)
EMPLACE = re.compile(r'missed_\s*\.\s*emplace_back\s*\(\s*[^\s)]')

def scan_text(text):
    hits = []
    for m in PAT.finditer(text):
        if m.group(1) != '.':
            hits.append(text.count('\n', 0, m.start()) + 1)
    for m in EMPLACE.finditer(text):
        hits.append(text.count('\n', 0, m.start()) + 1)
    return hits

def scan_tree(root):
    bad = []
    for p in sorted(pathlib.Path(root, 'src', 'renderer_scene').rglob('*')):
        if p.suffix in ('.cpp', '.h'):
            for ln in scan_text(p.read_text(encoding='utf-8', errors='replace')):
                bad.append(f'{p.as_posix()}:{ln}')
    return bad

def selftest():
    twin = 'missed_.push_back({std::string(pac), std::string(entry), variant, key, true, count_miss});'
    twin2 = 'Missed m{pac, entry, 0, key, true, false};'
    twin3 = 'missed_.emplace_back(a, b, 0, key, true, x);'
    ok = 'missed_.push_back({.pac = a, .entry = b, .stored = true, .gates = c});\nrf::ModelCache::Missed{}; Missed pw;'
    fails = []
    if not scan_text(twin): fails.append('positional push_back not flagged')
    if not scan_text(twin2): fails.append('positional Missed{...} not flagged')
    if not scan_text(twin3): fails.append('emplace_back not flagged')
    if scan_text(ok): fails.append('designated / empty init flagged')
    for f in fails: print('SELFTEST FAIL:', f)
    if not fails: print('selftest ok: positional flagged (3 twins), designated + empty pass')
    return 1 if fails else 0

TWIN_HEAD = '#include <cstdint>\n#include <string>\n#include <vector>\nstruct Mk {\n%s\n%s\n};\nstruct Job { Mk::Missed m; int arena = -1; };\n'
S = 'std::string("a"), std::string("b"), 0u, std::string("k"), true, false'
# forms that MUST NOT compile (positional / parenthesised: the field-insert bug) and forms that MUST (designated, empty)
BAD_FORMS = [
    'Mk::Missed m{%s};' % S,
    'Mk::Missed m = {%s};' % S,
    'Mk::Missed m(%s);' % S,
    'std::vector<Mk::Missed> v; v.emplace_back(%s);' % S,
    'std::vector<Mk::Missed> v; v.push_back({%s});' % S,
    'std::vector<Mk::Missed> v{{%s}};' % S,
    'Job j{Mk::Missed{%s}, -1};' % S,
    'Mk::Missed r() { return {%s}; }' % S,
    'Mk::Missed m{"a", "b"};',
]
OK_FORMS = [
    'Mk::Missed m{.pac = "a", .entry = "b", .stored = true, .gates = false};',
    'std::vector<Mk::Missed> v; v.push_back({.pac = "a", .bulk = true});',
    'Job j{Mk::Missed{}, 3};',
    'Mk::Missed pw;',
    'Mk::Missed r() { return {.pac = "a", .key = "k"}; }',
    'Mk::Missed m{}; Mk::Missed c = m; std::vector<Mk::Missed> v{m, c};',
]

def compile_twin():
    hdr = pathlib.Path(__file__).resolve().parents[2] / 'src' / 'renderer_scene' / 'record_frame.h'
    text = hdr.read_text(encoding='utf-8', errors='replace')
    tag = re.search(r'struct NoPositional \{[^\n]*\};', text)
    st = re.search(r'struct Missed \{[^\n]*\};', text)
    if not tag or not st:
        print('COMPILE-TWIN FAIL: record_frame.h has no `struct NoPositional` / `struct Missed` line'); return 1
    if not re.match(r'struct Missed \{\s*NoPositional\s+\w+\s*\{\}\s*;', st.group(0)):
        print('COMPILE-TWIN FAIL: NoPositional is not the first field of Missed'); return 1
    clang = shutil.which('clang++')
    if not clang:
        print('COMPILE-TWIN BLOCKED: clang++ not on PATH (run through tools/env.ps1)'); return 2
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        for forms, must_compile in ((BAD_FORMS, False), (OK_FORMS, True)):
            for f in forms:
                body = (f + '\nint main(){}\n') if f.startswith('Mk::Missed r()') else ('int main() {\n' + f + '\n}\n')
                p = os.path.join(td, 'f.cpp')
                open(p, 'w').write(TWIN_HEAD % (tag.group(0), st.group(0)) + body)
                rc = subprocess.run([clang, '-std=c++20', '-fsyntax-only', '-w', p], capture_output=True, text=True).returncode
                good = (rc == 0) == must_compile
                print('PASS' if good else 'FAIL', 'compiles ' if must_compile else 'REFUSED  ', f[:90])
                fails += 0 if good else 1
    print('compile-twin:', 'FAIL' if fails else 'ok')
    return 1 if fails else 0

if __name__ == '__main__':
    if '--compile-twin' in sys.argv: sys.exit(compile_twin())
    if '--selftest' in sys.argv: sys.exit(selftest())
    root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else '.'
    bad = scan_tree(root)
    for b in bad: print('POSITIONAL Missed init (use designated initializers):', b)
    print('missed_init_lint:', 'FAIL' if bad else 'ok')
    sys.exit(1 if bad else 0)
