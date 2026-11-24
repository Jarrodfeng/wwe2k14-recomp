"""S110 SEAT-B: builds + runs the host unit of src/seats_core.h (tools/seats/seats_unit.cpp) and its planted twins. No game runs.

    py -I tools/seats/seats_selftest.py     compiles seats_unit.cpp with clang++ against src/seats_core.h and runs it (every check must PASS), then
                                            against COPIES of the header that each carry one planted fault (a refusal removed, a bound widened, a
                                            swap skipped, ...) and requires the unit to FAIL on each. The unit writes only under out\\seats_unit\\<tag>.

Exit 0 = the unit passes on the shipped header and fails on every planted twin.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.append(os.path.join(REPO, 'tools', 'harness'))
import plant_runner as pr   # S110 PLANTFAST: the shared bounded pool of the planted builds
HEADER = os.path.join(REPO, 'src', 'seats_core.h')

PLANTS = [
    ('frame_bank_twice_accepted', r'bank_twice = bank_twice \|\| o\.bank == s\.bank;', ';'),
    # the card / frame consistency (a card that does not match the frame is left out)
    ('old_form_inst_accepted', r'if \(c\.inst\.seat != sc::kRefSeat \|\| c\.inst\.bank != sc::kRefBank \|\| sc::FileForm\(c\) != sc::kFileForm\)', 'if (false)'),
    ('file_form_unchecked', r' \|\| sc::FileForm\(c\) != sc::kFileForm\)', ')'),
    ('frame_signature_unchecked', r'if \(c\.inst\.frame_sig != f\.sig\) \{', 'if (false) {'),
    ('frame_without_signature_usable', r'if \(!out\.frame\.has_sig\)(\r?\n)', r'if (false)\1'),
    ('card_against_unsigned_frame', r'  if \(!f\.has_sig\) return "the frame has no signature line";\r?\n', ''),
    ('wide_library_path_lost', r'if \(!sc::ReadCard\(d / "card\.txt", card, err\)\)', 'if (!sc::ReadCard((d / "card.txt").string(), card, err))'),
    ('rows_seat_unchecked', r'if \(r\.seat != sc::kRefSeat\)', 'if (false)'),
    ('seat_not_in_frame_unchecked', r'if \(!fs\) return "seat "', 'if (false) return "seat "'),
    ('missing_pac_accepted', r'if \(!ReadFileHead\(pac, kPacHeader, head, size\)\) return leave', 'if (false) return leave'),
    ('pac_entry_bounds_unchecked', r'if \(off > data_end \|\| len > data_end - off \+ 256\) \{', 'if (false) {'),
    ('duplicate_seat_accepted', r'it != taken\.end\(\)\) \{', 'false) {'),
    ('card_key_vs_folder_unchecked', r'if \(card\.key != folder\) return leave', 'if (false) return leave'),
    ('card_kept_without_relo', r'if \(!r\.has_relo\) return "rows\.bin has no relocation list', 'if (false) return "rows.bin has no relocation list'),
    # (S110 LIBSIZE-1 review) an alias row not in the reference form is left out at load (it would be registered as itself)
    ('card_alias_ref_form_unchecked', r'if \(!sb::AliasRefForm\(r\.alias\[i\]\)\)', 'if (false)'),
    # S110 LIBSIZE: ONE loader; a seated card is re-targeted to his seat at load, a library character stays in the reference form
    ('seated_rows_not_retargeted', r'    rows = std::move\(seat_rows\);', '    (void)seat_rows;'),
    ('seated_node_rows_not_retargeted', r'    r\.hd = std::move\(hd\), r\.sd = std::move\(sd\);', '    (void)hd, (void)sd;'),
    ('seated_rank_not_the_frames', r'out\.pack_id, fs_->rank, seat_rows', 'out.pack_id, 0, seat_rows'),
    ('seated_bank_is_the_reference', r'r\.bank = seated \? fs_->bank : sc::kRefBank;', 'r.bank = sc::kRefBank;'),
    ('seated_was_not_the_frames', r'    r\.was = fs_->was;', '    ;'),
    ('library_no_inst_left_out_noisily', r'  if \(!seated && !card\.inst\.valid\) return;(\r?\n)', r'\1'),
    ('library_card_holds_a_seat', r'out\.left_out\.push_back\(folder \+ " \(library\): " \+ why\);', 'out.left_out.push_back(folder + " (library): " + why); out.held[card.inst.seat] = why;'),
    ('library_node_rows_hd_only', r'if \(!NodeRows\(head, size, false, r\.hd, why\) \|\| !NodeRows\(head, size, true, r\.sd, why\)\) return leave', 'if (!NodeRows(head, size, false, r.hd, why)) return leave'),
    # the seat's EV file
    ('ev_file_bank_not_the_seats', r'sc::kRefSeat, sc::kRefBank, s, bank_s, e2, &a\)', 'sc::kRefSeat, sc::kRefBank, s, sc::kRefBank, e2, &a)'),
    ('ev_file_name_not_moved', r'sb::EvName\(r\.name, sc::kRefSeat, s, sc::kRefBank, bank_s\)', 'r.name'),
    ('ev_file_missing_pac_accepted', r'if \(!ReadFileHead\(pac, kPacHeader, head, fsize\)\) return err', 'if (false) return err'),
    # the frame's lines
    ('frame_dup_seat_accepted', r'if \(f\.Find\(s\.seat\)\) \{', 'if (false) {'),
    ('frame_bank_zero_accepted', r's\.bank < 1 \|\| ', ''),
    ('frame_last_seat_600', r'kLastId = 499', 'kLastId = 599'),
    # the availability rule
    ('hide_ignores_occupant', r'return r\.frame\.Find\(int\(id\)\) && st == SeatState::kEmpty;', 'return r.frame.Find(int(id));'),
    ('hide_offers_held_seat', r'  if \(st == SeatState::kHeld\) return true;\r?\n', ''),
    # an unmigrated old pack: its stars' seats are held
    ('old_pack_holds_none', r'if \(s >= kFirstId && s <= kLastId\) out\.held\[s\] = kOldPackReason;', ';'),
    # D1: a seat whose card exists but was left out is HELD: never purged
    ('held_seat_purged', r'\{ return StateOf\(r, id\) == SeatState::kHeld; \}', '{ return false; }'),
    ('left_out_card_holds_nothing', r'if \(!out\.held\.count\(card\.seat\)\) out\.held\[card\.seat\] = folder \+ ": " \+ why;', ';'),
    ('empty_seat_kept_too', r'return r\.held\.count\(int\(id\)\) \? SeatState::kHeld : SeatState::kEmpty;', 'return SeatState::kHeld;'),
    ('hide_seats_not_in_frame', r'return r\.frame\.Find\(int\(id\)\) && ', 'return '),
    ('seat_over_active_row', r'return live_active_byte == 0;', 'return true;'),
    # table 102, names
    ('t102_key_not_masked', r'\(key16 & 0xFFFF\) && ', 'key16 && '),
    ('t102_without_block', r' && s\.rows\.has_t102\)', ')'),
    ('names_all_the_first', r'&s->rows\.names\[k\]', '&s->rows.names[0]'),
    # DSBD, the bounds
    ('dsbd_swaps_the_whole_row', r'for \(int i = 0; i < 8; i \+= 2\) std::swap', 'for (int i = 0; i < 80; i += 2) std::swap'),
    ('dsbd_no_swap', r'for \(int i = 0; i < 8; i \+= 2\) std::swap', 'for (int i = 0; i < 0; i += 2) std::swap'),
    ('dsbd_buffer_600_rows', r'kDsbdRows = 500', 'kDsbdRows = 600'),
    ('row_addr_past_499', r'seat > kLastId \|\| !rows_array', '!rows_array'),
    ('profile_off_by_one', r'out = kProfileBase \+ kProfileSize \* uint32_t\(seat - kFirstId\);', 'out = kProfileBase + kProfileSize * uint32_t(seat - kFirstId + 1);'),
    # the BIN / LSD builders
    ('bin5_always_appends', r'Be16\(&a\.bytes\[size_t\(i\) \* 16\]\) == uint16_t\(seat\)', 'false'),
    ('bin_swap_not_recorded', r'    a\.swap_at\.push_back\(at \* 16\);\r?\n', ''),
    ('binp_index_not_set', r'    a\.bytes\[at\] = uint8_t\(a\.count & 0xFF\), a\.bytes\[at \+ 1\] = uint8_t\(a\.count >> 8\);\r?\n', ''),
    ('binp_index_big_endian', r'a\.bytes\[at\] = uint8_t\(a\.count & 0xFF\), a\.bytes\[at \+ 1\] = uint8_t\(a\.count >> 8\)', 'a.bytes[at] = uint8_t(a.count >> 8), a.bytes[at + 1] = uint8_t(a.count & 0xFF)'),
    ('lsd_text_word_unswapped', r'PutBe32\(&m\[4\], Le32\(le \+ 4\)\);', 'std::memcpy(&m[4], le + 4, 4);'),
    # the node rows
    ('node_sd_not_renamed', r"out_tag\[1\] = 'S';", ';'),
    ('node_hd_keeps_sd_dirs', r'\(!sd && is_sd_render\)', 'false'),
    ('node_flags_without_kind', r'\(kind == 1 \? 0x1000u : 0u\)', '0u'),
    ('node_without_terminator', r'  PutBe32\(w, kTerm\);\r?\n  out\.insert\(out\.end\(\), w, w \+ 4\);\r?\n  return true;', '  return true;'),
]


ALIAS_PLANTS = [
    ('findfirst_takes_the_last_node', r'for \(const NodeView& n : game\) \{', 'for (auto it = game.rbegin(); it != game.rend(); ++it) { const NodeView& n = *it;'),
    ('findfirst_ignores_the_ours_flag', r'n\.ours \|\| IsOurPath\(n\.path\)', 'IsOurPath(n.path)'),
    ('findfirst_ignores_our_paths', r'n\.ours \|\| IsOurPath\(n\.path\)', 'n.ours'),
    ('findfirst_matches_hex_directory', r'!hex && ', ''),
    ('findfirst_matches_inner_level', r'level0 && ', ''),
    ('findfirst_sector_size_swapped', r'sector = sb::Be32\(r \+ rs - 8\), size = sb::Be32\(r \+ rs - 4\)', 'size = sb::Be32(r + rs - 8), sector = sb::Be32(r + rs - 4)'),
    ('missing_name_gets_a_row', r'(missing->push_back\([^\n]*\);\r?\n)        continue;', r'\1        path = "NONE";'),
    ('plate_row_for_a_seat_without_one', r'if \(a\.tag == "MOVN" && !sb::OwnPlate\(s\)\) continue;', ''),
    ('names_not_retargeted', r'sb::RetargetRowName\(a\.tag, a\.kind, &name\[0\], sc::kRefSeat, s, sc::kRefBank, bank->second\);', ''),
    ('bank_not_the_seats', r's, sc::kRefBank, bank->second\)', 's, sc::kRefBank, sc::kRefBank)'),
    ('directory_per_path_not_grouped', r'by_path\[path\]\[\{a\.tag, a\.kind\}\]', 'by_path[a.tag + path][{a.tag, a.kind}]'),
    ('node_without_terminator', r'    sb::PutBe32\(w, 0xFFFFFFFFu\);[^\r\n]*\r?\n    v\.insert\(v\.end\(\), w, w \+ 4\);', ''),
    ('kind_bits_lost', r'\(key\.second \? 0x1000u : 0u\)', '0u'),
    # (S110 ALIASMISS) a game node's rows end at the first word with a 0xFF TOP BYTE (a group marker 0xFFFF00kk), and the span includes that word
    ('rows_span_stops_only_at_ffffffff', r'\(w >> 24\) == 0xFF', 'w == 0xFFFFFFFFu'),
    ('rows_span_without_marker', r'return span = at \+ 4, true;', 'return span = at, true;'),
    # (S110 LIBSIZE-1 review) a name the re-naming leaves as it is would be registered as itself; two seats with one bank make one name twice
    ('alias_not_ref_form_accepted', r'if \(!sb::AliasRefForm\(a\)\)', 'if (false)'),
    ('alias_shared_bank_accepted', r'seat_of_bank\.emplace\(bank->second, s\); !fresh\)', 'seat_of_bank.emplace(bank->second, s); false && !fresh)'),
]

# seat_bind_core.h's AliasRefForm, through the alias unit (the rule both the loader and AliasNodes apply)
REFFORM_PLANTS = [
    ('alias_evp_tag_accepted', r'\{"EVZ ", "GMEV", "PCE6"', '{"EVP ", "EVZ ", "GMEV", "PCE6"'),
    ('alias_name_not_renamed_accepted', r'  return RetargetRowName\(a\.tag, a\.kind, &n\[0\], sc::kRefSeat, ', '  return true || RetargetRowName(a.tag, a.kind, &n[0], sc::kRefSeat, '),
]

# S110 LIBSIZE lane 3: src\seat_alias_plan.h (which nodes are ours, the alias nodes to write / empty, the EV file's stamp)
PLAN_PLANTS = [
    ('markours_size_mismatch_ignored', r'if \(game\.size\(\) != addrs\.size\(\)\) return 0;', ''),
    ('markours_marks_all', r'if \(addrs\[i\] && ours\.count\(addrs\[i\]\)\)', 'if (addrs[i])'),
    ('markours_zero_address_ours', r'addrs\[i\] && ', ''),
    ('plan_rewrites_unchanged', r'it == last\.end\(\) \|\| it->second != rows', 'true'),
    ('plan_forgets_gone_paths', r'if \(wanted\.count\(path\) \|\| rows == EmptyNodeRows\(\)\) continue;', 'continue;'),
    ('plan_empties_again', r' \|\| rows == EmptyNodeRows\(\)\) continue;', ') continue;'),
    ('empty_node_unterminated', r', 0xFF, 0xFF, 0xFF, 0xFF\};', '};'),
    ('stamp_ignores_seat', r'const int32_t words\[4\] = \{seat, bank,', 'const int32_t words[4] = {0, bank,'),
    ('stamp_ignores_bank', r'const int32_t words\[4\] = \{seat, bank,', 'const int32_t words[4] = {seat, 0,'),
    ('stamp_ignores_key', r'uint64_t h = Fnv1a\(reinterpret_cast<const uint8_t\*>\(key\.data\(\)\), key\.size\(\)\);', 'uint64_t h = 0xCBF29CE484222325ull;'),
    ('stamp_ignores_content', r'if \(f\.bytes\.size\(\) > sb::kEvHeader\) h = ', 'if (false) h = '),
    ('stamp_ignores_rows', r'for \(const sb::EvRow& r : f\.rows\) \{', 'for (const sb::EvRow& r : std::vector<sb::EvRow>()) {'),
    ('stamp_size_unchecked', r'disk_size == f\.bytes\.size\(\) && ', ''),
    ('stamp_not_in_header', r'sb::PutBe32\(&f\.bytes\[0\], kStampMagic\);', ''),
    ('write_always', r'if \(size && StampMatches\(head, size, f\)\) return true;', ''),
    ('headerless_file_written', r'if \(f\.bytes\.size\(\) < sb::kEvHeader\) return false;', ''),
    # (review S110 LIBSIZE-3) the retry of a refused Refresh
    ('retry_success_keeps_debt', r'if \(ok\) return pending = false, tries = 0, false;', 'if (ok) return tries = 0, false;'),
    ('retry_refusal_not_owed', r'  pending = true;\r?\n    if \(\+\+tries', '  if (++tries'),
    ('retry_never_gives_up', r'if \(\+\+tries < kTries\) return false;', 'return ++tries, false;'),
    ('retry_owe_keeps_count', r'void Owe\(\) \{ pending = true, tries = 0; \}', 'void Owe() { pending = true; }'),
]

CARD_PLANTS = [
    ('alia_block_not_written', r'  if \(!r\.alias\.empty\(\)\) \{(\r?\n)    std::string p;', r'  if (false) {\1    std::string p;'),
    ('alias_names_not_checked', r'a\.game\.size\(\) != AliasNameLen\(a\.kind\) \|\| a\.ours\.size\(\) != AliasNameLen\(a\.kind\)', 'false'),
    ('file_form_never_read', r'if \(kv\.first == "file_form"\) return std::atoi\(kv\.second\.c_str\(\)\);', 'if (kv.first == "file_form") return 0;'),
    ('file_form_line_added_twice', r'if \(kv\.first == "file_form"\) return void\(kv\.second = std::to_string\(v\)\);', ';'),
]


# the headers of the unit's include chain that live in src and include each other by quotes: a planted copy is only seen by the ones next to it, so the
# planted run copies them all beside the plant (each include "x.h" then finds the sibling first)
SIBLINGS = ['seats_core.h', 'seat_bind_core.h', 'seat_alias_core.h', 'seat_alias_plan.h', 'seat_card.h']


def run_test(exe, unit, tag):
    work = os.path.join(REPO, 'out', unit, tag)
    try:
        r = subprocess.run([exe, work], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def run_unit(unit, header_name, plants, problems, jobs=None):
    tmp = tempfile.mkdtemp(prefix=unit + '_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool, each in its own folder under tmp
        # (the planted copy first on -I, then src for the other headers); the verdicts below are the serial script's, in plant order
        defs = ['-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING']
        sh, outs = pr.run_unit_plants([os.path.join(HERE, unit + '.cpp')], [os.path.join(REPO, 'src')], ['-std=c++20', '-Wall'] + defs,
                                      os.path.join(REPO, 'src', header_name), [os.path.join(REPO, 'src', s) for s in SIBLINGS], plants,
                                      lambda exe, tag: run_test(exe, unit, tag), unit + '.exe', tmp, jobs, fail_tail=800,
                                      caught=lambda rc, out: rc != 0, run_args=lambda tag: [os.path.join(REPO, 'out', unit, tag)])
        if sh.error:
            raise RuntimeError(sh.error)
        rc, out = sh.rc, sh.out
        sys.stdout.write(out)
        if rc != 0:
            problems.append('%s FAILS on the shipped %s' % (unit, header_name))
        for o in outs:
            name, prc, pout = o.name, o.rc, o.out
            if o.matches != 1:
                problems.append('plant %s: the pattern matched %d times (%s changed under the plant)' % (name, o.matches, header_name))
                continue
            if o.error:
                problems.append('plant %s: %s' % (name, o.error))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            print('plant %-34s %s %s (%d failing checks)%s' % (name, unit, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(os.path.join(REPO, 'out', unit), ignore_errors=True)


def main():
    problems = []
    jobs = pr.parse_jobs(sys.argv[1:])
    run_unit('seats_unit', 'seats_core.h', PLANTS, problems, jobs)
    run_unit('alias_unit', 'seat_alias_core.h', ALIAS_PLANTS, problems, jobs)
    run_unit('alias_unit', 'seat_card.h', CARD_PLANTS, problems, jobs)
    run_unit('alias_unit', 'seat_bind_core.h', REFFORM_PLANTS, problems, jobs)
    run_unit('alias_plan_unit', 'seat_alias_plan.h', PLAN_PLANTS, problems, jobs)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('seats_selftest: %s' % ('OK' if not problems else '%d problem(s)' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
