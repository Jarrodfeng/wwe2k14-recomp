// Session 98: the Superstar Importer core's PC check (src\importer). Built by tools\importer\build_test.ps1; runs on this PC
// with the player's own files and writes ONLY under the --mods folder you give it (keep it under out\: game data).
//
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source ZIP_OR_PAC [--slot 397] [--base 100] [--name "Mr. Kennedy"]
//                 [--short KENNEDY] [--entrance-from N] [--selftest-only] [--render]
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source ZIP_OR_PAC --fit-cases [--stfs-dir DIR]
//                 Session 101: the "why imports fail" cases (FitCases below), each in OUT_DIR\fit_<case>
//
// 1. selftest: the writers against the real packs (arcs rebuilt from the pacs' directories, chEtc / DLC_HD re-packed: byte for
//    byte, like build_mod_pack.py --selftest); deflate / BC3 / string-table round trips.
// 2. InspectSources + ExtractSource on the source, then BuildPack, then reads the pack back (the game's parser order, arcs vs
//    pac directories, the name texts, row SLOT, the model names). Exit code = failures.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/dlc_catalog.h"
#include "../../src/importer/importer_core.h"
#include "../../src/importer/pack_fit.h"
#include "../../src/importer/pack_io.h"
#include "../../src/importer/bc3.h"

namespace fs = std::filesystem;
using importer::io::Bytes;
namespace io = importer::io;

static int g_fail = 0;
static bool Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
  return ok;
}
static Bytes Read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static fs::path Find(const fs::path& root, std::string rel) {
  for (char& c : rel)
    if (c == '\\') c = '/';
  fs::path p = root / rel;
  std::error_code ec;
  if (fs::exists(p, ec)) return p;
  for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
    std::string a = e.path().filename().string(), b = p.filename().string();
    for (char& c : a) c = char(std::tolower((unsigned char)c));
    for (char& c : b) c = char(std::tolower((unsigned char)c));
    if (a == b) return e.path();
  }
  return {};
}

