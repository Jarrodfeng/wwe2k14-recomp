"""S110 SEAT-A0: builds + runs the host unit of src/seat_card.h (tools/seats/seat_card_unit.cpp) and its planted twins. No game runs.

    py -I tools/seats/seat_card_selftest.py     compiles seat_card_unit.cpp with clang++ against src/seat_card.h and runs it (every check must PASS), then
                                                against COPIES of the header that each carry one planted fault (a skipped size check, ...) and
                                                requires the unit to FAIL on each.

Exit 0 = the unit passes on the shipped header and fails on every planted twin.
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
HEADER = os.path.join(REPO, 'src', 'seat_card.h')

PLANTS = [
    ('rows_block_size_unchecked', r'if \(len != want\) \{', 'if (false) {'),
    ('rows_binp_size_unchecked', r'if \(len != 1 \+ kBinpRow\) \{', 'if (false) {'),
    ('rows_lsd_size_unchecked', r'if \(len != 1 \+ kLsdRow\) \{', 'if (false) {'),
    ('rows_t102_size_unchecked', r'if \(len != kT102Size\) \{', 'if (false) {'),
    ('rows_bad_magic_accepted', r'std::memcmp\(f\.data\(\), kRowsMagic, 8\) != 0', 'false'),
    ('rows_unknown_tag_skipped', r'err = "rows: unknown tag \'" \+ tag \+ "\' \(format version 1 is closed\)";\r?\n\s*return false;', 'continue;'),
    ('rows_truncated_payload_accepted', r'if \(len > f\.size\(\) - at\)', 'if (len > f.size())'),
    ('rows_truncated_header_accepted', r'if \(f\.size\(\) - at < 8\)', 'if (false)'),
    ('rows_dup_binp_leaf_read', r'if \(o\.k == p\[0\]\) \{', 'if (false) {'),
    ('rows_dup_lsd_list_read', r'if \(o\.list == p\[0\]\) \{', 'if (false) {'),
    ('rows_dup_binp_leaf_write', r'if \(r\.binp\[j\]\.k == r\.binp\[i\]\.k\) \{', 'if (false) {'),
    ('rows_dup_lsd_list_write', r'if \(r\.lsd\[j\]\.list == r\.lsd\[i\]\.list\) \{', 'if (false) {'),
    ('rows_binp_leaf_unchecked', r'if \(p\[0\] >= kBinpLeaves\) \{', 'if (false) {'),
    ('rows_name_count_unchecked', r'if \(names != 3\) \{', 'if (false) {'),
    ('rows_dup_block_accepted', r'if \(got\) \{ err = "rows: block " \+ tag \+ " twice"; return false; \}', ''),
    ('rows_missing_bin5_accepted', r'if \(!got_bin5\) \{', 'if (false) {'),
    ('rows_name_too_long_read', r'if \(int\(len\) > kMaxNameBytes\) \{', 'if (false) {'),
    ('rows_seat_little_endian', r'r\.seat = int\(detail::Get16\(b \+ 8\)\);', 'r.seat = int(b[8] | (b[9] << 8));'),
    ('write_not_atomic', r'  tmp \+= "\.tmp";', ''),
    ('wide_path_lost', r'const auto s = p\.u8string\(\);\r?\n  return std::string\(s\.begin\(\), s\.end\(\)\);', 'return std::string(p.string());'),
    ('card_version_accepted', r'if \(v != kCardVersion\) \{', 'if (false) {'),
    ('card_key_unchecked_read', r'if \(!ValidKey\(rest\)\) \{', 'if (false) {'),
    ('card_key_lowercase_ok', r"c <= 'F'\)\)\) return false;", "c <= 'f'))) return false;"),
    ('card_key_7_digits_ok', r'k\.size\(\) != 8', 'k.size() < 7'),
    ('card_unknown_key_dropped', r'default: c\.extra\.emplace_back\(k, rest\); break;', 'default: break;'),
    ('card_tron_line_never_written', r'if \(c\.tron_from > 0 \|\| c\.tron_slot > 0\)', 'if (false)'),
    ('card_movie_never_written', r'if \(c\.inst\.movie > 0\)', 'if (false)'),
    ('card_dup_key_accepted', r'if \(seen\[idx\]\) \{', 'if (false) {'),
    ('card_first_line_free', r'if \(!have_ver && k != "card"\) \{', 'if (false) {'),
    ('card_write_unchecked', r'if \(!CheckCard\(c, e\)\) \{ if \(err\) \*err = "card: " \+ e; return false; \}', ''),
    # ---- the relocation list (RELO, S110 SEAT3-R)
    ('relo_unknown_kind_accepted', r'    if \(k < kRelKindFirst \|\| k > kRelKindLast\) return [^\r\n]*', ''),
    ('relo_arg_unchecked', r'    if \(text \? e\.arg > 2 : e\.kind == RelKind::PartBank16 \? e\.arg > 9 : e\.arg != 0\) return [^\r\n]*', ''),
    ('relo_missing_block_accepted', r'    if \(size < 0\) return err = at \+ "no such block[^\r\n]*', ''),
    ('relo_past_block_accepted', r'    if \(size_t\(e\.off\) \+ w > size_t\(size\)\) return [^\r\n]*', ''),
    ('relo_overlap_accepted', r'      if \(o\.tag == e\.tag && o\.sub == e\.sub && size_t[^\r\n]*', '      ;'),
    ('relo_binp_leaf_ignored', r'if \(b\.k == sub\) return int\(kBinpRow\);', 'if (true) return int(kBinpRow);'),
    ('relo_lsd_list_ignored', r'if \(l\.list == sub\) return int\(kLsdRow\);', 'if (true) return int(kLsdRow);'),
    ('relo_block_twice_accepted', r'      if \(r\.has_relo\) \{ err = "rows: block RELO twice"; return false; \}', ''),
    ('relo_entries_without_block_accepted', r'    if \(!r\.relo\.empty\(\)\) return err = "rows: relocation entries without a RELO block", false;', ''),
    ('relo_not_written', r'  if \(r\.has_relo\) \{(\r?\n    std::string p;\r?\n    detail::Put32\(p, uint32_t\(r\.relo\.size\(\)\)\);)', r'  if (false) {\1'),
    ('relo_offset_little_endian', r'      detail::Put16\(p, e\.off\);', '      p += char(e.off), p += char(e.off >> 8);'),
    ('relo_arg_not_read', r'e\.arg = q\[8\];', 'e.arg = 0;'),
    ('relo_sub_not_read', r'e\.sub = q\[4\], ', 'e.sub = 0, '),
    ('relo_not_checked_after_read', r'  if \(!CheckRelo\(r, err\)\) return false;', ''),
    ('relo_not_checked_before_write', r'  return CheckRelo\(r, err\) && CheckAlias\(r, err\);', '  return CheckAlias(r, err);'),]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(header_text, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'seat_card.h'), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'seat_card_unit.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-Wall', '-I', inc, os.path.join(HERE, 'seat_card_unit.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('seat_card_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
    return exe


def run_test(exe):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    text = open(HEADER, encoding='utf-8', newline='').read()
    problems = []
    tmp = tempfile.mkdtemp(prefix='seat_card_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order
        once = pr.Once(pr.make_spec(os.path.join(HERE, 'seat_card_unit.cpp'), {os.path.basename(HEADER): HEADER}, ['-std=c++17', '-Wall']), lambda rc, out: rc != 0, os.path.basename(HEADER))
        sh, outs = pr.run_plants(text, PLANTS, lambda t, tag: build(t, tmp, tag), lambda exe, tag: run_test(exe), pr.parse_jobs(sys.argv[1:]), once)
        if sh.error:
            raise RuntimeError(sh.error)
        rc, out = sh.rc, sh.out
        sys.stdout.write(out)
        if rc != 0:
            problems.append('the unit FAILS on the shipped header')
        for o in outs:
            name, prc, pout = o.name, o.rc, o.out
            if o.matches != 1:
                problems.append('plant %s: the pattern matched %d times (the header changed under the plant)' % (name, o.matches))
                continue
            if o.error:
                problems.append('plant %s: %s' % (name, o.error))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            print('plant %-34s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:70].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('seat_card_selftest: %s' % ('OK' if not problems else '%d problem(s)' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
