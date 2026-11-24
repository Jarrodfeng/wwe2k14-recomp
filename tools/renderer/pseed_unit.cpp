// S106 PSEED unit: the pipeline seed's loader half (src\renderer\pipeline_seed.h: R-line parse, merge into the player's list, what the save keeps). 0 game runs, no GPU, no SDK.
// (MergeSeed is entered directly as renderer.cpp StoreInit calls it: a pure function over the list, its records and the key index; the wiring lint in pseed_unit.py checks StoreInit makes that call)
// entry: rdr::pstore::ParseSeedLines, rdr::pstore::MergeSeed, rdr::pstore::SaveKeeps, rdr::pstore::RecordFromHex, rdr::pstore::HexOfRecord
//   - roundtrip: a record written as an R line reads back with the same RecordKey, its name kept, seed set, seen 0.
//   - refusals: a wrong length, a non-hex digit and a record that fails its range checks are refused BY LINE (counted, the first named), never loaded; other line types are ignored.
//   - multiview: a multiview class is dropped on a device without it, kept on one with it.
//   - merge: a key the player's list holds is not added twice (the player's record, its seen and its name win); duplicates inside the seed collapse; added records are pending and marked seed.
//   - save: a seed record nobody drew (seen 0) is not written into the player's list; a drawn one is; a dead one never is.
//   - depth token (S106 PSEED fix round B1): the depth slot of a seed record is kDepthToken and becomes the DEVICE's depth format (129 on a D24 device, 130 on a D32S8 one); a literal depth format
//     in the slot is refused; a record whose colour format the device cannot render to is dropped (counted), never built.
//   - header (F1): R lines with no H rdr line, a wrong version, a wrong count or a wrong checksum (one flipped in-range digit) refuse the WHOLE seed.
//   - the committed db\shader_list.txt (when it has R lines): every R line parses (`--list FILE`).
// Usage: pseed_unit [plant N] [--list FILE]   (plants, each the UNSAFE direction of one verdict: 1 range checks skipped, 2 merge ignores known keys, 3 save keeps unseen seed records,
//   4 multiview kept on a device without it, 5 names stripped, 6 the depth token not swapped, 7 the H rdr header not checked, 8 colour formats not asked of the device)
// Exit 0 = every check passed, 1 = a check failed (its name is on the FAIL line).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer/pipeline_seed.h"

namespace ps = rdr::pstore;

static int g_fail = 0;
static void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++g_fail;
}

static ps::Record Make(uint64_t vh, uint64_t fh, const char* name, uint8_t pad = 0, uint8_t cull = 1) {
  ps::Record r;
  r.vhash = vh, r.fhash = fh;
  r.state.cull = cull;
  r.pass.color_count = 1, r.pass.has_depth = 1, r.pass.samples = 1, r.pass.pad = pad;
  r.pass.formats[0] = 37;                                  // a colour format (VK_FORMAT_R8G8B8A8_UNORM)
  r.pass.formats[rdr::kMaxColorTargets] = ps::kDepthToken;  // what the merger writes: never a literal depth format
  std::strncpy(r.name, name, rdr::pstore::kNameBytes - 1);
  return r;
}

static std::string Line(const ps::Record& r) { return "R " + ps::HexOfRecord(r) + "\n"; }
// "H rdr <version> <count> <checksum>" for these records, as the merger writes it (`version` / `count` / `flip` let a test break one number).
static std::string Header(const std::vector<ps::Record>& recs, unsigned version = ps::kListVersion, int count_delta = 0, bool flip_crc = false) {
  std::vector<std::string> hx;
  for (const ps::Record& r : recs) hx.push_back(ps::HexOfRecord(r));
  char buf[96];
  std::snprintf(buf, sizeof buf, "H rdr %u %u %016llx\n", version, unsigned(int(recs.size()) + count_delta), (unsigned long long)(ps::SeedChecksum(hx) ^ (flip_crc ? 1 : 0)));
  return buf;
}
static const uint16_t kD32S8 = 130, kD24S8 = 129;
// What the loader makes of a seed record on the D32S8 device (the token swapped), for key comparisons.
static ps::Record Dev(ps::Record r) {
  r.pass.formats[rdr::kMaxColorTargets] = kD32S8;
  return r;
}