static void SelfTest(const std::string& dlc) {
  std::printf("selftest (writers vs the real packs):\n");
  for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc))) {
    if (pk.status != wwe2k14::dlc::Status::kReady || pk.package) continue;
    const Bytes cat = Read(pk.root / "info" / "catalog.dlc");
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    const std::string why = io::ParseCatalogLikeGame(cat, gate, info);
    Say(why.empty(), "pack " + std::to_string(pk.pack_id) + " catalog parses like sub_8281F150" + (why.empty() ? "" : ": " + why));
    if (!why.empty() || info.files.empty()) continue;
    for (int col = 0; col < 2; ++col) {
      std::map<int, std::string> idx;
      for (const auto& f : info.files) {
        const int k = col ? f.b : f.a;
        if (k >= 0) idx[k] = f.name;
      }
      std::vector<io::Pac> pacs(idx.size());
      std::vector<const io::Pac*> ptr;
      bool ok = true;
      for (auto& [k, n] : idx) {
        std::string e;
        ok = ok && io::ReadPac(Read(Find(pk.root, n)), pacs[size_t(k)], e);
        ptr.push_back(&pacs[size_t(k)]);
      }
      const Bytes arc = Read(pk.root / "info" / (col ? "catalog_sd.arc" : "catalog_hd.arc"));
      Say(ok && io::WriteArc(ptr) == arc, "pack " + std::to_string(pk.pack_id) + (col ? " catalog_sd.arc" : " catalog_hd.arc") + " rebuilt from the pacs: identical");
    }
    for (const auto& f : info.files) {
      std::string l = f.name;
      for (char& c : l) c = char(std::tolower((unsigned char)c));
      if (l.rfind("pac\\ch\\chetc_dlc", 0) != 0 && l.rfind("pac\\dlc_hd_dlc", 0) != 0 && l.rfind("pac\\string_dlc", 0) != 0) continue;
      const Bytes orig = Read(Find(pk.root, f.name));
      io::Pac p;
      std::string e;
      io::ReadPac(orig, p, e);
      for (auto& d : p.dirs)
        for (auto& x : d.ents) {
          std::vector<io::Leaf> leaves;
          if (io::ReadPach(x.blob, leaves)) {
            Bytes w = io::WritePach(leaves);
            const size_t n = x.blob.size();
            if (w.size() <= n) w.resize(n, 0);
            x.blob = w;
          }
        }
      Say(io::WritePac(p) == orig, f.name + " re-packed (every PACH re-written): identical");
    }
  }
  // crafted pacs (review): overlapping entries and an entry past the end are refused, not copied
  {
    io::Pac c;
    c.magic = "EPK8";
    c.dirs.resize(1);
    c.dirs[0].tag = "EMD ", c.dirs[0].kind = 1;
    for (int i = 0; i < 2; ++i) {
      io::Entry e;
      e.name = "0001000" + std::to_string(i * 2);
      e.blob.assign(1 << 20, uint8_t(i));
      c.dirs[0].ents.push_back(e);
    }
    Bytes good = io::WritePac(c);
    io::Pac back;
    std::string e;
    Say(io::ReadPac(good, back, e), "a well-formed test pac reads back");
    Bytes over = good;  // entry 1 re-pointed at sector 0 with a different size: overlaps entry 0
    io::W32(&over[0x800 + 12 + 16 + 8], 0), io::W32(&over[0x800 + 12 + 16 + 12], (2 << 20) / 256);  // (the whole data: more bytes copied than the file has)
    Say(!io::ReadPac(over, back, e), "overlapping entries refused (" + e + ")");
    Bytes past = good;
    io::W32(&past[0x800 + 12 + 16 + 8], 100000);
    Say(!io::ReadPac(past, back, e), "an entry past the end refused (" + e + ")");
    std::vector<importer::SourceInfo> none = importer::InspectSources("C:\\no\\such\\file.pac");
    Say(none.size() == 1 && !none[0].ok, "InspectSources on a missing file: '" + none[0].err + "'");
  }
  // deflate round trip
  Bytes t;
  for (int i = 0; i < 200000; ++i) t.push_back(uint8_t("THE ROCK\0SCOTT HALL\0"[i % 20] + (i % 997 == 0)));
  const Bytes leaf = io::ZlibLeaf(t, 0x1230);
  Say(io::Decode(leaf) == t, "zlib leaf round trip (" + std::to_string(t.size()) + " -> " + std::to_string(leaf.size()) + " bytes)");
  Bytes rnd(70000);
  uint32_t s = 1;
  for (auto& c : rnd) s = s * 1103515245 + 12345, c = uint8_t(s >> 16);
  Say(io::Decode(io::ZlibLeaf(rnd, 0x1230)) == rnd, "zlib leaf round trip (70000 random bytes)");
  Say(io::Decode(io::BpeIdentity(rnd)) == rnd, "identity BPE round trip across Gage blocks (70000 bytes)");
  // BC3: a gradient with soft alpha, decoded back by our own reader
  std::vector<uint8_t> img(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &img[(size_t(y) * 64 + x) * 4];
      p[0] = uint8_t(x * 4), p[1] = uint8_t(y * 4), p[2] = 128, p[3] = uint8_t((x + y) * 2);
    }
  const Bytes dds = importer::bc3::Dds(img, 64, 64);
  double err = 0;
  for (int by = 0; by < 16; ++by)
    for (int bx = 0; bx < 16; ++bx) {
      const uint8_t* blk = &dds[128 + (size_t(by) * 16 + bx) * 16];
      const int a0 = blk[0], a1 = blk[1];
      uint64_t abits = 0;
      for (int k = 0; k < 6; ++k) abits |= uint64_t(blk[2 + k]) << (8 * k);
      const uint16_t c0 = uint16_t(blk[8] | blk[9] << 8), c1 = uint16_t(blk[10] | blk[11] << 8);
      const uint32_t ci = uint32_t(blk[12] | blk[13] << 8 | blk[14] << 16 | uint32_t(blk[15]) << 24);
      float pc[4][3];
      auto u = [](uint16_t v, float* c) { c[0] = float((v >> 11) & 31) * 255 / 31, c[1] = float((v >> 5) & 63) * 255 / 63, c[2] = float(v & 31) * 255 / 31; };
      u(c0, pc[0]), u(c1, pc[1]);
      for (int c = 0; c < 3; ++c) pc[2][c] = (2 * pc[0][c] + pc[1][c]) / 3, pc[3][c] = (pc[0][c] + 2 * pc[1][c]) / 3;
      for (int i = 0; i < 16; ++i) {
        const int ai = int((abits >> (3 * i)) & 7);
        int a = ai == 0 ? a0 : ai == 1 ? a1 : a0 > a1 ? ((8 - ai) * a0 + (ai - 1) * a1) / 7 : 0;
        const uint8_t* p = &img[(size_t(by * 4 + i / 4) * 64 + bx * 4 + i % 4) * 4];
        const float* c = pc[(ci >> (2 * i)) & 3];
        err += std::abs(a - p[3]) + std::abs(c[0] - p[0]) + std::abs(c[1] - p[1]) + std::abs(c[2] - p[2]);
      }
    }
  err /= 64.0 * 64 * 4;
  Say(err < 4.0, "BC3 encode of a 64x64 gradient: mean abs error " + std::to_string(err) + " (< 4)");
}

