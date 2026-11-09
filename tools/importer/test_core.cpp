// Session 98: the Superstar Importer core's PC check (src\importer). Built by tools\importer\build_test.ps1; runs on this PC
// with the player's own files and writes ONLY under the --mods folder you give it (keep it under out\: game data).
//
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source ZIP_OR_PAC [--slot 397] [--base 100] [--name "Mr. Kennedy"]
//                 [--short KENNEDY] [--entrance-from N] [--selftest-only] [--render]
//
// 1. selftest: the writers against the real packs (arcs rebuilt from the pacs' directories, chEtc / DLC_HD re-packed: byte for
//    byte, like build_mod_pack.py --selftest); deflate / BC3 / string-table round trips.
// 2. InspectSources + ExtractSource on the source, then BuildPack, then reads the pack back (the game's parser order, arcs vs
//    pac directories, the name texts, row SLOT, the model names). Exit code = failures.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/dlc_catalog.h"
#include "../../src/importer/importer_core.h"
#include "../../src/importer/pack_io.h"
#include "../../src/importer/bc3.h"

namespace fs = std::filesystem;
using importer::io::Bytes;
namespace io = importer::io;

static int g_fail = 0;
static void Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
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

int main(int argc, char** argv) {
  std::string game, dlc, mods, source, name = "Mr. Kennedy", short_name = "KENNEDY";
  int slot = 397, base = 100, ent = -1, count = 1;
  bool self_only = false, render = false;
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
  }
  SelfTest(dlc);
  if (self_only) return g_fail;
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