int main(int argc, char** argv) {
  int plant = 0;
  std::string list;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "plant" && i + 1 < argc) plant = std::atoi(argv[++i]);
    else if (std::string(argv[i]) == "--list" && i + 1 < argc) list = argv[++i];
  }
  // The shims below are the planted twins: each flips ONE verdict into its unsafe direction so the check that guards it must fail.
  const auto parse = [&](const std::string& t, std::vector<ps::Record>& out, bool mv) {
    if (plant == 4) mv = true;
    const std::function<bool(uint16_t)> ok = plant == 8 ? std::function<bool(uint16_t)>() : std::function<bool(uint16_t)>([](uint16_t f) { return f != 999; });
    ps::SeedParse sp = ps::ParseSeedLines(t, out, mv, kD32S8, ok);
    if (plant == 6) for (ps::Record& r : out) r.pass.formats[rdr::kMaxColorTargets] = ps::kDepthToken;   // the twin: the token is never swapped
    return sp;
  };

  const ps::Record a = Make(0x1111, 0x2222, "yBG_Test_base"), b = Make(0x3333, 0x4444, "yProp_Test_base", 0, 2), c = Make(0x5555, 0, "rdr_ui", 0, 0);
  {   // roundtrip + other line types ignored + CRLF
    std::string text = "S 1 2 3\n" + Header({a, b, c}) + Line(a) + "P fsi 00\n";
    text += "R " + ps::HexOfRecord(b) + "\r\n";
    text += Line(c) + "N fbo 00\nV 5\n";
    std::vector<ps::Record> out;
    const ps::SeedParse sp = parse(text, out, false);
    Check(sp.lines == 3 && sp.refused == 0 && out.size() == 3, "parse.three_lines_read_other_types_ignored");
    Check(out.size() == 3 && ps::RecordKey(out[0]) == ps::RecordKey(Dev(a)) && ps::RecordKey(out[1]) == ps::RecordKey(Dev(b)), "parse.roundtrip_same_record_key");
    Check(out.size() == 3 && out[0].seed && out[0].seen == 0 && !out[0].built && !out[0].dead, "parse.seed_flag_set_seen_zero");
    if (plant == 5 && out.size() == 3) std::memset(out[1].name, 0, rdr::pstore::kNameBytes);
    Check(out.size() == 3 && std::strcmp(out[1].name, "yProp_Test_base") == 0, "parse.names_kept");
    Check(out.size() == 3 && out[0].pass.formats[rdr::kMaxColorTargets] == kD32S8 && out[0].pass.formats[rdr::kMaxColorTargets] != ps::kDepthToken, "depth.token_becomes_the_devices_depth_format");
  }
  {   // refusals
    std::string text = "R 00\n";                                            // wrong length
    std::string bad = ps::HexOfRecord(a);
    bad[10] = 'z';
    text += "R " + bad + "\n";                                               // not hex
    ps::Record weird = a;
    weird.pass.samples = 3;                                                  // not a power of two: the range checks
    text += "R " + ps::HexOfRecord(weird) + "\n";
    text += Line(b);
    std::string hx;
    {   // a header that matches these four lines exactly (the refusal is by line, not by the header)
      std::vector<std::string> v;
      std::istringstream is(text);
      std::string ln;
      while (std::getline(is, ln)) v.push_back(ln.substr(2));
      char buf[96];
      std::snprintf(buf, sizeof buf, "H rdr %u %u %016llx\n", ps::kListVersion, unsigned(v.size()), (unsigned long long)ps::SeedChecksum(v));
      hx = buf;
    }
    text = hx + text;
    std::vector<ps::Record> out;
    const ps::SeedParse sp = parse(text, out, false);
    if (plant == 1) {   // the twin: a loader without the range checks takes the weird record
      out.clear();
      ps::Record r = weird;
      r.seed = true;
      out.push_back(r), out.push_back(b);
    }
    Check(out.size() == 1 && ps::RecordKey(out[0]) == ps::RecordKey(Dev(b)), "parse.bad_lines_never_loaded");
    Check(sp.refused == 3 && sp.lines == 4 && sp.first_refusal.find("wrong length") != std::string::npos, "parse.refusals_counted_and_the_first_named");
  }
  {   // multiview
    const ps::Record mv = Make(0x7777, 0x8888, "yBG_mv", 2);
    std::vector<ps::Record> out;
    const std::string mvtext = Header({mv, a}) + Line(mv) + Line(a);
    const ps::SeedParse sp = ps::ParseSeedLines(mvtext, out, false, kD32S8);
    std::vector<ps::Record> out2;
    ps::ParseSeedLines(mvtext, out2, true, kD32S8);
    if (plant == 4) { out.clear(); ps::ParseSeedLines(mvtext, out, true, kD32S8); }
    Check(out.size() == 1 && sp.multiview == (plant == 4 ? 0u : 1u) && out2.size() == 2 && !out.empty() && out[0].pass.pad == 0, "parse.multiview_dropped_without_the_device_feature");
  }
  {   // merge
    std::vector<ps::Record> records;
    std::unordered_map<uint64_t, uint32_t> index;
    std::vector<uint32_t> pending;
    ps::Record mine = a;                         // the player's own record: seen 9, its own name
    mine.seen = 9;
    mine.pass.formats[rdr::kMaxColorTargets] = kD32S8;   // the player's own record holds the device's real depth format; the seed's token must meet it
    std::strncpy(mine.name, "mine", 5);
    records.push_back(mine);
    index.emplace(ps::RecordKey(mine), 0u);
    pending.push_back(0);
    std::vector<ps::Record> seed;
    std::vector<ps::Record> parsed;
    ps::ParseSeedLines(Header({a, b, b, c}) + Line(a) + Line(b) + Line(b) + Line(c), parsed, false, kD32S8);   // a is in the list, b twice, c once
    seed = parsed;
    ps::SeedMerge m = ps::MergeSeed(records, index, pending, seed);
    if (plant == 2) {   // the twin: no known-key test
      records.clear(), index.clear(), pending.clear();
      records.push_back(mine), index.emplace(ps::RecordKey(mine), 0u), pending.push_back(0);
      for (const ps::Record& r : seed) pending.push_back(uint32_t(records.size())), records.push_back(r);
      m = ps::SeedMerge{uint32_t(seed.size()), 0};
    }
    Check(m.added == 2 && m.known == 2 && records.size() == 3 && pending.size() == 3, "merge.known_keys_and_seed_duplicates_not_added_twice");
    Check(records[0].seen == 9 && std::strcmp(records[0].name, "mine") == 0 && !records[0].seed, "merge.the_players_record_wins");
    Check(records.size() == 3 && records[1].seed && records[2].seed && pending[1] == 1 && pending[2] == 2, "merge.added_records_pending_and_marked_seed");
    // save
    ps::Record drawn = records.size() == 3 ? records[1] : a;
    drawn.seen = 4;
    ps::Record dead = drawn;
    dead.dead = true;
    const bool unseen_kept = plant == 3 ? true : ps::SaveKeeps(records[1]);
    Check(!unseen_kept, "save.an_undrawn_seed_record_is_not_saved");
    Check(ps::SaveKeeps(drawn), "save.a_drawn_seed_record_is_saved");
    Check(!ps::SaveKeeps(dead) && ps::SaveKeeps(mine), "save.dead_never_the_players_own_always");
  }
  {   // B1: the depth token meets each device's own depth format; a literal one is refused; a colour format the device cannot render to drops the record
    const std::string t = Header({a, b}) + Line(a) + Line(b);
    std::vector<ps::Record> d24, d32;
    ps::ParseSeedLines(t, d24, false, kD24S8);
    ps::ParseSeedLines(t, d32, false, kD32S8);
    Check(d24.size() == 2 && d24[0].pass.formats[rdr::kMaxColorTargets] == kD24S8 && d32.size() == 2 && d32[0].pass.formats[rdr::kMaxColorTargets] == kD32S8, "depth.token_is_the_device_format_on_a_D24_device_and_a_D32S8_one");
    ps::Record lit = a;
    lit.pass.formats[rdr::kMaxColorTargets] = kD32S8;   // the builder's old seed: the PC's literal depth format
    std::vector<ps::Record> o;
    const ps::SeedParse sl = ps::ParseSeedLines(Header({lit}) + Line(lit), o, false, kD24S8);
    Check(o.empty() && sl.refused == 1 && sl.first_refusal.find("token") != std::string::npos, "depth.a_literal_depth_format_in_the_seed_is_refused");
    std::vector<ps::Record> n8;
    const ps::SeedParse su = ps::ParseSeedLines(t, n8, false, kD32S8, [](uint16_t f) { return f != 37; });
    uint32_t unsupported = su.unsupported;
    if (plant == 8) { n8.clear(); ps::ParseSeedLines(t, n8, false, kD32S8); unsupported = 0; }   // the twin: the device is never asked
    Check(n8.empty() && unsupported == 2, "depth.a_colour_format_the_device_cannot_render_to_drops_the_record");
    // S106 PSEEDF2 N9: a device that reports depth format 0 (none found) must not get records whose depth slot would be 0 with has_depth set: the whole seed is refused
    std::vector<ps::Record> z;
    ps::SeedParse sz = ps::ParseSeedLines(t, z, false, 0);
    if (plant == 9) { z.clear(); sz = ps::ParseSeedLines(t, z, false, kD32S8); }   // the twin: a device depth format of 0 is taken at face value
    Check(z.empty() && sz.header_refusal.find("depth") != std::string::npos, "depth.a_device_depth_format_of_0_refuses_the_seed");
  }
  {   // F1: the H rdr line guards the whole seed
    std::vector<ps::Record> o;
    ps::ParseSeedLines("H rdr 99 2 0\n" + Line(a) + Line(b), o, false, kD32S8);
    bool ok = o.empty();
    const ps::SeedParse s1 = ps::ParseSeedLines(Header({a, b}, 99) + Line(a) + Line(b), o, false, kD32S8);
    if (plant == 7) { o.push_back(Dev(a)); o.push_back(Dev(b)); }   // the twin: a loader that never reads the header loads the version-99 seed
    ok = ok && o.empty() && !s1.header_refusal.empty() && s1.header_refusal.find("version") != std::string::npos;
    Check(ok, "header.a_wrong_version_refuses_the_whole_seed");
    o.clear();
    const ps::SeedParse s2 = ps::ParseSeedLines(Header({a, b}, ps::kListVersion, 1) + Line(a) + Line(b), o, false, kD32S8);
    Check(o.empty() && s2.header_refusal.find("count") != std::string::npos, "header.a_wrong_count_refuses_the_whole_seed");
    o.clear();
    std::string text = Header({a, b}) + Line(a) + Line(b);
    const size_t at = text.rfind("R ") + 2 + 2 * 56 + 3;   // a digit inside the vertex hash of the last record: any value is in range, only the checksum can tell
    text[at] = text[at] == '1' ? '2' : '1';
    const ps::SeedParse s3 = ps::ParseSeedLines(text, o, false, kD32S8);
    Check(o.empty() && s3.header_refusal.find("checksum") != std::string::npos, "header.an_in_range_flipped_digit_fails_the_checksum");
    o.clear();
    const ps::SeedParse s4 = ps::ParseSeedLines(Line(a), o, false, kD32S8);
    Check(o.empty() && !s4.header_refusal.empty(), "header.R_lines_with_no_H_rdr_line_are_refused");
    o.clear();
    const ps::SeedParse s5 = ps::ParseSeedLines(Header({a, b}, ps::kListVersion, 0, true) + Line(a) + Line(b), o, false, kD32S8);
    Check(o.empty() && !s5.header_refusal.empty(), "header.a_wrong_checksum_is_refused");
    o.clear();
    const ps::SeedParse s6 = ps::ParseSeedLines("S 1 2 3\n", o, false, kD32S8);
    Check(o.empty() && s6.lines == 0 && s6.header_refusal.empty(), "header.no_seed_at_all_is_fine");
  }
  if (!list.empty()) {   // the committed list
    std::ifstream f(list, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<ps::Record> out;
    const ps::SeedParse sp = ps::ParseSeedLines(ss.str(), out, true, kD32S8);
    std::printf("list %s: %u R lines, %zu loaded, %u refused\n", list.c_str(), sp.lines, out.size(), sp.refused);
    Check(f.good() || f.eof(), "list.readable");
    Check(sp.refused == 0 && out.size() == sp.lines, "list.every_R_line_parses");
  }
  std::printf("%s\n", g_fail ? "pseed_unit: FAILED" : "pseed_unit: every check passed");
  return g_fail ? 1 : 0;
}
