"""S110 SEAT3 (lane G-A): builds + runs the host unit of src/seat_bind_core.h (tools/seats/bind_unit.cpp) and its planted twins. No game runs.

    py -I tools/seats/bind_selftest.py   compiles bind_unit.cpp with clang++ against src/seat_bind_core.h and runs it (every check must PASS), then
                                         against COPIES of the header that each carry one planted fault (a rule removed, a refusal removed, an order
                                         swapped, ...) and requires the unit to FAIL on each, by a printed FAIL line and the unit's own summary (a crash
                                         with no FAIL line is a PROBLEM, not a caught fault). The unit writes nothing.

Exit 0 = the unit passes on the shipped header and fails on every planted twin. The plant patterns use \\r?\\n (a checkout may have CRLF files).
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
HEADER = os.path.join(REPO, 'src', 'seat_bind_core.h')
NAME = 'seat_bind_core.h'

PLANTS = [
    # ---- the moved helpers
    ('keys_scan_whole_row', r'for \(uint32_t o = kKeyBeg; o < kKeyEnd; o \+= 2\)', 'for (uint32_t o = 0; o < kRowSize; o += 2)'),
    ('keys_range_one_too_wide', r'v <= hi\)', 'v <= hi + 1)'),
    ('same_id_not_refused', r'  if \(to == from\) return "refused: TO and FROM are the same slot " \+ std::to_string\(to\);(\r?\n)', r'\1'),
    ('inactive_to_not_refused', r'!to_row \|\| to_row\[kActiveOff\] == 0', '!to_row'),
    ('reserved_slot_allowed', r'id != kReservedSeat', 'true'),
    ('name_rewrite_ignores_the_id', r'if \(std::memcmp\(n \+ at, f\.data\(\), 3\) != 0\) return false;', ''),
    ('node_span_without_terminator', r'return size_t\(len\) \+ kTermBytes;', 'return size_t(len);'),
    ('copy_dirs_drops_terminator', r'  out\.insert\(out\.end\(\), term, term \+ 4\);(\r?\n)  return true;', r'\1  return true;'),
    # ---- RetargetCard: the words the relocation list names, by kind (S110 SEAT3-R: the list, not a scan for values)
    ('card_seat_kind_not_moved', r'case sc::RelKind::Seat16: was = uint32_t\(t\), to = uint32_t\(s\); break;', 'case sc::RelKind::Seat16: was = uint32_t(t), to = uint32_t(t); break;'),
    ('card_key_low_digits_lost', r'to = uint32_t\(s\) \* 100u \+ \(v - lo\);', 'to = uint32_t(s) * 100u;'),
    ('card_key_any_id', r'if \(v >= lo && v <= lo \+ 99u\) to = ', 'if (true) to = '),
    ('card_text_family_ignored', r'was = 60000u \+ 2000u \* e\.arg \+ uint32_t\(t\), to = 60000u \+ 2000u \* e\.arg \+ uint32_t\(s\);', 'was = 60000u + uint32_t(t), to = 60000u + uint32_t(s);'),
    ('card_text_not_moved', r'to = 60000u \+ 2000u \* e\.arg \+ uint32_t\(s\);', 'to = 60000u + 2000u * e.arg + uint32_t(t);'),
    ('card_bank_not_moved', r'case sc::RelKind::Bank16: was = uint32_t\(bank_t\), to = uint32_t\(bank_s\); break;', 'case sc::RelKind::Bank16: was = uint32_t(bank_t), to = uint32_t(bank_t); break;'),
    ('card_part_bank_k_ignored', r'was = PartBank\(e\.arg, bank_t\), to = PartBank\(e\.arg, bank_s\); break;', 'was = PartBank(0, bank_t), to = PartBank(0, bank_s); break;'),
    ('card_part_bank_not_moved', r'to = PartBank\(e\.arg, bank_s\); break;', 'to = PartBank(e.arg, bank_t); break;'),
    ('card_unlock_not_moved', r'was = 1100u \+ uint32_t\(t\), to = 1100u \+ uint32_t\(s\); break;', 'was = 1100u + uint32_t(t), to = 1100u + uint32_t(t); break;'),
    ('card_pack_code_not_set', r'case sc::RelKind::Pack8: to = uint32_t\(pack_id\); break;', 'case sc::RelKind::Pack8: break;'),
    ('card_rank_carried_from_the_card', r'case sc::RelKind::Rank16: to = uint32_t\(rank_s\); break;', 'case sc::RelKind::Rank16: break;'),
    ('card_rank_range_unchecked', r'  if \(rank_s < 0 \|\| rank_s > 65535\) return [^\r\n]*(\r?\n)', r'\1'),
    ('card_disagreement_accepted', r'    if \(v != was\) return err = [^\r\n]*(\r?\n)', r'\1'),
    ('card_overflow_16_unchecked', r'    if \(w == 2 && to > 0xFFFFu\) return [^\r\n]*(\r?\n)', r'\1'),
    ('card_no_list_accepted', r'  if \(!in\.has_relo\) return [^\r\n]*(\r?\n)', r'\1'),
    ('card_past_block_unchecked', r'\|\| size_t\(e\.off\) \+ w > size\) return', ') return'),
    ('card_binp_leaf_ignored', r'if \(b\.k == r\.sub\) return n = b\.row\.size\(\)', 'if (true) return n = b.row.size()'),
    ('card_lsd_list_ignored', r'if \(l\.list == r\.sub\) return n = l\.row\.size\(\)', 'if (true) return n = l.row.size()'),
    ('card_unknown_kind_accepted', r'      default: return err = at \+ "unknown kind", false;', '      default: break;'),
    # the fault of the review: a scan for values in the seat's key range moves a stock word that only looks like a key
    ('card_scan_moves_coincidences', r'  o\.seat = s;', '  for (size_t off = 120; off < 204; off += 2) { const uint32_t v = Le16(o.dat.data() + off); if (v / 100 == uint32_t(t)) PutLe16(o.dat.data() + off, uint16_t(uint32_t(s) * 100u + v % 100)); }\n  o.seat = s;'),
    ('card_scan_moves_dsbd_coincidences', r'  o\.seat = s;', '  for (size_t off = 0; off < 80; off += 2) { const uint32_t v = Le16(o.dsbd.data() + off); if (v / 100 == uint32_t(t)) PutLe16(o.dsbd.data() + off, uint16_t(uint32_t(s) * 100u + v % 100)); }\n  o.seat = s;'),
    ('card_rows_seat_not_set', r'  o\.seat = s;(\r?\n)', r'\1'),
    ('card_wrong_seat_accepted', r'  if \(in\.seat != t\) return [^\r\n]*(\r?\n)', r'\1'),
    ('card_bank_range_unchecked', r'if \(bank_t < 1 \|\| bank_t > 65535 \|\| bank_s < 1 \|\| bank_s > 65535\) return', 'if (false) return'),
    ('card_seat_range_unchecked', r'  if \(!ValidSeat\(t\) \|\| !ValidSeat\(s\)\) return err = "re-target: seat', '  if (false) return err = "re-target: seat'),
    ('card_pack_range_unchecked', r'  if \(pack_id < 0 \|\| pack_id > 255\) return [^\r\n]*(\r?\n)', r'\1'),
    ('card_t102_rewritten', r'  o\.seat = s;', '  o.t102_src_bank = uint32_t(bank_s), o.seat = s;'),
    # ---- the node rows
    ('dir_emd_not_renamed', r'if \(tag == "EMD " && kind == 1\) return RetargetName\(nm, 8, t, s\);', 'if (tag == "EMD " && kind == 1) return false;'),
    ('dir_render_not_renamed', r'kind == 0\) return RetargetName\(nm, 4, t, s\);', 'kind == 0) return false;'),
    ('dir_evp_kept_when_moved', r'\|\| \(evp && keep_evp\)\)', '|| evp)'),
    # (S110 LIBSIZE) the file holds only what we own: another id's EMD / render row and any other directory are not registered
    ('dir_second_id_kept', r'if \(!RetargetName\(probe, int\(rs - 8\), t, t\)\) continue;', ''),
    ('dir_foreign_tag_kept', r'!\(emd \|\| render \|\| \(evp && keep_evp\)\)', 'false'),
    ('dir_evp_kept_flag_ignored', r'if \(keep_evp && t != s\) return err = [^\r\n]*(\r?\n)', r'\1'),
    ('dir_evz_bank_not_moved', r'if \(n == bank_t\) return bank_s;', ''),
    ('dir_part_bank_not_moved', r'if \(n == int\(PartBank\(k, bank_t\)\)\) return int\(PartBank\(k, bank_s\)\);', ''),
    ('dir_victory_not_moved', r'if \(n == 6000 \+ t\) return 6000 \+ s;', ''),
    ('dir_fx_bank_not_moved', r'std::atoi\(std::string\(nm, 4\)\.c_str\(\)\) == bank_t\) \{', 'false) {'),
    ('dir_plate_not_moved', r'std::atoi\(std::string\(nm \+ 1, 3\)\.c_str\(\)\) == 500 \+ t\) \{', 'false) {'),
    ('dir_terminator_dropped', r'  v\.insert\(v\.end\(\), rows\.begin\(\) \+ ptrdiff_t\(body\), rows\.end\(\)\);[^\r\n]*(\r?\n)', r'\1'),
    ('dir_hex_flag_ignored', r'\|\| \(flags & 0x10000u\)\) \{', ') {'),
    ('dir_unterminated_accepted', r'  if \(!ScanDirs\(rows\.data\(\), rows\.size\(\), body, err\)\) return false;', '  body = rows.size() >= 4 ? rows.size() - 4 : 0;'),
    # a directory that lost rows has its length word made smaller, one left with none goes whole
    ('dir_dropped_rows_leave_length', r'    else PutBe32\(&v\[first \+ 4\], [^\r\n]*', '    else (void)0;'),
    ('dir_empty_dir_kept', r'    if \(kept == 0\) v\.resize\(first\);', '    if (false) v.resize(first);'),
    # ---- the EV blob
    # (S110 SEAT3 review) the importer's blobs of more than 0xFFFF raw bytes are several identity chunks; a pac row pads with < 256 zero bytes
    ('ev_only_first_chunk_written', r'for \(const EvChunk& c : ch\) std::memcpy\(&o\[c\.at\], &dec\[d\], c\.n\), d \+= c\.n;', 'std::memcpy(&o[ch[0].at], &dec[0], ch[0].n), (void)d;'),
    ('ev_expanded_overrun_accepted', r'n == 0 \|\| n > expanded - got', 'n == 0'),
    ('ev_compressed_accepted', r'    if \(b\[at\] != 0xFF \|\| b\[at \+ 1\] != 0x80 \|\| b\[at \+ 2\] != 0xFE\) return [^\r\n]*(\r?\n)', r'\1'),
    ('ev_padding_unbounded', r'  if \(b\.size\(\) - end >= kEvUnit\) return [^\r\n]*(\r?\n)', r'\1'),
    ('ev_padding_nonzero_accepted', r'    if \(b\[i\]\) return "EV blob: a non-zero byte after the last chunk";', '    ;'),
    ('ev_packed_word_unchecked', r'  if \(at != end\) return [^\r\n]*(\r?\n)', r'\1'),
    ('ev_not_bpe_accepted', r'  if \(std::memcmp\(b\.data\(\), "BPE ", 4\) != 0\) return [^\r\n]*(\r?\n)', r'\1'),
    ('ev_not_a_table_accepted', r'std::memcmp\(ev, "0FOP", 4\) != 0\)', 'false)'),
    ('ev_bank_not_moved', r'  PutBe16\(ev \+ kEvBankOff, uint16_t\(to\)\);(\r?\n)', r'\1'),
    ('ev_bank_always_written', r'const int to = MapBankNumber\(Be16\(ev \+ kEvBankOff\), t, s, bank_t, bank_s\);', 'const int to = bank_s;'),
    ('ev_actor_any_character', r'if \(r\[16\] == 1 && int\(x / 100\) == t\)', 'if (r[16] == 1)'),
    ('ev_actor_any_kind', r'if \(r\[16\] == 1 && int\(x / 100\) == t\)', 'if (int(x / 100) == t)'),
    ('ev_actor_low_digits_lost', r'PutBe32\(r \+ 12, uint32_t\(s\) \* 100u \+ x % 100\)', 'PutBe32(r + 12, uint32_t(s) * 100u)'),
    ('ev_actor_partial_record_read', r'kEvRecOff \+ kEvRecSize \* \(size_t\(i\) \+ 1\) <= raw; \+\+i\)', 'kEvRecOff + kEvRecSize * size_t(i) + 17 <= raw; ++i)'),
    ('ev_actor_offset_wrong', r'kEvBlobHead = 21, kEvBankOff = 0x20, kEvCountOff = 0x40, kEvRecOff = 0x44', 'kEvBlobHead = 21, kEvBankOff = 0x20, kEvCountOff = 0x40, kEvRecOff = 0x48'),
    ('ev_bank_offset_wrong', r'kEvBankOff = 0x20,', 'kEvBankOff = 0x22,'),
    # ---- the card's EV refs (lane G-B)
    ('evrefs_any_directory', r'if \(std::memcmp\(&rows\[at\], "EVP ", 4\) == 0 && kind == 1 && !\(flags & 0x10000u\)\) \{', 'if (kind == 1 && !(flags & 0x10000u)) {'),
    ('evrefs_name_not_trimmed', r'while \(!e\.name\.empty\(\) && e\.name\.back\(\) == \' \'\) e\.name\.pop_back\(\);', ''),
    ('evrefs_sector_size_swapped', r'e\.sector = Be32\(r \+ 8\);(\r?\n)        e\.size = Be32\(r \+ 12\);', r'e.sector = Be32(r + 12);\1        e.size = Be32(r + 8);'),
    ('evname_not_moved', r'return std::to_string\(MapBankNumber\(std::atoi\(name\.c_str\(\)\), t, s, bank_t, bank_s\)\);', 'return name;'),
    ('evname_word_changed', r'    if \(c < \'0\' \|\| c > \'9\'\) return name;', '    ;'),
    ('evtrim_not_cut', r'return std::vector<uint8_t>\(raw\.begin\(\), raw\.begin\(\) \+ ptrdiff_t\(len\)\);', 'return raw;'),
    ('evtrim_magic_unchecked', r'raw\.size\(\) < 16 \|\| std::memcmp\(raw\.data\(\), "BPE ", 4\) != 0', 'raw.size() < 16'),
    # ---- the EV file
    ('evfile_blob_not_on_sector', r'sector \+= \(uint64_t\(r\.size\) \* kEvUnit \+ kEvSector - 1\) / kEvSector;', 'sector += uint64_t(r.size) * kEvUnit / kEvSector;'),
    ('evfile_size_floor', r'r\.size = uint32_t\(\(e\.blob\.size\(\) \+ kEvUnit - 1\) / kEvUnit\);', 'r.size = uint32_t(e.blob.size() / kEvUnit);'),
    ('evfile_no_header', r'out\.bytes\.assign\(size_t\(kEvHeader \+ sector \* kEvSector\), 0\);', 'out.bytes.assign(size_t(sector * kEvSector), 0);'),
    ('evfile_duplicate_name_accepted', r'if \(!seen\.insert\(e\.name\)\.second\) return [^\r\n]*(\r?\n)', r'\1'),
    ('evfile_long_name_accepted', r'e\.name\.size\(\) > 8 \|\|', '', ),
    ('evfile_name_not_padded', r'    r\.name\.resize\(8, \' \'\);(\r?\n)', r'\1'),
    ('evfile_node_rows_flags_wrong', r'uint32_t\(f\.rows\.size\(\) \* 16 / 4\) << 20 \| 0x1000u', 'uint32_t(f.rows.size() * 16 / 4) << 20'),
    ('evfile_node_no_terminator', r'  v\.insert\(v\.end\(\), term, term \+ 4\);(\r?\n)  return v;', r'\1  return v;'),
    # ---- the collation, the page, the letters
    ('sort_case_sensitive', r'if \(cp >= \'A\' && cp <= \'Z\'\) return kLetterRank \+ \(cp - \'A\' \+ \'a\'\);', 'if (cp >= \'A\' && cp <= \'Z\') return kLetterRank + cp;'),
    ('sort_tie_not_by_key', r'  return a\.key < b\.key;', '  return false;'),
    ('sort_latin1_not_folded', r'if \(const uint32_t b = Latin1Base\(cp\)\) return kLetterRank \+ b;', ''),
    ('sort_symbols_after_letters', r'  if \(const uint32_t b = Latin1Base\(cp\)\) return kLetterRank \+ b;(\r?\n)  return cp;',
     r'  if (const uint32_t b = Latin1Base(cp)) return kLetterRank + b;\1  return 2 * kLetterRank + cp;'),
    ('latin1_multiplication_sign_a_letter', r'  if \(cp == 0xD7\) return 0;', '  if (cp == 0xD7) return \'x\';'),
    # (S110 SEAT3 review) ONE rule with the letter jump (src/letter_jump_core.h): its reader, its prefix rule, its Initial
    ('codepoints_bytewise', r'v\.push_back\(letter_jump::detail::CodeAt\(s, i, w\)\);', 'v.push_back(uint8_t(s[i])), w = 1;'),
    ('bucket_own_rule', r'inline char BucketOf\(const std::string& name\) \{ return letter_jump::Initial\(name\); \}',
     'inline char BucketOf(const std::string& name) { const std::vector<uint32_t> v = FoldedName(name); return v.empty() || v[0] < kLetterRank ? \'#\' : char(v[0] - kLetterRank - \'a\' + \'A\'); }'),
    ('sortname_leading_space_kept', r'  while \(i < name\.size\(\) && name\[i\] == \' \'\) \+\+i;(\r?\n)  if \(name\.size\(\) - i > 4', r'\1  if (name.size() - i > 4'),
    ('catalog_duplicate_key_listed_twice', r'if \(e\.key\.empty\(\) \|\| !keys\.insert\(e\.key\)\.second\) continue;', 'if (e.key.empty()) continue;'),
    ('catalog_resident_in_library', r'      if \(e\.seat < 0\) c\.library\.push_back\(e\);', '      c.library.push_back(e);'),
    ('bindable_residents_bound', r'if \(!residents\.count\(s\) && !held\.count\(s\)\) v\.push_back\(s\);', 'if (!held.count(s)) v.push_back(s);'),
    ('bindable_held_seat_bound', r'if \(!residents\.count\(s\) && !held\.count\(s\)\) v\.push_back\(s\);', 'if (!residents.count(s)) v.push_back(s);'),
    ('bindable_cap_ignored', r'if \(cap > 0 && int\(v\.size\(\)\) > cap\) v\.resize\(size_t\(cap\)\);', ''),
    ('bindable_not_ascending', r'std::sort\(frame_seats\.begin\(\), frame_seats\.end\(\)\);', ''),
    ('page_count_floor', r'return p == 0 \|\| l == 0 \? 0 : int\(\(l \+ p - 1\) / p\);', 'return p == 0 || l == 0 ? 0 : int(l / p);'),
    ('flip_no_wrap', r'return int\(\(\(\(long long\)from \+ dir\) % n \+ n\) % n\);', 'return std::max(0, std::min(n - 1, from + dir));'),
    ('assign_page_not_modulo', r'if \(pl\.pages > 0\) k = int\(\(\(long long\)k % pl\.pages \+ pl\.pages\) % pl\.pages\);', 'if (pl.pages > 0) k = std::max(0, std::min(k, pl.pages - 1));'),
    ('assign_page_size_off_by_one', r'i < \(size_t\(k\) \+ 1\) \* seats\.size\(\); \+\+i\)', 'i < (size_t(k) + 1) * seats.size() + 1; ++i)'),
    ('assign_unchanged_seat_in_plan', r'      if \(now != tk\) pl\.steps\.push_back\(\{seat, now, tk\}\);', '      pl.steps.push_back({seat, now, tk});'),
    # (S110 SEAT3-R d1 / d2) the page rules: a held seat keeps its man; a man of the page keeps the seat he has; slot first, then the free seats in order
    ('assign_keep_rule_removed', r'const auto it = sits\.find\(page\[i\]\);', 'const auto it = sits.end();'),
    ('assign_kept_man_not_placed', r'      if \(!st\.screen_held\.count\(it->second\)\) target\[it->second\] = page\[i\];', '      ;'),
    ('assign_keep_ignores_other_seats', r'      if \(bindable\.count\(seat\)\) sits\[key\] = seat;', '      if (bindable.count(seat) && seat == seats[0]) sits[key] = seat;'),
    ('assign_slot_first_removed', r'      if \(!taken\.count\(seats\[i\]\)\) taken\.insert\(seats\[i\]\), target\[seats\[i\]\] = page\[i\];', '      if (false) taken.insert(seats[i]), target[seats[i]] = page[i];'),
    ('assign_slot_map_descending', r'      if \(!taken\.count\(seats\[i\]\)\) taken\.insert\(seats\[i\]\), target\[seats\[i\]\] = page\[i\];',
     '      if (!taken.count(seats[seats.size() - 1 - i])) taken.insert(seats[seats.size() - 1 - i]), target[seats[seats.size() - 1 - i]] = page[i];'),
    ('assign_fill_reversed', r'target\[seats\[at\]\] = page\[later\[n\]\];', 'target[seats[at]] = page[later[later.size() - 1 - n]];'),
    ('assign_fill_takes_a_taken_seat', r'while \(at < seats\.size\(\) && taken\.count\(seats\[at\]\)\) \+\+at;', ''),
    ('assign_screen_held_slot_free', r'      if \(st\.screen_held\.count\(seat\)\) taken\.insert\(seat\);', '      ;'),
    ('assign_screen_held_seat_emptied', r'if \(!st\.screen_held\.count\(seat\) && !target\.count\(seat\)\) target\[seat\] = std::string\(\);', 'if (!target.count(seat)) target[seat] = std::string();'),
    ('assign_stale_guest_kept', r'if \(!bindable\.count\(seat\) && !st\.screen_held\.count\(seat\) && !residents\.count\(seat\)\) target\[seat\] = std::string\(\);', ';'),
    ('assign_stale_guest_screen_held_unbound', r'if \(!bindable\.count\(seat\) && !st\.screen_held\.count\(seat\) && !residents\.count\(seat\)\) target\[seat\]', 'if (!bindable.count(seat) && !residents.count(seat)) target[seat]'),
    ('listing_residents_missing', r'      if \(e\.seat >= 0\) m\[e\.seat\] = e\.key;', '      ;'),
    ('listing_plan_not_applied', r'      if \(s\.arrive\.empty\(\)\) m\.erase\(s\.seat\);(\r?\n)      else m\[s\.seat\] = s\.arrive;', r'      ;\1'),
    ('pageof_resident_needs_page', r'if \(e\.seat >= 0\) return r\.seated = true, r\.seat = e\.seat, r;', ';'),
    ('pageof_guest_needs_page', r'      if \(g == key\) return r\.seated = true, r\.seat = seat, r;', '      ;'),
    ('pageof_wrong_page', r'if \(library\[i\]\.key == key\) r\.page = int\(i / p\);', 'if (library[i].key == key) r.page = int(i / p) + 1;'),
    ('pageof_unknown_found', r'    if \(!r\.found\) return r;(\r?\n)', r'    r.found = true;\1'),
    ('letter_first_of_wrong_bucket', r'if \(BucketOf\(e\.name\) == letter\) return &e;', 'if (BucketOf(e.name) >= letter) return &e;'),
    ('letter_lowercase_not_folded', r'  if \(letter >= \'a\' && letter <= \'z\'\) letter = char\(letter - \'a\' \+ \'A\'\);(\r?\n)', r'\1'),
    ('letter_empty_bucket_found', r'return e \? c\.PageOf\(e->key, st\) : PageRef\(\);', 'return e ? c.PageOf(e->key, st) : PageRef{true, false, -1, 0};'),
    ('sortname_the_kept', r'    if \(j < name\.size\(\)\) i = j;', '    (void)j;'),
    # ---- the ranks
    ('rank_not_unique', r'out\[v\[i\]\.second\] = base_rank \+ 1 \+ int\(i\);', 'out[v[i].second] = base_rank + 1;'),
    ('rank_not_above_base', r'out\[v\[i\]\.second\] = base_rank \+ 1 \+ int\(i\);', 'out[v[i].second] = 1 + int(i);'),
    ('rank_by_seat_order', r'return EntryLess\(\*a\.first, \*b\.first\); \}\);', 'return a.second < b.second; });'),
    ('rank_residents_first', r'return EntryLess\(\*a\.first, \*b\.first\); \}\);',
     'return (a.first->seat >= 0) != (b.first->seat >= 0) ? a.first->seat >= 0 : EntryLess(*a.first, *b.first); });'),
    ('rank_guests_not_listed', r'for \(const Seated& s : Listing\(k, st\)\)', 'for (const Seated& s : Listing(0, st))'),
    # ---- the re-ask
    ('reask_other_id_touched', r'if \(seat < 0 \|\| entry_id != uint32_t\(seat\)\) return r;', 'if (seat < 0) return r;'),
    ('reask_pending_not_set', r'  r\.stores\.push_back\(\{kPoolPending, 1\}\);(\r?\n)', r'\1'),
    ('reask_delay_not_cleared', r'  r\.stores\.push_back\(\{kPoolDelay, 0\}\);(\r?\n)', r'\1'),
    ('reask_loading_not_cancelled', r'if \(entry_state == 1\) r\.cancel_first = true, r\.stores\.push_back\(\{kPoolState, 3\}\);', ';'),
    ('reask_cancelling_touched', r'  if \(entry_state == 3\) return r\.again = true, r;(\r?\n)', r'\1'),
    ('reask_state_store_after', r'if \(entry_state == 1\) r\.cancel_first = true, r\.stores\.push_back\(\{kPoolState, 3\}\);', 'if (entry_state == 1) r.cancel_first = true;'),
]


def run_test(exe, tag=None):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    problems = []
    tmp = tempfile.mkdtemp(prefix='bind_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order. seats_core.h (included by the unit) includes seat_bind_core.h by quotes: its copy sits
        # beside the plant, so it sees the planted one. -I src for seat_card.h / seats_core.h / superstars_migrate.h and src\importer's headers.
        defs = ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING']
        sh, outs = pr.run_unit_plants([os.path.join(HERE, 'bind_unit.cpp')], [os.path.join(REPO, 'src'), os.path.join(REPO, 'src', 'launcher')],
                                      ['-std=c++20', '-Wall', '-Wno-unused-command-line-argument'] + defs, HEADER, [os.path.join(REPO, 'src', 'seats_core.h')],
                                      PLANTS, run_test, 'bind_unit.exe', tmp, pr.parse_jobs(sys.argv[1:]), caught=pr.fail_lines('bind_unit'))
        if sh.error:
            raise RuntimeError(sh.error)
        if 'warning' in sh.art[1]:
            problems.append('the unit compiles with warnings: ' + sh.art[1][-400:])
        sys.stdout.write(sh.out)
        if sh.rc != 0 or 'PASS bind_unit' not in sh.out:
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
            elif not failed or 'FAIL bind_unit' not in pout:
                # a crash (or a run cut short) is not a caught fault
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (name, prc))
            print('plant %-40s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:80].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('bind_selftest: %s (%d plants)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
