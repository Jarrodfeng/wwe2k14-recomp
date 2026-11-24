"""S110 LETTERUI: builds + runs the host unit of src/letter_ui_core.h (tools/letter_ui/letter_ui_unit.cpp) and its planted twins. No game runs.

    py -I tools/letter_ui/letter_ui_selftest.py   compiles letter_ui_unit.cpp with clang++ against src/letter_ui_core.h (+ src/letter_jump_core.h) and runs it
                                                  (every check must PASS), then against COPIES of the headers that each carry one planted fault (a rule removed
                                                  or reversed) and requires the unit to FAIL on each, by a printed FAIL line and its summary (no crash).

Exit 0 = the unit passes on the shipped headers and fails on every planted twin. The plant patterns use \\r?\\n where they span a line (CRLF checkouts).
The game side (src/letter_ui.h) has no host harness; its wiring CONTRACTS are patterns that must be in the file (listed below).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(REPO, 'src')
sys.path.append(os.path.join(REPO, 'tools', 'harness'))
import plant_runner as pr   # S110 PLANTFAST: the shared bounded pool of the planted builds
UI = 'letter_ui_core.h'
JUMP = 'letter_jump_core.h'

# (name, file, pattern, replacement)
PLANTS = [
    # SizeFor
    ('regular_needs_one_row_only', UI, r'same == 1 &&', 'same >= 1 &&'),
    ('vs_record_ignored', UI, r'has_vs && same == 1', 'same == 1'),
    ('centre_is_regular', UI, r' && rows\[k\]\.side != 1\)', ')'),
    ('referee_gets_a_box', UI, r'if \(k < 0 \|\| k > 5 \|\| rows\[k\]\.kind != 0\) return Box::None;', 'if (k < 0 || k > 5) return Box::None;'),
    ('manager_counts_on_the_side', UI, r'rows\[i\]\.kind == 0 && rows\[i\]\.side == rows\[k\]\.side', 'rows[i].side == rows[k].side'),
    # strings
    ('glyph_lt_wrong', UI, r'return "\\xEE\\x80\\x8C";', r'return "\\xEE\\x80\\x8D";'),
    ('glyph_rt_wrong', UI, r'return "\\xEE\\x80\\x8F";', r'return "\\xEE\\x80\\x8E";'),
    ('current_letter_tagged', UI, r's \+= cur;', 's += Dimmed(cur, dim_rgba);'),
    ('small_shows_neighbours', UI, r'if \(box == Box::Regular\) Neighbours', 'Neighbours'),
    ('prev_not_wrapped', UI, r'if \(!prev\) prev = hi;  // wrap', ';'),
    ('next_not_wrapped', UI, r'if \(!next\) next = lo;', ';'),
    ('prev_is_the_larger', UI, r'if \(c < cur\) prev = c;', 'if (c > cur) prev = c;'),
    ('no_letter_draws_strip', UI, r'if \(cur == 0 \|\| box == Box::None\) return \{\};', 'if (box == Box::None) return {};'),
    ('top_row_without_provider', UI, r'if \(!downloaded_tab \|\| pages <= 0 \|\| page < 1\) return \{\};', 'if (!downloaded_tab) return {};'),
    ('top_row_on_every_tab', UI, r'if \(!downloaded_tab \|\| ', 'if ('),
    ('present_letters_ignores_selectable', JUMP, r'if \(r\.selectable && r\.letter != 0\) present', 'if (r.letter != 0) present'),
    # override parser
    ('unknown_key_silent', UI, r'if \(!done\) bad\(', 'if (false) bad('),
    ('bad_number_applied', UI, r'if \(val\.empty\(\) \|\| end == val\.c_str\(\) \|\| \*end != 0 \|\| !std::isfinite\(f\)\) bad', 'if (false) bad'),
    ('trailing_junk_ok', UI, r'\|\| \*end != 0 ', ' '),
    ('pill_option_keeps_glyphs', UI, r"std::string s = glyphs \? std::string\(GlyphLT\(\)\) \+ ' ' : std::string\(\);", "std::string s = std::string(GlyphLT()) + ' ';"),
    # how a panel reaches its bar
    ('bar_slot_sides_swapped', UI, r'side == 0 \? kBarSlotLeft : side == 2 \? kBarSlotRight :', 'side == 0 ? kBarSlotRight : side == 2 ? kBarSlotLeft :'),
    ('bar_slot_list_mode_ignored', UI, r'\(list_mode \? kBarSlotCentreList : kBarSlotCentre\)', 'kBarSlotCentre'),
    ('bar_group_not_per_panel', UI, r'r\.group = group_of_panel\[k\];', 'r.group = group_of_panel[0];'),
    ('group_read_from_the_record_word', UI, r'int\(words\[2 \* k \+ 1\]\)', 'int(words[2 * k])'),
    ('group_off_by_one_slot', UI, r'int\(words\[2 \* k \+ 1\]\)', 'int(words[(2 * k + 3) % 12])'),
    ('bar_index_is_not_the_slot', UI, r'r\.index = slot;', 'r.index = 6;'),
    # source lists
    ('source_first_does_not_win', UI, r'if \(resolves\(bases\[i\]\)\) return int\(i\);', 'if (resolves(bases[bases.size() - 1 - i])) return int(bases.size() - 1 - i);'),
    ('source_none_is_zero', UI, r'return -1;', 'return 0;'),
    ('split_keeps_empty_items', UI, r'if \(bar > pos\) out\.push_back', 'out.push_back'),
    # the underline's x
    ('underline_offset_ignores_before', UI, r'return before \+ r\.adv\[size_t\(r\.cur\)\] / 2 - total / 2;', 'return r.adv[size_t(r.cur)] / 2 - total / 2;'),
    ('underline_offset_not_centred', UI, r'return before \+ r\.adv\[size_t\(r\.cur\)\] / 2 - total / 2;', 'return before + r.adv[size_t(r.cur)] / 2;'),
    ('run_always_has_icons', UI, r'if \(glyphs\) \{\r?\n    add\(L\.adv_icon\);', 'if (true) {\n    add(L.adv_icon);'),
    ('underline_not_at_the_letter', UI, r'p\.ul_x = p\.text_x \+ cur_dx;', 'p.ul_x = p.text_x;'),
    # placement / mirror
    ('mirror_not_negated', UI, r'const float s = \(side == 0\) \? -1\.0f : 1\.0f;', 'const float s = 1.0f;'),
    ('centre_small_goes_left', UI, r'const float s = \(side == 0\) \? -1\.0f', 'const float s = (side != 2) ? -1.0f'),
    ('regular_never_shrinks', UI, r'p\.h = has_top_row \? L\.reg_h2 : L\.reg_h;', 'p.h = L.reg_h2;'),
    ('text_tied_to_box_offset', UI, r'p\.text_x = p\.cx;', 'p.text_x = p.cx + 5;'),
    # the box pieces by centres
    ('cap_treated_as_left_anchored', UI, r'b\.xl = x0 \+ b\.left_cap / 2;', 'b.xl = x0;'),
    ('right_cap_treated_as_left_anchored', UI, r'b\.xr = x0 \+ p\.w - b\.right_cap / 2;', 'b.xr = x0 + p.w - b.right_cap;'),
    ('middle_not_centred', UI, r'b\.xc = x0 \+ b\.left_cap \+ span / 2;', 'b.xc = x0 + b.left_cap;'),
    ('middle_not_stretched', UI, r'b\.sxc = n\.cw > 0\.5f \? b\.mid_w / n\.cw : 1\.0f;', 'b.sxc = 1.0f;'),
    ('overlap_ignored', UI, r'b\.mid_w = span \+ 2 \* L\.c_overlap;', 'b.mid_w = span;'),
    ('box_y_not_stretched', UI, r'b\.sy = n\.h > 0\.5f \? p\.h / n\.h : 1\.0f;', 'b.sy = 1.0f;'),
    ('native_sizes_ignored', UI, r'const float lw = b\.swap \? n\.rw : n\.lw, rw = b\.swap \? n\.lw : n\.rw;', 'const float lw = 16, rw = 16;'),
    ('cap_shape_option_ignored', UI, r'\(L\.cap_keep_shape != 0\.0f\) \? b\.sy : L\.cap_sx', 'L.cap_sx'),
    ('flip_never_swaps', UI, r'b\.swap = \(L\.flip_slant != 0\.0f\) && side == 2;', 'b.swap = false;'),
    ('flip_on_both_sides', UI, r'\(L\.flip_slant != 0\.0f\) && side == 2;', '(L.flip_slant != 0.0f);'),
    # the box rule (lane STRIPTUNE): even caps on both boxes, an impossible box refused
    ('refusal_never_set', UI, r'b\.refused = b\.left_cap \+ b\.right_cap \+ L\.mid_min > p\.w;', 'b.refused = false;'),
    ('refusal_ignores_mid_min', UI, r'\+ L\.mid_min > p\.w;', '> p.w;'),
    ('caps_default_uneven', UI, r'float cap_keep_shape = 1;', 'float cap_keep_shape = 0;'),
    ('small_height_back_to_30', UI, r'float small_h = 24;', 'float small_h = 30;'),
    ('regular_height_back_to_46', UI, r'float reg_h = 40;', 'float reg_h = 46;'),
    ('two_row_height_back_to_64', UI, r'float reg_h2 = 48;', 'float reg_h2 = 64;'),
    ('small_line_like_regular', UI, r'\(box == Box::Small \? L\.small_ul_dy : L\.ul_dy\)', 'L.ul_dy'),
]

# The game side's wiring contracts (each from the listings: LETTERUI.md claims 4 / 5 and the proof listings of sub_823C54C8 / sub_82860CF0).
GAME = os.path.join(SRC, 'letter_ui.h')
CONTRACTS = [
    ('tick_hook_is_the_panel_tick', r'REX_HOOK_RAW\(sub_82860CF0\)'),
    ('tick_reads_r3_before_the_game_runs', r'const uint32_t panel = ctx\.r3\.u32;\s*__imp__sub_82860CF0\(ctx, base\);'),
    ('draw_hook_is_the_element_draw', r'REX_HOOK_RAW\(sub_823C54C8\)'),
    ('draw_reads_r3_r4_r5_before_the_game_runs', r'const uint32_t r3 = ctx\.r3\.u32, r4 = ctx\.r4\.u32, r5 = ctx\.r5\.u32;'),
    ('draw_notes_which_panels_bar_group_is_drawn', r'g_cur_panel = entered[\s\S]*?__imp__sub_823C54C8\(ctx, base\);[\s\S]*?g_cur_panel = prev;'),
    ('the_bar_slot_is_the_panels_word_37020', r'kPanelBarSlot = 37020'),
    ('the_bar_table_starts_at_0x82038480', r'kBarTable = 0x82038480'),
    ('the_bar_group_is_read_through_the_table_word_pair', r'words\[i\] = seats::U32\(base, kBarTable \+ 4 \* i\);[\s\S]*GroupFromTable\(words, int\(k\)\)'),
    ('the_children_are_the_nodes_own_array', r'seats::U32\(base, b\.node \+ 100\)[\s\S]*seats::U32\(base, b\.node \+ 76\)'),
    ('the_anchor_is_a_child_of_the_bar_node', r'FindIn\(base, bar\.first, bar\.count, L\.anchor\)'),
    ('the_box_list_first_that_resolves_wins', r'FirstResolving\(bases, '),
    ('off_means_only_the_game', r'if \(!REXCVAR_GET\(wwe2k14_letter_ui\)\)'),
    ('no_recursion_guard', r'g_draw_depth > 0'),
    ('copies_refreshed_before_our_fields', r'std::memcpy\(seats::P\(base, s\.copy\[i\]\), seats::P\(base, s\.src\[i\]\), kElemSize\)'),
    ('drawn_only_where_the_game_drew_the_anchor', r'seats::U32\(base, r4 \+ 144\) != 0'),
    ('the_jumps_own_gate', r'letter_jump::GateOf\(call, base, panel, g\)'),
    ('the_jumps_own_letters', r'letter_jump::LettersOf\(call, base, panel, pl\)'),
    ('the_games_own_current_row', r'call\(sub_828568B8, \{panel\}\)'),
    ('guest_reads_are_checked', r'seats::Readable\(e\.arr, kElemSize \* e\.count\)'),
]


def check_contracts(problems):
    text = open(GAME, encoding='utf-8', newline='').read()
    for name, pat in CONTRACTS:
        ok = re.search(pat, text) is not None
        print('contract %-44s %s' % (name, 'held' if ok else 'MISSING'))
        if not ok:
            problems.append('contract %s: src/letter_ui.h has no match for %s' % (name, pat))


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(texts, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    for name, text in texts.items():
        with open(os.path.join(inc, name), 'w', encoding='utf-8', newline='') as f:
            f.write(text)
    exe = os.path.join(inc, 'letter_ui_unit.exe')
    r = subprocess.run([find_clang(), '-std=c++20', '-Wall', '-I', inc, os.path.join(HERE, 'letter_ui_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('letter_ui_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
    return exe, (r.stdout + r.stderr)


def run_test(exe, tag=None):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    texts = {n: open(os.path.join(SRC, n), encoding='utf-8', newline='').read() for n in (UI, JUMP)}
    problems = []
    tmp = tempfile.mkdtemp(prefix='letter_ui_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order
        variants = []
        for name, fname, pat, rep in PLANTS:
            new, n = re.subn(pat, lambda m: rep, texts[fname], count=1)
            planted = dict(texts)
            planted[fname] = new
            variants.append((name, n, planted))
        once = pr.Once(pr.make_spec(os.path.join(HERE, 'letter_ui_unit.cpp'), {UI: os.path.join(SRC, UI), JUMP: os.path.join(SRC, JUMP)}, ['-std=c++20', '-Wall']),
                       pr.fail_lines('letter_ui_unit'), plants=[pr.Plant(n, f, p, r, True) for n, f, p, r in PLANTS])   # the replacements are literal (lambda m: rep above)
        sh, outs = pr.run_variants(texts, variants, lambda t, tag: build(t, tmp, tag), lambda art, tag: run_test(art[0]), pr.parse_jobs(sys.argv[1:]), once)
        if sh.error:
            raise RuntimeError(sh.error)
        warn, rc, out = sh.art[1], sh.rc, sh.out
        if 'warning' in warn:
            problems.append('the unit compiles with warnings: ' + warn[-300:])
        sys.stdout.write(out)
        if rc != 0 or 'PASS letter_ui_unit' not in out:
            problems.append('the unit FAILS on the shipped headers (or ended without its summary line)')
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
            elif not failed or 'FAIL letter_ui_unit' not in pout:
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (name, prc))
            print('plant %-36s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    check_contracts(problems)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('letter_ui_selftest: %s (%d plants)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
