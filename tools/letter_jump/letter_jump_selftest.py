"""S110 LETTER: builds + runs the host unit of src/letter_jump_core.h (tools/letter_jump/letter_jump_unit.cpp) and its planted twins. No game runs.

    py -I tools/letter_jump/letter_jump_selftest.py   compiles letter_jump_unit.cpp with clang++ against src/letter_jump_core.h and runs it (every check
                                                      must PASS), then against COPIES of the header that each carry one planted fault (a rule removed or
                                                      reversed) and requires the unit to FAIL on each, by a printed FAIL line and its summary (no crash).

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
sys.path.append(os.path.join(REPO, 'tools', 'harness'))
import plant_runner as pr   # S110 PLANTFAST: the shared bounded pool of the planted builds
HEADER = os.path.join(REPO, 'src', 'letter_jump_core.h')
NAME = 'letter_jump_core.h'

PLANTS = [
    # Initial
    ('the_not_dropped', r'name\.size\(\) - i > 4 &&', 'false &&'),
    ('the_case_sensitive', r"detail::Upper\(name\[i \+ 1\]\) == 'H'", "name[i + 1] == 'H'"),
    ('the_needs_no_blank', r"name\[i \+ 3\] == ' '", 'true'),
    ('lower_accent_not_folded', r'if \(c >= 0xE0 && c != 0xF7\) c -= 0x20;', ';'),
    ('utf8_accent_not_read', r'if \(n > 1 && i \+ n <= s\.size\(\)\)', 'if (false)'),
    ('utf8_lead_read_as_latin1', r'if \(n > 1 && i \+ n <= s\.size\(\)\)', 'if (n == 2 && c <= 0xC3 && i + n <= s.size())'),    ('digit_not_bucketed', r"return \(u >= 'A' && u <= 'Z'\) \? u : kSymbolBucket;", 'return u;'),
    ('symbol_bucket_after_z', r"constexpr char kSymbolBucket = '#';", "constexpr char kSymbolBucket = '~';"),
    ('empty_name_has_letter', r'if \(i >= name\.size\(\)\) return 0;', 'if (i >= name.size()) return kSymbolBucket;'),
    ('sharp_s_not_s', r"if \(c == 0xDF\) return 'S';", ';'),
    # NextRow
    ('direction_swapped', r'if \(dir > 0\) \{', 'if (dir < 0) {'),
    ('next_wrap_missing', r'for \(int c = 0; c < 256 && !target; \+\+c\)  // wrap', 'for (int c = 0; false; ++c)  // wrap'),
    ('prev_wrap_missing', r'for \(int c = 255; c >= 0 && !target; --c\)  // wrap', 'for (int c = 255; false; --c)  // wrap'),
    ('next_includes_current_letter', r'for \(int c = int\(uint8_t\(cl\)\) \+ 1; c < 256', 'for (int c = int(uint8_t(cl)); c < 256'),
    ('prev_includes_current_letter', r'for \(int c = int\(uint8_t\(cl\)\) - 1; c >= 0', 'for (int c = int(uint8_t(cl)); c >= 0'),
    ('locked_letter_counts_present', r'if \(!r\.selectable \|\| r\.letter == 0\) continue;', 'if (r.letter == 0) continue;'),
    ('locked_row_is_target', r'if \(r\.selectable && r\.letter == l\) return r\.index;', 'if (r.letter == l) return r.index;'),
    ('middle_of_run_rule_missing', r'if \(f != cur\) return f;', '(void)f;'),
    ('middle_of_run_ignores_first_row', r'if \(f != cur\) return f;', 'return f;'),
    ('single_letter_moves', r'if \(distinct < 2\) return -1;', 'if (distinct < 1) return -1;'),
]

# The game side (src/letter_jump.h) has no host harness; these are its wiring CONTRACTS (review S110 LETTER, each settled from the listings):
# a pattern that must be in the file. The proof that each can fail is the unfixed file (the builder's head f62c78a93), which misses every one.
GAME = os.path.join(REPO, 'src', 'letter_jump.h')
CONTRACTS = [
    # sub_828568B8 is the game's own "current row" (+64, or +37064 when sub_82856108): what sub_8285BD18 itself reads
    ('current_row_is_the_games_own', r'cur = call\(sub_828568B8, \{panel\}\);'),
    # in the grid the game's "cursor moved" reads the list index only in state 4 (sub_82856108); in 2 / 3 it would read +64, a cell ROW
    ('grid_only_where_the_game_reads_its_index', r'if \(tab == 0 && call\(sub_82856108, \{panel\}\) == 0\) return false;'),
    # the grid walk's own rest test (sub_8282C648 +320..+33C): a cell, row < 5, and sub_8282C298(G = [screen + 56796], id)
    ('grid_rest_row_below_5', r'row < kGridRows'),
    ('grid_rest_is_the_walks_own_test', r'call\(sub_8282C298, \{walker, id\}\) != 0'),
    # a created superstar (id < 100) carries its name inline at its record +204 (sub_822C0D90); its row +6 is not a text id
    ('caw_name_is_inline', r'call\(sub_827D92F0, \{id\}\)'),
]


def check_contracts(problems):
    text = open(GAME, encoding='utf-8', newline='').read()
    for name, pat in CONTRACTS:
        ok = re.search(pat, text) is not None
        print('contract %-42s %s' % (name, 'held' if ok else 'MISSING'))
        if not ok:
            problems.append('contract %s: src/letter_jump.h has no match for %s' % (name, pat))


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
    exe = os.path.join(inc, 'letter_jump_unit.exe')
    r = subprocess.run([find_clang(), '-std=c++20', '-Wall', '-I', inc, os.path.join(HERE, 'letter_jump_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('letter_jump_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
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
    tmp = tempfile.mkdtemp(prefix='letter_jump_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order
        once = pr.Once(pr.make_spec(os.path.join(HERE, 'letter_jump_unit.cpp'), {NAME: HEADER}, ['-std=c++20', '-Wall']), pr.fail_lines('letter_jump_unit'), NAME)
        sh, outs = pr.run_plants(text, PLANTS, lambda t, tag: build(t, tmp, tag), lambda art, tag: run_test(art[0]), pr.parse_jobs(sys.argv[1:]), once)
        if sh.error:
            raise RuntimeError(sh.error)
        warn, rc, out = sh.art[1], sh.rc, sh.out
        if 'warning' in warn:
            problems.append('the unit compiles with warnings: ' + warn[-300:])
        sys.stdout.write(out)
        if rc != 0 or 'PASS letter_jump_unit' not in out:
            problems.append('the unit FAILS on the shipped header (or ended without its summary line)')
        for o in outs:
            name, prc, pout = o.name, o.rc, o.out
            if o.matches != 1:
                problems.append('plant %s: the pattern matched %d times (the header changed under the plant)' % (name, o.matches))
                continue
            if o.error:
                problems.append('plant %s: %s' % (name, o.error))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL ')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            elif not failed or 'FAIL letter_jump_unit' not in pout:
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (name, prc))
            print('plant %-34s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    check_contracts(problems)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('letter_jump_selftest: %s (%d plants)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
