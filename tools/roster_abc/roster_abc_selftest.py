"""S110 DLCRANK: builds + runs the host unit of src/roster_abc_core.h (tools/roster_abc/roster_abc_unit.cpp) and its planted twins. No game runs.

    py -I tools/roster_abc/roster_abc_selftest.py   compiles roster_abc_unit.cpp with clang++ against src/roster_abc_core.h and runs it (every check must
                                                    PASS), then against COPIES of the header that each carry one planted fault (a rule removed or reversed)
                                                    and requires the unit to FAIL on each, by a printed FAIL line and its summary (no crash).

Exit 0 = the unit passes on the shipped header and fails on every planted twin. The plant patterns use \\r?\\n where they span a line (CRLF checkouts).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, 'src', 'roster_abc_core.h')
NAME = 'roster_abc_core.h'

KEY = r'seat_bind::FoldedName\(items\[p\]\.name\)'
PLANTS = [
    # which entries move
    ('all_entries_move', r'if \(items\[i\]\.downloaded\) pos\.push_back\(i\);', 'pos.push_back(i);'),
    ('refill_ignores_held_positions', r'out\[pos\[k\]\] = order\[k\];', 'out[k] = order[k];'),
    ('entry_doubled', r'out\[pos\[k\]\] = order\[k\];', 'out[pos[k]] = order[0];'),
    # the order
    ('ties_reversed', r'return std::lexicographical_compare\(key\[a\]', 'return key[a] == key[b] ? a > b : std::lexicographical_compare(key[a]'),
    ('order_descending', r'return std::lexicographical_compare\(key\[a\]\.begin\(\), key\[a\]\.end\(\), key\[b\]\.begin\(\), key\[b\]\.end\(\)\);',
     'return std::lexicographical_compare(key[b].begin(), key[b].end(), key[a].begin(), key[a].end());'),
    ('empty_name_not_sorted', r'if \(items\[i\]\.downloaded\) pos\.push_back\(i\);', 'if (items[i].downloaded && !items[i].name.empty()) pos.push_back(i);'),
    # the collation
    ('raw_bytes_not_folded', KEY, '[&] { std::vector<uint32_t> v; for (unsigned char c : items[p].name) v.push_back(c); return v; }()'),
    ('the_not_dropped', KEY,
     '[&] { std::vector<uint32_t> v; for (unsigned char c : items[p].name) v.push_back(c >= \'a\' && c <= \'z\' ? c - 32 : c); return v; }()'),
    ('symbols_after_letters', KEY,
     '[&] { auto v = seat_bind::FoldedName(items[p].name); for (auto& c : v) if (c < seat_bind::kLetterRank) c += 0x10000000u; return v; }()'),
]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(header_text, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, NAME), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'roster_abc_unit.exe')
    defs = ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING']
    # -I inc first (the planted copy wins), then src for seat_bind_core.h and what it includes
    r = subprocess.run([find_clang(), '-std=c++20', '-Wall', '-Wno-unused-command-line-argument'] + defs + ['-I', inc, '-I', os.path.join(REPO, 'src'),
                        '-I', os.path.join(REPO, 'src', 'launcher'), os.path.join(HERE, 'roster_abc_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('roster_abc_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-1200:]))
    return exe, (r.stdout + r.stderr)


def run_test(exe):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    text = open(HEADER, encoding='utf-8', newline='').read()
    problems = []
    tmp = tempfile.mkdtemp(prefix='roster_abc_unit_')
    try:
        exe, warn = build(text, tmp, 'shipped')
        if 'warning' in warn:
            problems.append('the unit compiles with warnings: ' + warn[-300:])
        rc, out = run_test(exe)
        sys.stdout.write(out)
        if rc != 0 or 'PASS roster_abc_unit' not in out:
            problems.append('the unit FAILS on the shipped header (or ended without its summary line)')
        for name, pat, rep in PLANTS:
            new, n = re.subn(pat, lambda m: rep, text, count=1)
            if n != 1:
                problems.append('plant %s: the pattern matched %d times (the header changed under the plant)' % (name, n))
                continue
            try:
                pexe, _ = build(new, tmp, name)
                prc, pout = run_test(pexe)
            except RuntimeError as e:
                problems.append('plant %s: %s' % (name, e))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL ')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            elif not failed or 'FAIL roster_abc_unit' not in pout:
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (name, prc))
            print('plant %-30s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for p in problems:
        print('PROBLEM: ' + p)
    print('roster_abc_selftest: %s (%d plants)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