// ---- Session 101 (lane 199, "why imports fail"): one case per way an imported superstar could miss the game. Each builds its own
// pack under <mods>\fit_<case> from the player's own files (nothing else is written) and checks it with the SAME rule the game
// applies at boot (src\importer\pack_fit.h Problem, through importer::PackProblem). DLC sets are varied with the launcher's own
// switch (dlc_off) and USE MY DLC off (dlc_dir ""), so no pack is copied. --stfs-dir DIR: a folder holding ONE Xbox 360 package
// made from the top pack (tools\dlc\make_test_stfs.py "<dlc>\DLC Pack 4" DIR\pack5 --name "Legends Pack"), for the package case.
static std::string TextOf(const fs::path& p) {
  const Bytes b = Read(p);
  return std::string(b.begin(), b.end());
}
static std::string LineOf(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);)
    if (l.rfind(key + " ", 0) == 0 || l == key) return l;
  return {};
}
static void FitCases(const std::string& game, const std::string& dlc, const std::string& mods_root, const std::string& source,
                     const std::string& stfs_dir) {
  std::printf("fit cases (Session 101, lane 199):\n");
  bool same = true;
  for (int i = 0; i < 5; ++i) same = same && wwe2k14::dlc::kGateWords[i] == io::kGateTable[i];
  Say(same, "the game's gate table is the same in dlc_catalog.h (the game, the launchers) and pack_io.h (the builder)");
  launcher::GameFiles gf;
  gf.SetRoot(fs::u8path(game));
  const importer::SourceInfo one = importer::InspectSource(source);
  std::error_code ec;
  // a fresh mods folder for one case, with n characters (the same model) on `slots` (else the first free ones)
  auto make = [&](const std::string& cs, int n, std::vector<int> slots = {}) {
    const fs::path m = fs::u8path(mods_root) / ("fit_" + cs);
    fs::remove_all(m, ec);
    std::string e;
    const std::u8string mu = m.u8string();
    const std::string pac = importer::ExtractSource(source, one.entry_name, std::string(mu.begin(), mu.end()), &e);
    importer::PackState st;
    const std::vector<int> free = importer::FreeSlots(gf, dlc);
    for (int k = 0; k < n; ++k) {
      importer::Superstar s;
      s.slot = k < int(slots.size()) ? slots[size_t(k)] : free[size_t(k)];
      s.name = "Fit Test " + std::to_string(k + 1), s.short_name = "FIT " + std::to_string(k + 1);
      s.base_id = 100, s.source_pac = pac;
      st.stars.push_back(s);
    }
    return std::make_pair(std::string(mu.begin(), mu.end()), st);
  };
  auto build = [&](const std::string& m, const importer::PackState& st, const std::string& d, const std::string& off) {
    std::string e;
    const bool ok = importer::BuildPack(m, gf, d, st, nullptr, e, off);
    if (!ok)
      for (const auto& l : importer::LastBuildLog()) std::printf("  | %s\n", l.c_str());
    if (!ok) std::printf("  build error: %s\n", e.c_str());
    return ok;
  };
  auto problem = [&](const std::string& m, const std::string& d, const std::string& off) { return importer::PackProblem(m, d, off); };
  auto txt = [&](const std::string& m) { return TextOf(fs::u8path(m) / "superstars" / "superstars.txt"); };

  // A. three characters on Jarrod's whole DLC: the pack holds all three, and the game's rule against other DLC sets
  auto [ma, sa] = make("multi", 3);
  if (Say(build(ma, sa, dlc, ""), "A: BuildPack, 3 characters, all DLC on")) {
    const fs::path out = fs::u8path(ma) / "superstars" / "pack6";
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    gate[0] = 6u << 16 | 1;
    const std::string why = io::ParseCatalogLikeGame(Read(out / "info" / "catalog.dlc"), gate, info);
    std::set<int> chars, ids;
    for (const auto& e : info.entries)
      if (e.type == 1 && e.bit == 0) chars.insert(e.char_id), ids.insert(e.id);
    Say(why.empty() && chars.size() == 3 && ids.size() == 3 && chars.count(sa.stars[0].slot) && chars.count(sa.stars[2].slot),
        "A: catalog: 3 type-1 entries, distinct entry ids, one per slot");
    io::Pac p;
    std::string e2;
    io::ReadPac(Read(out / "pac" / "ch" / "chEtc_dlc06.pac"), p, e2);
    std::vector<io::Leaf> leaves;
    io::ReadPach(p.Find("CHAR", "DAT ")->blob, leaves);
    int rows = 0;
    std::set<int> ranks;
    for (auto& l : leaves)
      for (const auto& s : sa.stars)
        if (int(l.id) == s.slot) {
          const Bytes r = io::Decode(l.raw);
          if (r.size() == 208 && r[4 + 16] == 1 && r[4 + 17] == 6 && io::L16(&r[4 + 6]) == 60000 + s.slot) ++rows, ranks.insert(io::L16(&r[4 + 2]));
        }
    Say(rows == 3 && ranks.size() == 3, "A: chEtc: 3 active rows, pack code 6, own name texts, 3 different ranks");
    Say(std::find_if(sa.stars.begin(), sa.stars.end(), [](auto& s) { return s.slot == 399; }) == sa.stars.end(), "A: slot 399 never given out");
    const std::string top = LineOf(txt(ma), "top");
    Say(top.rfind("top 5 ", 0) == 0 && top.size() == 14, "A: superstars.txt records the top pack and its copy: '" + top + "'");
    Say(problem(ma, dlc, "").empty(), "A: fits all DLC on");
    Say(problem(ma, dlc, "2").empty(), "A: fits with the catalog-only pack 2 switched off");
    Say(problem(ma, dlc, "3").empty(), "A: fits with pack 3 switched off (not built on it; the top is still 5)");
    const std::string p5 = problem(ma, dlc, "5");
    Say(!p5.empty(), "A: pack 5 switched off -> left out: '" + p5 + "'");
    const std::string nd = problem(ma, "", "");
    Say(!nd.empty(), "A: USE MY DLC off -> left out: '" + nd + "'");
    const importer::PlayCheck c0 = importer::CheckBeforePlay(ma, dlc, ""), c5 = importer::CheckBeforePlay(ma, dlc, "5");
    Say(c0.stars == 3 && !c0.build && c5.build, "A: CheckBeforePlay: nothing to do all on; build with pack 5 off ('" + c5.why + "')");
    // the game's verdict file: shown, and cleared by the next build
    std::ofstream(fs::u8path(ma) / "superstars" / "game_status.txt") << "out test reason\n";
    const std::string last = importer::LastGameProblem(ma);
    Say(!last.empty(), "A: the game's 'out' verdict reaches the launcher: '" + last + "'");
    // D. an older superstars.txt (no top line, Session 98 builds): still judged by built_on
    {
      std::string old = txt(ma), kept;
      std::istringstream in(old);
      for (std::string l; std::getline(in, l);)
        if (l.rfind("top ", 0) != 0) kept += l + "\n";
      const fs::path f = fs::u8path(ma) / "superstars" / "superstars.txt";
      std::ofstream(f, std::ios::trunc) << kept;
      Say(problem(ma, dlc, "").empty() && !problem(ma, dlc, "5").empty() && !problem(ma, "", "").empty() && problem(ma, dlc, "3").empty(),
          "D: an old superstars.txt (no top line): fits all on and with pack 3 off, left out with pack 5 off or no DLC");
      // (review SF1) the old rule only: an old file "built_on 4" (built with pack 5 off) is out when pack 5 comes back (a newer pack
      // with its own string file), fits while pack 5 stays off; the copy is never compared
      std::ofstream(f, std::ios::trunc) << "pack 6\nbuilt_on 4\nstar 397 music 100\n";
      Say(!problem(ma, dlc, "").empty() && problem(ma, dlc, "5").empty(),
          "D: an old file built on pack 4: out with pack 5 on ('" + problem(ma, dlc, "") + "'), fits with pack 5 off");
      // E. another copy of the top pack (a different revision / region): the recorded catalog FNV differs
      std::ofstream(f, std::ios::trunc) << kept << "top 5 12345678\n";
      const std::string cp = problem(ma, dlc, "");
      Say(!cp.empty() && cp.find("copy") != std::string::npos, "E: a different copy of pack 5 -> left out: '" + cp + "'");
      std::ofstream(f, std::ios::trunc) << old;
    }
    Say(wwe2k14::superstars_fit::GamePacks(fs::u8path(ma) / "superstars", "").empty(),
        "I: our own pack (id 6) is never one of the game's real packs (the gate), wherever it sits");
    // A rebuild clears the stale verdict
    Say(build(ma, sa, dlc, "") && importer::LastGameProblem(ma).empty(), "A: a rebuild removes the old game verdict");
  }
  // B. built while pack 5 is switched off (re-find: before Session 101 the builder ignored the switch, recorded built_on 5 and the
  //    game left the pack out with pack 5 off)
  auto [mb, sb] = make("off5", 1);
  if (Say(build(mb, sb, dlc, "5"), "B: BuildPack with pack 5 switched off")) {
    const std::string t = txt(mb);
    Say(LineOf(t, "top").rfind("top 4 ", 0) == 0 && LineOf(t, "built_on").find('5') == std::string::npos,
        "B: built on pack 4 (the top the game gets): '" + LineOf(t, "built_on") + "' / '" + LineOf(t, "top") + "'");
    Say(problem(mb, dlc, "5").empty(), "B: fits with pack 5 off");
    Say(!problem(mb, dlc, "").empty(), "B: pack 5 switched back on -> rebuild: '" + problem(mb, dlc, "") + "'");
  }
  // C. no DLC at all (USE MY DLC off): built on the disc
  auto [mc, sc] = make("nodlc", 1);
  if (Say(build(mc, sc, "", ""), "C: BuildPack with USE MY DLC off")) {
    Say(LineOf(txt(mc), "top") == "top 0 00000000" && LineOf(txt(mc), "built_on") == "built_on", "C: built on the disc: top 0, built_on empty");
    Say(problem(mc, "", "").empty(), "C: fits with no DLC");
    const importer::PlayCheck c = importer::CheckBeforePlay(mc, dlc, "");
    Say(c.build, "C: DLC switched on later -> CheckBeforePlay says build: '" + c.why + "'");
  }
  // F. a character on a slot that is not free (399, pack 4's; or a pack added since) is moved, not refused
  auto [mf, sf] = make("slot399", 2, {399, 397});
  {
    const std::vector<int> nodlc = importer::FreeSlots(gf, "");
    Say(std::find(nodlc.begin(), nodlc.end(), 399) == nodlc.end(), "F: FreeSlots leaves 399 out even with no DLC");
    const bool ok = build(mf, sf, dlc, "");
    importer::PackState back;
    std::string e;
    importer::LoadPack(mf, back, e);
    bool moved = false;
    for (const auto& l : importer::LastBuildLog()) moved = moved || l.find("moved to") != std::string::npos;
    Say(ok && back.stars.size() == 2 && back.stars[0].slot != 399 && back.stars[0].slot != back.stars[1].slot && moved,
        "F: slot 399 (pack 4 present) moved to " + std::to_string(back.stars.empty() ? -1 : back.stars[0].slot) + "; pack.json follows");
    const std::vector<std::string> said = importer::LastBuildMoves();
    Say(said.size() == 1 && said[0].find("moved from slot 399") != std::string::npos && said[0].find("saves") != std::string::npos,
        "F: the move is said to the player: '" + (said.empty() ? std::string() : said[0]) + "'");
    // (review S5a) no pack uses 399 (no DLC): a character already there stays; the reservation is for new imports only
    auto [mf2, sf2] = make("slot399_nodlc", 1, {399});
    const bool ok2 = build(mf2, sf2, "", "");
    importer::PackState back2;
    importer::LoadPack(mf2, back2, e);
    Say(ok2 && back2.stars.size() == 1 && back2.stars[0].slot == 399 && importer::LastBuildMoves().empty(),
        "F: with no pack using it, a character on 399 stays on 399 (no move)");
    // two on one slot: the second moves, never onto a slot another one keeps
    auto [mf3, sf3] = make("slot_twice", 3, {397, 397, 398});
    const bool ok3 = build(mf3, sf3, dlc, "");
    importer::PackState back3;
    importer::LoadPack(mf3, back3, e);
    std::set<int> got3;
    for (const auto& s : back3.stars) got3.insert(s.slot);
    Say(ok3 && back3.stars.size() == 3 && got3.size() == 3 && got3.count(397) && got3.count(398) && !got3.count(399),
        "F: a slot used twice: the second character moves to a slot nobody keeps (" + std::to_string(got3.size()) + " distinct)");
  }
  // L. one build at a time across processes (review S4): a fresh build.lock refuses, a stale one (> 10 min) is taken over and removed
  {
    auto [ml, sl] = make("lock", 1);
    const fs::path lock = fs::u8path(ml) / "superstars" / "build.lock";
    fs::create_directories(lock.parent_path(), ec);
    std::ofstream(lock) << "another builder\n";
    std::string e;
    const bool busy = importer::BuildPack(ml, gf, dlc, sl, nullptr, e, "");
    Say(!busy && e.find("another build") != std::string::npos && fs::exists(lock), "L: a running build's lock refuses a second one: '" + e + "'");
    fs::last_write_time(lock, fs::file_time_type::clock::now() - std::chrono::minutes(30), ec);
    Say(build(ml, sl, dlc, "") && !fs::exists(lock), "L: a stale lock (30 min) is taken over; the lock is gone after the build");
  }
  // M. (review S6) a pack with no pack.json (copied in by hand, or the list lost) is never removed by the check before PLAY
  {
    auto [mm, sm] = make("nolist", 1);
    build(mm, sm, dlc, "");
    fs::remove(fs::u8path(mm) / "superstars" / "pack.json", ec);
    const importer::PlayCheck c = importer::CheckBeforePlay(mm, dlc, "");
    Say(!c.build && c.stars == 0, "M: no pack.json + a built pack -> nothing to do (not 'remove the pack')");
  }
  // G. the mods folder moved: pack.json's absolute source path is gone, the copy in superstars\sources is used
  auto [mg, sg] = make("moved", 1);
  {
    importer::PackState st = sg;
    const std::string name = fs::u8path(st.stars[0].source_pac).filename().string();
    st.stars[0].source_pac = "C:\\gone\\old release\\mods\\superstars\\sources\\" + name;
    std::string e;
    importer::SavePack(mg, st, e);
    importer::PackState back;
    importer::LoadPack(mg, back, e);
    Say(!back.stars.empty() && fs::exists(fs::u8path(back.stars[0].source_pac)), "G: LoadPack finds the source in this mods folder: " +
                                                                                       (back.stars.empty() ? std::string() : back.stars[0].source_pac));
    Say(build(mg, st, dlc, ""), "G: BuildPack from the stale path works");
  }
  // J. every superstar removed: the build takes the old pack out (before, it refused an empty list and the old pack stayed in the game)
  {
    auto [mj, sj] = make("empty", 1);
    build(mj, sj, dlc, "");
    importer::PackState none;
    std::string e;
    importer::SavePack(mj, none, e);
    const importer::PlayCheck c = importer::CheckBeforePlay(mj, dlc, "");
    Say(c.build, "J: every superstar removed, old pack still there -> CheckBeforePlay says build: '" + c.why + "'");
    const bool ok = build(mj, none, dlc, "");
    Say(ok && !fs::exists(fs::u8path(mj) / "superstars" / "pack6") && !fs::exists(fs::u8path(mj) / "superstars" / "superstars.txt") &&
            !importer::CheckBeforePlay(mj, dlc, "").build,
        "J: building the empty list takes the pack out (no pack6, no superstars.txt; nothing left to do)");
  }
  // K. a base wrestler from DLC the game does not get: said plainly; a catalog-only DLC wrestler (pack 2's Ultimate Warrior, 254)
  //    with that pack on is not refused by it
  {
    auto [mk, sk] = make("dlcbase", 1);
    sk.stars[0].base_id = 385;  // Fandango: pack 4 ("DLC Pack 3")
    std::string e;
    const bool off4 = importer::BuildPack(mk, gf, dlc, sk, nullptr, e, "4");
    Say(!off4 && e.find("comes with DLC") != std::string::npos, "K: base 385 with pack 4 switched off -> '" + e + "'");
    Say(build(mk, sk, dlc, ""), "K: base 385 with every pack on builds");
    sk.stars[0].base_id = 254;
    e.clear();
    const bool warrior = importer::BuildPack(mk, gf, dlc, sk, nullptr, e, "");
    Say(warrior || e.find("comes with DLC") == std::string::npos, "K: base 254 (pack 2's catalog entry) is not refused as missing DLC" +
                                                                     (warrior ? std::string(" (built)") : " (other error: " + e + ")"));
  }
  // H. the top pack as an Xbox 360 package (re-find: before Session 101 packages were skipped, so the pack was built on the disc and
  //    the game, which mounts packages, left it out). Same three characters as A: the roster / names / shared pacs must match A's.
  if (!stfs_dir.empty()) {
    std::vector<int> slots;
    for (auto& s : sa.stars) slots.push_back(s.slot);
    auto [mh2, sh2] = make("stfs", 3, slots);
    const bool ok = build(mh2, sh2, stfs_dir, "");
    Say(ok, "H: BuildPack on a DLC folder holding pack 5 as an Xbox 360 package");
    if (ok) {
      const std::string ta = LineOf(txt(ma), "top"), th = LineOf(txt(mh2), "top");
      Say(th == ta, "H: built on the package: '" + th + "' (the folder build: '" + ta + "')");
      for (const char* f : {"pac/ch/chEtc_dlc06.pac", "pac/string_dlc06.pac", "pac/DLC_dlc06.pac", "pac/sound_dlc06.pac"}) {
        const Bytes x = Read(fs::u8path(ma) / "superstars" / "pack6" / f), y = Read(fs::u8path(mh2) / "superstars" / "pack6" / f);
        Say(!x.empty() && x == y, std::string("H: ") + f + " identical to the folder build's");
      }
      Say(problem(mh2, stfs_dir, "").empty(), "H: fits the package");
    }
  }
}

