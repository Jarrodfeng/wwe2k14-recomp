"""S110 SEAT2 (lane H-A): builds + runs the host unit of src/slot_handover_core.h (tools/seats/handover_unit.cpp) and its planted twins. No game runs.

    py -I tools/seats/handover_selftest.py   compiles handover_unit.cpp with clang++ against src/slot_handover_core.h and runs it (every check must
                                             PASS), then against COPIES of the header that each carry one planted fault (a rule removed, a refusal
                                             removed, an order swapped, ...) and requires the unit to FAIL on each. The unit writes only under
                                             out\\handover_unit\\<tag>.

Exit 0 = the unit passes on the shipped header and fails on every planted twin. The plant patterns use \\r?\\n (the checkout has CRLF files).
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
HEADER = os.path.join(REPO, 'src', 'slot_handover_core.h')
NAME = 'slot_handover_core.h'
STRIP = os.path.join(REPO, 'src', 'universe_strip_core.h')  # S110 UNISTRIP: the second header of the unit (its plants carry the file name as a 4th item)
STRIP_NAME = 'universe_strip_core.h'
REBUILD = os.path.join(REPO, 'src', 'universe_rebuild_core.h')  # S110 UNIAVAIL: the third header of the unit
REBUILD_NAME = 'universe_rebuild_core.h'
HEADERS = {NAME: HEADER, STRIP_NAME: STRIP, REBUILD_NAME: REBUILD}  # file name -> its shipped path (a plant's optional 4th item names the file it edits)

PLANTS = [
    # the plan: the table's rows
    ('held_seat_handed_over', r'case st::SeatState::kHeld: break;', 'case st::SeatState::kHeld: if (k) p.leaves.push_back({s, *k}); break;'),
    ('imports_off_not_carried', r'if \(!r\.loaded \|\| r\.old_pack\) \{', 'if (r.old_pack) {'),
    ('old_pack_not_carried', r'if \(!r\.loaded \|\| r\.old_pack\) \{', 'if (!r.loaded) {'),
    ('no_entry_not_adopted', r'if \(!owner\) \{', 'if (false) {'),
    ('same_man_rearrives', r'if \(k && \*k == j\) break;', ''),
    ('empty_seat_not_left', r'case st::SeatState::kEmpty:(\r?\n)        if \(k\) p\.leaves\.push_back\(\{s, \*k\}\);', r'case st::SeatState::kEmpty:\1        ;'),
    ('leave_forgets_old_owner', r'if \(k\) p\.leaves\.push_back\(\{s, \*k\}\);(\r?\n)        p\.arrivals', r'\1        p.arrivals'),
    ('arrivals_only_over_empty_owner', r'p\.arrivals\.push_back\(\{s, j\}\);', 'if (!k) p.arrivals.push_back({s, j});'),
    # the export decision (data loss: a purge after a failed export)
    ('export_failure_ignored', r'if \(!a\.leave\.empty\(\) && export_failed\.count\(a\.seat\)\) \{', 'if (false) {'),
    ('moved_man_arrives_with_stale_state', r'\} else if \(!a\.arrive\.empty\(\) && failed_keys\.count\(a\.arrive\)\) \{', '} else if (false) {'),
    ('failed_seat_owner_changed', r'    if \(!a\.proceed\) continue;(\r?\n)', r'\1'),
    ('failed_seat_pending', r'if \(a\.proceed && !a\.leave\.empty\(\)\) s\.insert\(a\.seat\);', 'if (!a.leave.empty()) s.insert(a.seat);'),
    ('pending_not_carried', r'if \(loaded\) s\.insert\(loaded->pending\.begin\(\), loaded->pending\.end\(\)\);', ';'),
    ('carry_writes_empty_entry', r'if \(!loaded\) return std::nullopt;', 'if (!loaded) return m;'),
    ('carry_loses_owners', r'    return loaded->owners;(\r?\n)  \}', r'    return m;\1  }'),
    ('adopt_forgets_held_owner', r'if \(!ValidSeat\(kv\.first\) \|\| m\.count\(kv\.first\)\) continue;', 'continue;'),
    ('leave_only_keeps_owner', r'    else m\.erase\(a\.seat\);', '    else (void)0;'),
    # the side record
    ('entry_rewritten', r'if \(r\.Find\(e\.stamp\)\) return false;',
     'if (r.Find(e.stamp)) { for (auto it = r.entries.begin(); it != r.entries.end(); ++it) if (it->stamp == e.stamp) { r.entries.erase(it); break; } }'),
    ('record_not_trimmed', r'while \(r\.entries\.size\(\) > kKeepEntries\) \{', 'while (false) {'),
    ('record_oldest_first', r'r\.entries\.insert\(r\.entries\.begin\(\), std::move\(e\)\);', 'r.entries.push_back(std::move(e));'),
    ('record_version_unchecked', r'if \(v != kRecordVersion\) \{', 'if (false) {'),
    ('damaged_line_dropped_alone', r'if \(in_entry\) bad = true;', ';'),
    ('unknown_entry_lines_dropped', r'      cur\.extra\.emplace_back\(k, rest\);', '      ;'),
    ('seat_words_dropped', r'if \(!more\.empty\(\)\) cur\.owner_words\[int\(v\)\] = more;', ';'),
    ('duplicate_seat_accepted', r'\} else if \(cur\.owners\.count\(int\(v\)\)\) \{', '} else if (false) {'),
    ('duplicate_stamp_accepted', r'\} else if \(rec\.Find\(cur\.stamp\)\) \{', '} else if (false) {'),
    ('seat_range_unchecked_in_record', r'!sc::detail::Int\(w\[0\], kFirstSeat, kLastSeat, v\) \|\| ', ''),
    ('pending_not_written', r'if \(!e\.pending\.empty\(\)\) \{', 'if (false) {'),
    ('bad_record_written', r'if \(!CheckRecord\(r, e\)\) \{ if \(err\) \*err = e; return false; \}', ''),
    ('stamp_hash_constant_wrong', r'0x100000001B3ull', '0x100000001B5ull'),
    # (SEAT2-HB) the trim protects the loaded image's entry; the image stamp covers what the game's byte sum covers
    ('trim_drops_the_loaded_entry', r'while \(i > 0 && protect && r\.entries\[i\]\.stamp == \*protect\) --i;', ';'),
    ('image_stamp_skips_footer', r'return Fnv64\(payload \+ kImgStart, kImgEnd - kImgStart\);', 'return Fnv64(payload + kImgStart, kImgFoot - kImgStart);'),
    ('image_stamp_includes_header', r'return Fnv64\(payload \+ kImgStart, kImgEnd - kImgStart\);', 'return Fnv64(payload, kImgEnd);'),
    ('image_stamp_skips_block_a', r'return Fnv64\(payload \+ kImgStart, kImgEnd - kImgStart\);', 'return Fnv64(payload + kImgB, kImgEnd - kImgB);'),
    # (review of H-B) pending kept until the game's loop asked; fresh O6; the status clause on line 1
    ('pending_cleared_unasked', r'    if \(!answered\.count\(s\)\) left\.insert\(s\);', '    (void)answered;'),
    ('fresh_cnt_byte_zero', r'constexpr uint8_t kFreshCntByte = 1;', 'constexpr uint8_t kFreshCntByte = 0;'),
    ('unseen_clause_not_replaced', r'  if \(at != std::string::npos\) \{(\r?\n)    const size_t comma', r'  if (false) {\1    const size_t comma'),
    ('unseen_clause_on_out_line', r'  if \(line\.rfind\("in ", 0\) != 0\) return line;', ';'),
    # the state file
    ('state_bad_magic_accepted', r'std::memcmp\(f\.data\(\), "SEATST", 6\) != 0\)', 'false)'),
    ('state_version_unchecked', r'if \(std::memcmp\(f\.data\(\), kStateMagic, 8\) != 0\) \{', 'if (false) {'),
    ('state_unknown_tag_skipped', r'if \(hit < 0\) \{ err = "state: unknown tag[^\r\n]*\}', 'if (hit < 0) continue;'),
    ('state_short_block_accepted', r'if \(len != slots\[hit\]\.want\) \{', 'if (len > slots[hit].want) {'),
    ('state_duplicate_block_accepted', r'if \(got\[hit\]\) \{', 'if (false) {'),
    ('state_missing_block_accepted', r'if \(!got\[i\]\) \{', 'if (false) {'),
    ('state_truncated_block_accepted', r'if \(len > f\.size\(\) - at\) \{', 'if (len > f.size()) {'),
    ('state_truncated_header_accepted', r'if \(f\.size\(\) < 16\) \{', 'if (f.size() < 12) {'),
    ('state_block_header_cut_accepted', r'if \(f\.size\(\) - at < 8\) \{', 'if (false) {'),
    ('state_seat_unchecked_on_write', r'  if \(!ValidSeat\(s\.seat\)\) \{ err = "state: seat " \+ std::to_string\(s\.seat\) \+ " is not 100\.\.499"; return false; \}(\r?\n)  return true;', r'  return true;'),
    ('state_seat_unchecked_on_read', r'  if \(!ValidSeat\(s\.seat\)\) \{ err = "state: seat " \+ std::to_string\(s\.seat\) \+ " is not 100\.\.499"; return false; \}(\r?\n)  bool got', r'  bool got'),
    ('state_cnt_always', r's\.has_cnt = got\[5\];', 's.has_cnt = true;'),
    ('state_size_cap_off', r'if \(f\.size\(\) > kMaxStateFile\) \{', 'if (false) {'),
    ('state_write_failure_hidden', r'if \(!sc::detail::WriteAtomic\(path, bytes, &err\)\) return false;(\r?\n)  std::string back;[\s\S]*?(\r?\n)  return true;',
     r'sc::detail::WriteAtomic(path, bytes, &err);\2  return true;'),
    ('state_key_path_unchecked', r'if \(!sc::ValidKey\(key\)\) return \{\};', ';'),
    # re-targeting
    ('row_seat_id_not_set', r'  seat_bind::PutBe16\(out \+ seat_bind::kIdOff2, uint16_t\(s\)\);(\r?\n)', r'\1'),
    ('row_keys_not_rewritten', r'return seat_bind::RewriteKeys\(out, t, s\);', 'return 0;'),
    ('row_not_activated', r'  out\[seat_bind::kActiveOff\] = 1;(\r?\n)', r'\1'),
    ('row_rank_from_state', r'  std::memcpy\(out \+ seat_bind::kRankOff, card_row \+ seat_bind::kRankOff, 2\);(\r?\n)', r'\1'),
    ('row_texts_from_state', r'  std::memcpy\(out \+ seat_bind::kText0, card_row \+ seat_bind::kText0, 6\);(\r?\n)', r'\1'),
    ('row_twitter_text_from_state', r'  std::memcpy\(out \+ kTwitterTextOff, card_row \+ kTwitterTextOff, 4\);(\r?\n)', r'\1'),
    ('row_pack_code_from_state', r'  out\[kPackCodeOff\] = card_row\[kPackCodeOff\];(\r?\n)', r'\1'),
    ('row_players_bytes_from_card', r'std::memcpy\(out, state_row, kRowBytes\);', 'std::memcpy(out, card_row, kRowBytes);'),
    ('profile_not_migrated', r'int n = superstars_migrate::MigrateProfile\(out, card_prof, t, what, \{bank_t\}\);', 'int n = 0;'),
    ('profile_state_bank_not_old', r'MigrateProfile\(out, card_prof, t, what, \{bank_t\}\)', 'MigrateProfile(out, card_prof, t, what, {})'),
    ('profile_alt_key_not_moved', r'if \(superstars_migrate::Rd\(out \+ off\) == uint16_t\(t\) && t != s\) \{', 'if (false) {'),
    ('profile_alt_key_any_id', r'if \(superstars_migrate::Rd\(out \+ off\) == uint16_t\(t\) && t != s\) \{', 'if (t != s) {'),
    ('profile_players_bytes_from_card', r'std::memcpy\(out, state_prof, kProBytes\);', 'std::memcpy(out, card_prof, kProBytes);'),
    # (S110 UNISTRIP) the strip decision of a card slot
    ('strip_played_match_changed', r'if \(status == kStatusResult\) return false;', ';', STRIP_NAME),
    ('strip_auto_match_stripped', r'if \(!left_by_builder\) return false;', ';', STRIP_NAME),
    ('strip_ordinal_ignored', r'return special \|\| auto_ordinal >= kBuilderMatches;', 'return special;', STRIP_NAME),
    ('strip_ordinal_off_by_one', r'return special \|\| auto_ordinal >= kBuilderMatches;', 'return special || auto_ordinal > kBuilderMatches;', STRIP_NAME),
    ('strip_builder_n_ten', r'constexpr int kBuilderMatches = 7;', 'constexpr int kBuilderMatches = 10;', STRIP_NAME),
    ('strip_not_replaced_keeps_bit', r'return id_now == id_before \? before_strip : now;', 'return now;', STRIP_NAME),
    ('strip_replaced_restored', r'return id_now == id_before \? before_strip : now;', 'return before_strip;', STRIP_NAME),
    ('strip_bare_offset_wrong', r'return 16u \+ 2u \* uint32_t\(k\);', 'return 16u + 12u * uint32_t(k);', STRIP_NAME),
    ('strip_slot_caps_swapped', r'return rumble_type \? kRumbleSlots : bare_kind \? kBareKindSlots : kMaxSlots;', 'return rumble_type ? kBareKindSlots : bare_kind ? kRumbleSlots : kMaxSlots;', STRIP_NAME),
    ('strip_bare_id_kind_stripped', r'if \(bare_id_kind\) return false;', ';', STRIP_NAME),
    ('strip_slot_cap_off', r'slot >= kMaxSlots \|\| ', '', STRIP_NAME),
    ('strip_count_ignored', r' \|\| slot >= count\)', ')', STRIP_NAME),
    ('strip_any_id', r'return gone\.count\(int\(id\)\) != 0;', 'return true;', STRIP_NAME),
    ('strip_id_offset_wrong', r'return 16u \+ 12u \* uint32_t\(k\);', 'return 16u + 16u * uint32_t(k);', STRIP_NAME),
    ('strip_flag_byte_wrong', r'return 24u \+ 12u \* uint32_t\(k\);', 'return 8u + 12u * uint32_t(k);', STRIP_NAME),
    # (S110 UNIDIAG) the card state of the summary line and the slot verdict text
    ('card_no_card_is_empty', r'if \(reached == 0\) return CardState::NoCard;', 'if (reached == 0) return CardState::Empty;', STRIP_NAME),
    ('card_empty_not_told', r'if \(booked == 0\) return CardState::Empty;', ';', STRIP_NAME),
    ('card_none_named_not_told', r'if \(named == 0\) return CardState::NoneNamed;', ';', STRIP_NAME),
    ('verdict_played_missing', r'if \(status == kStatusResult\) return "played \(kept\)";', ';', STRIP_NAME),
    ('verdict_bare_missing', r'if \(bare\) return "bare-id kind \(kept, no game replacer\)";', ';', STRIP_NAME),
    ('verdict_builder_missing', r'if \(!left_by_builder\) return "left to the game\'s builder";', ';', STRIP_NAME),
    ('verdict_strip_wrong', r'return strip \? "strip" : "kept";', 'return "kept";', STRIP_NAME),
    # (S110 UNIAVAIL) the builder's hold: keyed by seat and day, read only inside a job's own thread-local frame, written into the record
    ('hold_never_ends', r'if \(h\.day != day\) h\.seats\.clear\(\);', ';', REBUILD_NAME),
    ('scope_ends_before_job_read', r'if \(h\.day != day\) h\.seats\.clear\(\);', 'h.seats.clear();', REBUILD_NAME),
    ('scope_not_keyed_by_seat', r'return t_job != nullptr && t_job->count\(int\(id\)\) != 0;', 'return t_job != nullptr;', REBUILD_NAME),
    ('answer_not_thread_scoped', r'inline thread_local const std::set<int>\* t_job = nullptr;', 'inline const std::set<int>* t_job = nullptr;', REBUILD_NAME),
    ('frame_outlives_job', r'~JobFrame\(\) \{ t_job = nullptr; \}', '~JobFrame() { static std::set<int> keep; keep = seats_; t_job = &keep; }', REBUILD_NAME),
    ('hold_leaks_across_loads', r'return have_entry \? entry_held : Held\{\};', 'return have_entry ? entry_held : before;', REBUILD_NAME),
    ('new_day_check_merges', r'h\.seats\.empty\(\) \|\| h\.day != day', 'h.seats.empty()', REBUILD_NAME),
    ('held_not_written', r'if \(!e\.held\.seats\.empty\(\)\) t \+= universe_rebuild::HeldLine\(e\.held\) \+ "\\n";', ';'),
]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(header_text, tmp, tag, other=None):
    """header_text = the text of src/slot_handover_core.h (planted or shipped); other = (file name, text) of ANOTHER header's planted copy: it wins over src (-I inc first)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, NAME), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    if other is not None:
        with open(os.path.join(inc, other[0]), 'w', encoding='utf-8', newline='') as f:
            f.write(other[1])
    exe = os.path.join(inc, 'handover_unit.exe')
    # -I inc first (the planted copy wins), then src for seat_card.h / seats_core.h / superstars_migrate.h (the seat-swap prototype's seats_proto_core.h is gone: its helpers live in seat_bind_core.h)
    r = subprocess.run([find_clang(), '-std=c++20', '-Wall', '-I', inc, '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'handover_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('handover_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
    return exe, (r.stdout + r.stderr)


def run_test(exe, tag):
    work = os.path.join(REPO, 'out', 'handover_unit', tag)
    try:
        r = subprocess.run([exe, work], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    text = open(HEADER, encoding='utf-8', newline='').read()
    problems = []
    tmp = tempfile.mkdtemp(prefix='handover_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order. An input is (slot_handover_core.h text, None or (file name, text) of ANOTHER
        # header's planted copy): a plant's optional 4th item names the file it edits (S110 UNIAVAIL: HEADERS).
        texts = {fname: open(path, encoding='utf-8', newline='').read() for fname, path in HEADERS.items()}
        variants = []
        for plant in PLANTS:
            name, pat, rep = plant[:3]
            fname = plant[3] if len(plant) > 3 else NAME  # the file this plant edits
            new, n = re.subn(pat, rep, texts[fname], count=1)
            variants.append((name, n, (new, None) if fname == NAME else (text, (fname, new))))
        once = pr.Once(pr.make_spec(os.path.join(HERE, 'handover_unit.cpp'), dict(HEADERS), ['-std=c++20', '-Wall'], [os.path.join(REPO, 'src')],
                                    run_args=lambda tag: [os.path.join(REPO, 'out', 'handover_unit', tag)]),
                       pr.fail_lines('handover_unit'), plants=[pr.Plant(pl[0], pl[3] if len(pl) > 3 else NAME, pl[1], pl[2]) for pl in PLANTS])
        sh, outs = pr.run_variants((text, None), variants, lambda t, tag: build(t[0], tmp, tag, t[1]), lambda art, tag: run_test(art[0], tag),
                                   pr.parse_jobs(sys.argv[1:]), once)
        if sh.error:
            raise RuntimeError(sh.error)
        warn, rc, out = sh.art[1], sh.rc, sh.out
        if 'warning' in warn:
            problems.append('the unit compiles with warnings: ' + warn[-300:])
        sys.stdout.write(out)
        if rc != 0 or 'PASS handover_unit' not in out:
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
            elif not failed or 'FAIL handover_unit' not in pout:
                # (review) a crash (or a run cut short) is not a caught fault: the selftest cannot tell "the rule failed" from "the unit is broken".
                # Every plant must fail by a printed FAIL line and the unit's own summary; none is expected to crash.
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (name, prc))
            print('plant %-36s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(os.path.join(REPO, 'out', 'handover_unit'), ignore_errors=True)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('handover_selftest: %s (%d plants)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