int main(int argc, char** argv) {
  std::string game, dlc, mods, source, name = "Mr. Kennedy", short_name = "KENNEDY", stfs_dir;
  int slot = 397, base = 100, ent = -1, count = 1;
  bool self_only = false, render = false, fit = false;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--dlc") dlc = next();
    else if (a == "--mods") mods = next();
    else if (a == "--source") source = next();
    else if (a == "--slot") slot = std::atoi(next().c_str());
    else if (a == "--base") base = std::atoi(next().c_str());
    else if (a == "--entrance-from") ent = std::atoi(next().c_str());
    else if (a == "--name") name = next();
    else if (a == "--short") short_name = next();
    else if (a == "--selftest-only") self_only = true;
    else if (a == "--render") render = true;
    else if (a == "--count") count = std::max(1, std::atoi(next().c_str()));
    else if (a == "--fit-cases") fit = true;     // Session 101 (lane 199)
    else if (a == "--stfs-dir") stfs_dir = next();  //   the package case (see FitCases)
  }
  SelfTest(dlc);
  if (self_only) return g_fail;
  if (fit) {
    FitCases(game, dlc, mods, source, stfs_dir);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  std::printf("sources in %s:\n", source.c_str());
  const auto infos = importer::InspectSources(source);
  for (const auto& s : infos) {
    std::printf("  '%s' ok=%d err='%s' claimed=%d attires=%d bip=%d suggested='%s'\n", s.entry_name.c_str(), s.ok, s.err.c_str(), s.claimed_id,
                s.attires, s.bip_bones, s.suggested_name.c_str());
    for (const auto& w : s.warnings) std::printf("    warning: %s\n", w.c_str());
  }
  const importer::SourceInfo one = importer::InspectSource(source);
  Say(one.ok, "InspectSource: claimed " + std::to_string(one.claimed_id));
  std::string e;
  const std::string pac = importer::ExtractSource(source, one.entry_name, mods, &e);
  Say(!pac.empty(), "ExtractSource -> " + pac + e);
  launcher::GameFiles gf;
  gf.SetRoot(fs::u8path(game));
  const auto free = importer::FreeSlots(gf, dlc);
  Say(!free.empty() && free.front() == 397 && std::find(free.begin(), free.end(), 399) == free.end(),
      "FreeSlots: " + std::to_string(free.size()) + " free, first " + std::to_string(free.empty() ? -1 : free.front()) + ", 399 excluded");
  importer::PackState st;
  importer::Superstar s;
  s.slot = slot, s.name = name, s.short_name = short_name, s.base_id = base, s.entrance_from = ent, s.source_pac = pac;
  if (render) {  // test pictures: SSFA red, SSFB green, SSFC blue (which colour a screen shows = which render it reads)
    for (int k = 0; k < 3; ++k) {
      s.render_w[k] = s.render_h[k] = 300;
      s.render_rgba[k].resize(300 * 300 * 4);
      for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 300; ++x) {
          uint8_t* p = &s.render_rgba[k][(size_t(y) * 300 + x) * 4];
          const bool in = (x - 150) * (x - 150) + (y - 120) * (y - 120) < 90 * 90;
          const uint8_t g = uint8_t(60 + y * 195 / 300);
          p[0] = k == 0 ? g : 30, p[1] = k == 1 ? g : 30, p[2] = k == 2 ? g : 30, p[3] = in ? 255 : 0;
        }
    }
  }
  st.stars.push_back(s);
  for (int k = 1; k < count && k < int(free.size()); ++k) {  // bulk: the same model on the next free slots
    importer::Superstar c = s;
    c.slot = free[size_t(k)] == slot ? free[0] : free[size_t(k)];
    c.name = name + " " + std::to_string(k + 1);
    st.stars.push_back(c);
  }
  std::printf("BuildPack:\n");
  const bool built = importer::BuildPack(mods, gf, dlc, st, [](float f, const char* w) { std::printf("    %3d%% %s\n", int(f * 100), w); }, e);
  for (const auto& l : importer::LastBuildLog()) std::printf("  | %s\n", l.c_str());
  Say(built, "BuildPack" + (built ? std::string() : ": " + e));
  if (!built) return g_fail;
  {  // the manifest: LoadPack gives back what was built (+ the portraits), SavePack keeps needs_build
    importer::PackState back;
    std::string e2;
    const bool ok = importer::LoadPack(mods, back, e2);
    Say(ok && back.stars.size() == st.stars.size() && back.stars[0].name == name && back.stars[0].slot == slot && !back.needs_build &&
            back.stars[0].render_rgba[0] == st.stars[0].render_rgba[0] && back.stars[0].render_w[0] == st.stars[0].render_w[0],
        "LoadPack: " + std::to_string(back.stars.size()) + " character(s), name, slot, portrait A " + std::to_string(back.stars[0].render_w[0]) +
            " wide (PNG round trip exact), needs_build false");
    back.needs_build = true;
    importer::PackState again;
    Say(importer::SavePack(mods, back, e2) && importer::LoadPack(mods, again, e2) && again.needs_build && again.stars.size() == back.stars.size(),
        "SavePack keeps the list and needs_build");
    back.needs_build = false;
    importer::SavePack(mods, back, e2);
  }
  // ---- read it back
  const fs::path out = fs::u8path(mods) / "superstars" / "pack6";
  std::printf("validation of %s:\n", out.string().c_str());
  const Bytes cat = Read(out / "info" / "catalog.dlc");
  io::CatInfo info;
  std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
  Say(!io::ParseCatalogLikeGame(cat, gate, info).empty(), "catalog refused by the UNPATCHED gate (the game side must open it)");
  gate[0] = 6u << 16 | 1;
  const std::string why = io::ParseCatalogLikeGame(cat, gate, info);
  Say(why.empty(), "catalog parses with gate word 0 = 00060001" + (why.empty() ? "" : ": " + why));
  Say(info.entries.size() == st.stars.size() && info.entries[0].char_id == slot && info.entries[0].type == 1,
      "entry " + std::to_string(info.entries.empty() ? -1 : info.entries[0].id) + " type 1 char " + std::to_string(slot) + " '" +
          (info.entries.empty() ? "" : info.entries[0].name) + "'");
  Say(wwe2k14::dlc::Scan(out.parent_path()).size() == 1 && wwe2k14::dlc::Scan(out.parent_path())[0].status == wwe2k14::dlc::Status::kReady,
      "our provider's Scan (src\\dlc_catalog.h) finds the pack ready, every listed file present");
  for (int col = 0; col < 2; ++col) {
    const auto groups = io::ReadArc(Read(out / "info" / (col ? "catalog_sd.arc" : "catalog_hd.arc")));
    int bad = 0, n = 0;
    for (const auto& f : info.files) {
      const int k = col ? f.b : f.a;
      if (k < 0) continue;
      ++n;
      io::Pac p;
      std::string e2;
      io::ReadPac(Read(Find(out, f.name)), p, e2);
      const Bytes mine = io::ArcRows(p);
      Bytes theirs;
      for (const auto& d : groups.at(k)) {
        theirs.insert(theirs.end(), d.tag.begin(), d.tag.end());
        io::PB32(theirs, d.flags), io::PB32(theirs, 0);
        for (const auto& [nm, sec, sz] : d.ents) theirs.insert(theirs.end(), nm.begin(), nm.end()), io::PB32(theirs, sec), io::PB32(theirs, sz);
      }
      bad += mine != theirs;
    }
    Say(!bad && int(groups.size()) == n, std::string(col ? "SD" : "HD") + " arc: " + std::to_string(groups.size()) + " groups = the pacs' own directories");
  }
  char pp[8];
  std::snprintf(pp, sizeof pp, "%02d", info.pack);
  // names
  {
    io::Pac p;
    std::string e2;
    io::ReadPac(Read(out / "pac" / ("string_dlc" + std::string(pp) + ".pac")), p, e2);
    int tables = 0, right = 0;
    for (auto& x : p.FindDir("SDB ")->ents) {
      std::vector<io::Leaf> leaves;
      io::ReadPach(x.blob, leaves);
      for (auto& l : leaves) {
        const Bytes d = io::Decode(l.raw);
        if (d.size() < 8) continue;
        const uint32_t cnt = io::L32(&d[4]);
        if (io::L32(&d[0]) != 0 || 8 + 12ull * cnt > d.size()) continue;
        bool names = false;
        std::string got[3];
        uint32_t prev = 0;
        bool sorted = true;
        for (uint32_t i = 0; i < cnt; ++i) {
          const uint32_t off = io::L32(&d[8 + 12 * i]), len = io::L32(&d[12 + 12 * i]), id = io::L32(&d[16 + 12 * i]);
          sorted = sorted && (i == 0 || id > prev), prev = id;
          names = names || id == 60100;
          for (int k = 0; k < 3; ++k)
            if (id == uint32_t(60000 + 2000 * k + slot)) got[k] = std::string(reinterpret_cast<const char*>(&d[off]), len ? len - 1 : 0);
        }
        if (!names) continue;
        ++tables;
        right += sorted && got[0] == name && got[1] == short_name && got[2] == name;
      }
    }
    Say(tables >= 1 && right == tables, std::to_string(right) + " of " + std::to_string(tables) + " name tables hold '" + name + "' / '" + short_name +
                                            "' at " + std::to_string(60000 + slot) + " / " + std::to_string(62000 + slot) + " / " + std::to_string(64000 + slot) + ", ids sorted");
  }
  // row
  {
    io::Pac p;
    std::string e2;
    io::ReadPac(Read(out / "pac" / "ch" / ("chEtc_dlc" + std::string(pp) + ".pac")), p, e2);
    std::vector<io::Leaf> leaves;
    io::ReadPach(p.Find("CHAR", "DAT ")->blob, leaves);
    Bytes rec, src;
    for (auto& l : leaves) {
      if (int(l.id) == slot) rec = io::Decode(l.raw);
      if (int(l.id) == base) src = io::Decode(l.raw);
    }
    const uint8_t* R = rec.size() == 208 ? &rec[4] : nullptr;
    Say(R && io::L16(R) == slot && io::L16(R + 4) == slot && io::L16(R + 200) == slot && R[16] == 1 && R[17] == info.pack &&
            io::L16(R + 6) == 60000 + slot,
        "row " + std::to_string(slot) + " decodes: ids, active, pack code " + std::to_string(R ? R[17] : -1) + ", text " + std::to_string(R ? io::L16(R + 6) : -1));
    std::string diff;
    for (size_t o = 4; R && o < 208; ++o)
      if (rec[o] != src[o]) diff += " " + std::to_string(o - 4);
    std::printf("  row %d differs from row %d at record bytes%s\n", slot, base, diff.c_str());
  }
  // models
  {
    io::Pac p;
    std::string e2;
    io::ReadPac(Read(out / "pac" / "ch" / ("ch" + std::to_string(slot) + "_dlc" + pp + ".pac")), p, e2);
    std::string names;
    bool pach = true;
    const io::Dir* emd = p.FindDir("EMD ");
    for (auto& x : emd->ents) {
      const Bytes& bl = io::Blob(*emd, x);
      names += " " + x.name + (x.alias >= 0 ? "(alias)" : ""), pach = pach && bl.size() >= 4 && std::memcmp(bl.data(), "PACH", 4) == 0;
    }
    char want[16];
    std::snprintf(want, sizeof want, "000%03d02", slot);
    Say(names.find(want) != std::string::npos && pach, "models:" + names + " (each a PACH)");
  }
  std::printf("failures: %d\n", g_fail);
  return g_fail;
}
