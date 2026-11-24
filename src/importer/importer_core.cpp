// Session 98: the Superstar Importer core (importer_core.h). A C++ port of tools\importer\build_mod_pack.py (proven in game by
// runs 073 / 074: pack 6 parsed, slot 397 in the DOWNLOADED SUPERSTARS list and in a match), generalised to N characters
// in one pack, with the character's own name texts (string pac), the base's entrance banks, tron and renders, and DXT5
// renders from the UI. Everything is read from the player's own files and written under <mods>\superstars\ only.
#include "importer_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#include "../assets/char_mods.h"
#include "../dlc_catalog.h"
#include "../tron_slots_core.h"  // S110 TRONSLOT: the custom tron numbers
#include "../xctd_file.h"
#include "asset_ids.h"  // S110 BANKID: every asset number of an import (his entrance bank, victory, plate) is asked there
#include "bc3.h"
#include "enex_rows.h"  // S110 ENEX: the DLC pac's per-character entrance rows (/ENEX/BIN, /ENEX/LSD)
#include "entrance_fx.h"  // S110 ENTFX: the entrance's scene objects / particles (gm, prtcls pacs)
#include "entrance_movies.h"  // S110 ENTMOV: the name-plate movie (evtmov pac)
#include "../seat_card.h"  // S110 SEAT-A: card.txt + rows.bin (the library's format, shared with the game)
#include "../seat_bind_core.h"  // S110 LIBSIZE-2: the migration's check re-targets the reference identity's rows to the old pack's seat (RetargetCard / RetargetName)
#include "pack_fit.h"  // Session 101: the game's DLC view + the "does the pack fit" rule (shared with src\dlc.h and the launchers)
#include "pack_io.h"
#include "screen_text.h"  // S110 SEAT-C: LastGameProblem's sentences
#include "pack_entry.h"  // S110: the pack's one type-21 catalog entry + its bundle records
#include "slot_retire.h"  // S105 IMP: retired slots (a new import never inherits a save's old row)
#include "png.h"

namespace importer {

namespace fs = std::filesystem;
using io::Bytes;

namespace {

std::vector<std::string> g_log;
std::vector<std::string> g_moves;  // Session 101 (review S5b): the last build's slot moves, said to the player
std::mutex g_log_mutex;

fs::path U8Path(const std::string& s) { return fs::u8path(s); }
std::string PathU8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// "pac\ch\x.pac" -> a path under `root` (the game's backslashes; POSIX folders need slashes).
fs::path Under(const fs::path& root, const std::string& rel) {
  fs::path p = root;
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find_first_of("\\/", i);
    if (j == std::string::npos) j = rel.size();
    if (j > i) p /= U8Path(rel.substr(i, j - i));
    i = j + 1;
  }
  return p;
}
// The same, with a case-insensitive last part (the catalogs' case differs from the folder's on some files).
fs::path FindUnder(const fs::path& root, const std::string& rel) {
  fs::path p = Under(root, rel);
  std::error_code ec;
  if (fs::exists(p, ec)) return p;
  const std::string want = Lower(PathU8(p.filename()));
  for (const auto& e : fs::directory_iterator(p.parent_path(), ec))
    if (Lower(PathU8(e.path().filename())) == want) return e.path();
  return {};
}
bool ReadAll(const fs::path& p, Bytes& out, uint64_t cap = 1ull << 31) {
  out.clear();
  std::error_code ec;
  const uint64_t n = fs::file_size(p, ec);
  if (ec || n > cap) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.resize(size_t(n));
  return n == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n)));
}
bool WriteAll(const fs::path& p, const Bytes& b) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  return f && f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())) && f.flush();
}
bool WriteText(const fs::path& p, const std::string& s) { return WriteAll(p, Bytes(s.begin(), s.end())); }
// Session 101 (lane 199): pack.json records each source pac as an absolute path. When that file is gone (the player unzipped a new
// release elsewhere and copied his mods folder over, or moved the folder), the copy ExtractSource made in THIS mods folder's
// superstars\sources\ is used, found by its file name; otherwise the next build failed with "cannot read <old path>".
std::string ResolveSource(const fs::path& stars_root, const std::string& recorded) {
  std::error_code ec;
  if (recorded.empty() || fs::is_regular_file(U8Path(recorded), ec)) return recorded;
  const size_t cut = recorded.find_last_of("\\/");  // (either separator: a pack.json from the other platform)
  const std::string name = cut == std::string::npos ? recorded : recorded.substr(cut + 1);
  if (name.empty()) return recorded;
  const fs::path here = stars_root / "sources" / U8Path(name);
  return fs::is_regular_file(here, ec) ? PathU8(here) : recorded;
}
Bytes Unxctd(Bytes raw) {
  if (!wwe2k14::xctd::IsXctd(raw)) return raw;
  Bytes out;
  std::string e;
  if (!wwe2k14::xctd::Unpack(raw, out, e)) return {};
  return out;
}

// ------------------------------------------------------------------ zip members (char_mods.h's rules and limits)
struct Member {
  std::string name;
  uint16_t method = 0, flags = 0;
  uint32_t csize = 0, usize = 0, lho = 0;
};
bool ListZip(const fs::path& src, std::vector<Member>& out, std::string& err) {
  using namespace assets::char_mods::imp_detail;
  out.clear();
  std::error_code ec;
  const uint64_t sz = fs::file_size(src, ec);
  std::ifstream f(src, std::ios::binary);
  if (ec || !f) return err = "cannot read the file", false;
  if (sz < 22) return err = "not a zip", false;
  const size_t tail = size_t(std::min<uint64_t>(sz, 22 + 65535));
  Bytes t(tail);
  if (!ReadAt(f, sz - tail, t.data(), tail)) return err = "read failed", false;
  size_t e = std::string::npos;
  for (size_t i = tail - 22 + 1; i-- > 0;)
    if (L32(&t[i]) == 0x06054b50u) {
      e = i;
      break;
    }
  if (e == std::string::npos) return err = "not a zip (no end record)", false;
  const uint32_t cd_size = L32(&t[e + 12]), cd_off = L32(&t[e + 16]);
  const uint16_t total = L16(&t[e + 10]);
  if (cd_size > (64u << 20) || uint64_t(cd_off) + cd_size > sz) return err = "damaged zip", false;
  Bytes cd(cd_size);
  if (cd_size && !ReadAt(f, cd_off, cd.data(), cd_size)) return err = "read failed", false;
  size_t p = 0;
  for (uint16_t k = 0; k < total && p + 46 <= cd.size(); ++k) {
    if (L32(&cd[p]) != 0x02014b50u) break;
    Member m;
    m.flags = L16(&cd[p + 8]), m.method = L16(&cd[p + 10]);
    m.csize = L32(&cd[p + 20]), m.usize = L32(&cd[p + 24]), m.lho = L32(&cd[p + 42]);
    const size_t nl = L16(&cd[p + 28]), xl = L16(&cd[p + 30]), cl = L16(&cd[p + 32]);
    if (p + 46 + nl + xl + cl > cd.size()) break;
    m.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    p += 46 + nl + xl + cl;
    out.push_back(m);
  }
  return true;
}
std::string ReadMember(const fs::path& src, const Member& m, Bytes& full) {
  using namespace assets::char_mods;
  using namespace assets::char_mods::imp_detail;
  full.clear();
  if (BadMemberPath(m.name)) return "refused an unsafe path in the zip: " + m.name;
  if (m.flags & 1) return "the zip is password protected";
  if (m.usize > kMaxPac || m.csize > kMaxPac) return "a member of the zip is too big";
  if (m.method != 0 && m.method != 8) return "unsupported zip compression";
  std::error_code ec;
  const uint64_t sz = fs::file_size(src, ec);
  std::ifstream f(src, std::ios::binary);
  uint8_t lh[30];
  if (!f || !ReadAt(f, m.lho, lh, 30) || L32(lh) != 0x04034b50u) return "damaged zip";
  const uint64_t data_off = uint64_t(m.lho) + 30 + L16(lh + 26) + L16(lh + 28);
  if (data_off + m.csize > sz) return "damaged zip";
  Bytes raw(m.csize);
  if (m.csize && !ReadAt(f, data_off, raw.data(), m.csize)) return "read failed";
  if (m.method == 0) full = std::move(raw);
  else if (!Inflate(raw.data(), raw.size(), full, size_t(m.usize) + 1)) return "damaged zip member " + m.name;
  if (full.size() != m.usize) return "damaged zip member " + m.name;
  return "";
}
bool IsZipFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  char m[4] = {};
  return f.read(m, 4) && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
}

// "folder/Mr_Kennedy_09.pac" -> "Mr Kennedy": the file's stem, '_' '-' '.' as spaces, trailing number / "zip" / "pac" /
// "mod" / "v2" words dropped, spaces collapsed.
std::string SuggestName(const std::string& path) {
  std::string s = path.substr(path.find_last_of("\\/") == std::string::npos ? 0 : path.find_last_of("\\/") + 1);
  if (s.size() > 4 && Lower(s.substr(s.size() - 4)) == ".pac") s.resize(s.size() - 4);
  for (char& c : s)
    if (c == '_' || c == '-' || c == '.') c = ' ';
  std::vector<std::string> words;
  std::istringstream in(s);
  for (std::string w; in >> w;) words.push_back(w);
  auto junk = [](const std::string& w) {
    const std::string l = Lower(w);
    if (l == "zip" || l == "pac" || l == "mod" || l == "final" || l == "new") return true;
    size_t i = (l.size() > 1 && l[0] == 'v') ? 1 : 0;
    return i < l.size() && l.find_first_not_of("0123456789", i) == std::string::npos;
  };
  while (words.size() > 1 && junk(words.back())) words.pop_back();
  std::string o;
  for (const auto& w : words) o += (o.empty() ? "" : " ") + w;
  return o;
}

// ------------------------------------------------------------------ character pacs
struct ModPac {
  io::Pac pac;
  int claimed = -1;
  std::map<int, std::map<int, int>> att;  // attire digit -> kind digit -> entry index in the EMD dir
};
bool IsEmdName(const std::string& n) {
  return n.size() == 8 && n.compare(0, 3, "000") == 0 && n.find_first_not_of("0123456789") == std::string::npos;
}
std::string ParseModPac(Bytes bytes, ModPac& m, SourceInfo* info) {
  bytes = Unxctd(std::move(bytes));
  if (bytes.empty()) return "a compressed pac that could not be unpacked";
  if (bytes.size() > assets::char_mods::kMaxPac) return "not a character pac (too big)";
  std::string err;
  if (!io::ReadPac(bytes, m.pac, err)) return "not a character pac (" + err + ")";
  io::Dir* emd = m.pac.FindDir("EMD ");
  if (!emd || emd->kind != 1 || emd->ents.empty()) return "not a character pac (no EMD models)";
  std::map<int, int> votes;
  for (size_t i = 0; i < emd->ents.size(); ++i) {
    const std::string& n = emd->ents[i].name;
    if (!IsEmdName(n)) continue;
    const int id = std::atoi(n.substr(3, 3).c_str());
    ++votes[id];
  }
  if (votes.empty()) return "not a character pac (no 000NNNxx model names)";
  m.claimed = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  for (size_t i = 0; i < emd->ents.size(); ++i) {
    const std::string& n = emd->ents[i].name;
    if (!IsEmdName(n) || std::atoi(n.substr(3, 3).c_str()) != m.claimed) continue;
    m.att[n[6] - '0'][n[7] - '0'] = int(i);
  }
  int with_ring = 0;
  for (auto& [a, kinds] : m.att) with_ring += kinds.count(2) ? 1 : 0;
  if (info) {
    info->claimed_id = m.claimed;
    info->attires = with_ring;
    if (votes.size() > 1) info->warnings.push_back("its models claim more than one character id: " + std::to_string(m.claimed) + " used");
    if (!with_ring) info->warnings.push_back("no in-ring model (000NNNx2): the game may not find a model to load");
    // bones: distinct "Bip..." names in attire 0's body model (the JBOY at leaf 0 of the first model PACH)
    const io::Entry* body = nullptr;
    for (auto& [a, kinds] : m.att) {
      auto it = kinds.count(2) ? kinds.find(2) : kinds.begin();
      if (it != kinds.end()) body = &emd->ents[size_t(it->second)];
      break;
    }
    std::vector<io::Leaf> leaves;
    if (body && io::ReadPach(io::Blob(*emd, *body), leaves)) {
      for (const io::Leaf& l : leaves) {
        if (l.id != 0) continue;
        const Bytes d = io::Decode(l.raw);
        if (d.size() < 4 || std::memcmp(d.data(), "JBOY", 4) != 0) {
          info->warnings.push_back("attire 0's body is not a JBOY model");
          break;
        }
        std::set<std::string> names;
        for (size_t i = 0; i + 3 < d.size(); ++i) {
          if (d[i] != 'B' || d[i + 1] != 'i' || d[i + 2] != 'p') continue;
          size_t j = i;
          while (j < d.size() && j - i < 48 && d[j] >= 0x20 && d[j] < 0x7F) ++j;
          if (j < d.size() && d[j] == 0) names.insert(std::string(reinterpret_cast<const char*>(&d[i]), j - i));
        }
        info->bip_bones = int(names.size());
        if (names.empty()) info->warnings.push_back("no Bip bones in its skeleton: motions and entrances may not move it");
      }
    }
  }
  return "";
}

SourceInfo InspectBytes(Bytes bytes, const std::string& label) {
  SourceInfo s;
  s.suggested_name = SuggestName(label);
  ModPac m;
  s.err = ParseModPac(std::move(bytes), m, &s);
  s.ok = s.err.empty();
  return s;
}

// ------------------------------------------------------------------ tiny JSON (our own manifest only)
struct Json {
  enum T { kNull, kBool, kNum, kStr, kArr, kObj } t = kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* Get(const char* k) const {
    for (auto& [kk, v] : o)
      if (kk == k) return &v;
    return nullptr;
  }
  int Int(const char* k, int def) const { auto* v = Get(k); return v && v->t == kNum ? int(v->n) : def; }
  bool Bool(const char* k, bool def) const { auto* v = Get(k); return v && v->t == kBool ? v->b : def; }
  std::string Str(const char* k) const { auto* v = Get(k); return v && v->t == kStr ? v->s : std::string(); }
};
struct JsonParser {
  const std::string& t;
  size_t p = 0;
  int depth = 0;
  void Ws() { while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p; }
  bool Str(std::string& out) {
    if (p >= t.size() || t[p] != '"') return false;
    ++p;
    while (p < t.size() && t[p] != '"') {
      char c = t[p++];
      if (c == '\\' && p < t.size()) {
        const char e = t[p++];
        if (e == 'n') c = '\n';
        else if (e == 't') c = '\t';
        else if (e == 'u' && p + 4 <= t.size()) {
          const unsigned v = unsigned(std::strtoul(t.substr(p, 4).c_str(), nullptr, 16));
          p += 4;
          if (v < 0x80) out += char(v);
          else if (v < 0x800) out += char(0xC0 | (v >> 6)), out += char(0x80 | (v & 0x3F));
          else out += char(0xE0 | (v >> 12)), out += char(0x80 | ((v >> 6) & 0x3F)), out += char(0x80 | (v & 0x3F));
          continue;
        } else c = e;
      }
      out += c;
    }
    if (p >= t.size()) return false;
    ++p;
    return true;
  }
  bool Val(Json& v) {
    if (++depth > 32) return false;
    Ws();
    bool ok = true;
    if (p >= t.size()) ok = false;
    else if (t[p] == '{') {
      v.t = Json::kObj, ++p;
      Ws();
      if (p < t.size() && t[p] == '}') ++p;
      else
        for (;;) {
          Ws();
          std::string k;
          Json x;
          if (!Str(k)) { ok = false; break; }
          Ws();
          if (p >= t.size() || t[p] != ':') { ok = false; break; }
          ++p;
          if (!Val(x)) { ok = false; break; }
          v.o.emplace_back(k, std::move(x));
          Ws();
          if (p < t.size() && t[p] == ',') { ++p; continue; }
          if (p < t.size() && t[p] == '}') { ++p; break; }
          ok = false;
          break;
        }
    } else if (t[p] == '[') {
      v.t = Json::kArr, ++p;
      Ws();
      if (p < t.size() && t[p] == ']') ++p;
      else
        for (;;) {
          Json x;
          if (!Val(x)) { ok = false; break; }
          v.a.push_back(std::move(x));
          Ws();
          if (p < t.size() && t[p] == ',') { ++p; continue; }
          if (p < t.size() && t[p] == ']') { ++p; break; }
          ok = false;
          break;
        }
    } else if (t[p] == '"') {
      v.t = Json::kStr;
      ok = Str(v.s);
    } else if (t.compare(p, 4, "true") == 0) v.t = Json::kBool, v.b = true, p += 4;
    else if (t.compare(p, 5, "false") == 0) v.t = Json::kBool, p += 5;
    else if (t.compare(p, 4, "null") == 0) p += 4;
    else {
      char* e = nullptr;
      v.t = Json::kNum, v.n = std::strtod(t.c_str() + p, &e);
      if (e == t.c_str() + p) ok = false;
      p = size_t(e - t.c_str());
    }
    --depth;
    return ok;
  }
};
// UTF-8 -> Latin-1 (the catalog entry names are Windows-1252 bytes); '?' for anything else.
std::string Latin1(const std::string& u) {
  std::string o;
  for (size_t i = 0; i < u.size();) {
    const unsigned char c = u[i];
    if (c < 0x80) o += char(c), ++i;
    else if ((c & 0xE0) == 0xC0 && i + 1 < u.size()) {
      const unsigned v = (c & 0x1F) << 6 | (u[i + 1] & 0x3F);
      o += v < 0x100 ? char(v) : '?';
      i += 2;
    } else {
      o += '?';
      ++i;
      while (i < u.size() && (u[i] & 0xC0) == 0x80) ++i;
    }
  }
  return o;
}

// ------------------------------------------------------------------ the build
struct RealPack {
  int id = -1;
  fs::path root;  // the pack's folder, or the package file (pkg set)
  std::shared_ptr<wwe2k14::dlc::StfsPackage> pkg;  // Session 101: an Xbox 360 package, read in place (it was skipped before, so a
                                                   // player with packages got a pack built on the disc, which the game then left out)
  uint16_t revision = 0;
  uint32_t sig = 0;  // Session 101: its catalog's FNV-1a (superstars.txt "top": which copy of the pack it was built on)
  std::vector<io::CatFile> files;
  std::vector<io::CatEntry> entries;
  std::string Pp() const { char b[8]; std::snprintf(b, sizeof b, "%02d", id); return b; }
  bool Has(const std::string& rel) const {
    for (const auto& f : files)
      if (Lower(f.name) == Lower(rel)) return true;
    return false;
  }
  bool roster = false;  // (review N6) pack_fit.h IsRosterPack - the one rule the game uses too
  bool Roster() const { return roster; }
  // One of its files (a catalog path, any case), from the folder or from inside the package.
  bool Read(const std::string& rel, Bytes& out, uint64_t cap = 1ull << 31) const {
    out.clear();
    if (pkg) {
      const wwe2k14::dlc::StfsPackage::File* f = pkg->Find(rel);
      return f && pkg->Read(*f, out, size_t(std::min<uint64_t>(cap, SIZE_MAX)));
    }
    const fs::path f = FindUnder(root, rel);
    return !f.empty() && ReadAll(f, out, cap);
  }
  uint64_t Size(const std::string& rel) const {
    if (pkg) {
      const wwe2k14::dlc::StfsPackage::File* f = pkg->Find(rel);
      return f ? f->length : 0;
    }
    std::error_code ec;
    const fs::path f = FindUnder(root, rel);
    const uint64_t n = f.empty() ? 0 : fs::file_size(f, ec);
    return ec ? 0 : n;
  }
};

// Session 101 (lane 199): the real packs THE GAME WILL GET (src\importer\pack_fit.h GamePacks: ready, not switched off, accepted by
// the game's gate; folders and packages), so the pack is built on exactly what the game sees. `off` = the launcher's dlc_off ("3,5");
// before this it was ignored, and a pack built on a switched-off pack was left out by the game.
std::vector<RealPack> RealPacks(const std::string& dlc_dir, const fs::path& exclude, const std::string& off = "") {
  std::vector<RealPack> out;
  std::error_code ec;
  if (dlc_dir.empty() || !fs::is_directory(U8Path(dlc_dir), ec)) return out;
  const fs::path ex = exclude.empty() ? fs::path() : fs::weakly_canonical(exclude, ec);
  for (const wwe2k14::dlc::Pack& p : wwe2k14::superstars_fit::GamePacks(U8Path(dlc_dir), off)) {
    if (!ex.empty() && !p.package) {
      const std::string a = Lower(PathU8(fs::weakly_canonical(p.root, ec))), b = Lower(PathU8(ex));
      if (a.compare(0, b.size(), b) == 0) continue;  // our own pack (a mods folder inside the DLC folder)
    }
    RealPack r;
    Bytes cat;
    if (p.package) {
      r.pkg = std::make_shared<wwe2k14::dlc::StfsPackage>();
      std::string why;
      const wwe2k14::dlc::StfsPackage::File* f = nullptr;
      if (!r.pkg->Open(p.root, why) || !(f = r.pkg->Find("info\\catalog.dlc")) || !r.pkg->Read(*f, cat, 1 << 20)) continue;
    } else if (!ReadAll(p.root / "info" / "catalog.dlc", cat, 1 << 20)) {
      continue;
    }
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    if (!io::ParseCatalogLikeGame(cat, gate, info).empty()) continue;
    r.id = info.pack, r.root = p.root, r.revision = info.revision, r.files = info.files, r.entries = info.entries;
    r.sig = wwe2k14::dlc::Fnv32(cat.data(), cat.size());
    r.roster = wwe2k14::superstars_fit::IsRosterPack(p);
    out.push_back(std::move(r));
  }
  std::sort(out.begin(), out.end(), [](const RealPack& a, const RealPack& b) { return a.id < b.id; });
  return out;
}

struct Builder {
  launcher::GameFiles game;
  std::vector<RealPack> packs;
  const RealPack* top = nullptr;  // the highest pack with a chEtc: its cumulative files are the base of ours
  int pack_id = 6;
  std::string pp;  // "06"
  fs::path out;    // pack6.building
  std::vector<std::string>* log;
  std::vector<std::string> warnings;
  std::function<void(float, const char*)> progress;
  std::set<int> built_on;

  void Log(const std::string& s) { log->push_back(s); }
  void Prog(float f, const char* what) {
    if (progress) progress(std::clamp(f, 0.f, 1.f), what);
  }
  bool ReadGame(const std::string& rel, Bytes& out) {
    out.clear();
    if (!game.Exists(rel)) return false;
    if (!game.Stream(rel, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); })) return false;
    out = Unxctd(std::move(out));
    return !out.empty();
  }
  bool ReadPackFile(const RealPack& p, const std::string& rel, Bytes& out) { return p.Read(rel, out); }
  // The pack that holds character `id`'s model pac (a DLC character), or nullptr (a disc character).
  const RealPack* PackOfChar(int id) const {
    for (const RealPack& p : packs)
      if (p.Has("pac\\ch\\ch" + std::to_string(id) + "_dlc" + p.Pp() + ".pac")) return &p;
    return nullptr;
  }
  std::string Rel(const std::string& base, const std::string& ext = ".pac") const { return base + "_dlc" + pp + ext; }
  // A cumulative file ("pac\\ch\\chEtc", "pac\\string", ...): the top real pack's, or the disc's own when the player has no DLC
  // (Jarrod, S98: the importer must work without any DLC).
  std::string CumulativeName(const std::string& base) const { return top ? base + "_dlc" + top->Pp() + ".pac" : base + ".pac"; }
  bool ReadCumulative(const std::string& base, Bytes& out) {
    return top ? ReadPackFile(*top, CumulativeName(base), out) : ReadGame(CumulativeName(base), out);
  }
  std::string BaseName() const { return top ? "pack " + std::to_string(top->id) : std::string("the game disc"); }
};

// A packed leaf's version word (+4); 0x100 when the leaf is too short or has none (review: never read past a short leaf).
uint32_t VersionWord(const Bytes& raw) { return raw.size() >= 8 && io::L32(&raw[4]) ? io::L32(&raw[4]) : 0x100; }

// ---- chEtc: one row set per character, cloned from his base (patch_rows in build_mod_pack.py)
constexpr size_t kRecHead = 4;  // the file row's header {u16 2, u16 208}
// ---- entrance banks: EVP (the EV table) + EVZ (the motions) of bank E and its variants 10000 + 1000 G + E, renamed to the slot
struct BankSrc {
  Bytes evp, evz;  // the raw (BPE) entries
};
// EV table (big-endian, decoded): +0x20 u16 bank id; records from +0x44 (count = low 24 bits of +0x40), X at record +12:
// an actor's X = character x 100 + attire x 10 + kind, rewritten E -> slot (docs\formats\entrance_ev_table.md).
Bytes RetargetEv(const Bytes& evp_raw, int from, int to_bank, int to_char, int& actors) {
  Bytes ev = io::Decode(evp_raw);
  actors = 0;
  if (ev.size() < 0x44 || std::memcmp(ev.data(), "0FOP", 4) != 0) return {};
  io::WB16(&ev[0x20], uint16_t(to_bank));
  const uint32_t n = io::B32(&ev[0x40]) & 0xFFFFFF;
  for (uint32_t i = 0; i < n && 0x44 + 32ull * (i + 1) <= ev.size(); ++i) {
    uint8_t* r = &ev[0x44 + 32 * size_t(i)];
    const uint32_t x = io::B32(r + 12);
    if (r[16] == 1 && int(x / 100) == from) io::WB32(r + 12, uint32_t(to_char) * 100 + x % 100), ++actors;
  }
  return io::BpeIdentity(ev, VersionWord(evp_raw));
}

// Reads every needed bank from the disc's Entrance00..NN and the packs' Entrance_dlcPP (first one found wins, disc first).
// S110 WINNING: victory = true reads the victory banks 6000 + e (no variants) from Winning0N.pac / Winning_dlcPP.pac instead (same EVP / EVZ layout).
std::map<int, BankSrc> FindBanks(Builder& b, const std::set<int>& wanted_chars, bool victory = false) {
  std::set<std::string> want;
  const int bank0 = victory ? 6000 : 0;
  for (int e : wanted_chars) {
    want.insert(std::to_string(bank0 + e));
    if (!victory)
      for (int g = 0; g < 10; ++g) want.insert(std::to_string(10000 + 1000 * g + e));
  }
  const std::string stem = victory ? "winning" : "entrance";
  std::map<int, BankSrc> got;
  auto take = [&](io::Pac& p) {
    io::Dir *evp = p.FindDir("EVP "), *evz = p.FindDir("EVZ ");
    if (!evp || !evz) return;
    for (io::Entry& e : evp->ents) {
      const std::string n = io::Trim(e.name);
      if (!want.count(n) || got.count(std::atoi(n.c_str()))) continue;
      for (io::Entry& z : evz->ents)
        if (io::Trim(z.name) == n) got[std::atoi(n.c_str())] = {e.blob, z.blob};
    }
  };
  std::vector<std::string> disc;
  for (const std::string& f : b.game.ListRoot("pac\\evt")) {
    const std::string l = Lower(f);
    const size_t n = stem.size();
    if (l.size() == n + 6 && l.compare(0, n, stem) == 0 && l.compare(n + 2, 4, ".pac") == 0 && std::isdigit((unsigned char)l[n]))
      disc.push_back(f);
  }
  std::sort(disc.begin(), disc.end());
  // (each file is unpacked once; the scan stops when every wanted bank is found: a bank's variants sit in its own file, as in
  // Entrance00 (100 / 10100) and Entrance_dlc04 (385 / 10385..14385))
  auto all = [&] {
    for (int e : wanted_chars)
      if (!got.count(bank0 + e)) return false;
    return true;
  };
  for (const std::string& f : disc) {
    if (all()) return got;
    Bytes d;
    io::Pac p;
    std::string e;
    if (b.ReadGame("pac\\evt\\" + f, d) && io::ReadPac(d, p, e)) take(p);
  }
  for (const RealPack& rp : b.packs) {
    if (all()) return got;
    const std::string rel = std::string("pac\\evt\\") + (victory ? "Winning" : "Entrance") + "_dlc" + rp.Pp() + ".pac";
    Bytes d;
    io::Pac p;
    std::string e;
    if (rp.Has(rel) && b.ReadPackFile(rp, rel, d) && io::ReadPac(d, p, e)) take(p);
  }
  return got;
}

// ---- renders: SSFA / SSFB / SSFC of a character, from the UI (RGBA) or copied from the base's own
// (review: a full pack's renders are ~90 MB per resolution, so HD and SD are made in turn, never held together)
using Renders = std::array<Bytes, 3>;  // the pac entries (BPE-packed DDS) for SSFA / SSFB / SSFC at one resolution
// (S110 LIBSIZE-2: a picture the player gave none of is no longer copied from the base's menu pac: it is an alias row, library_file.h AliasesOf)
// The UI's pictures at one resolution; have[k] = SSFx k came from the UI.
void UiRenders(const Superstar& s, bool sd, Renders& r, std::array<bool, 3>& have) {
  static const int hw[3] = {512, 512, 256}, hh[3] = {512, 512, 256}, sw[3] = {512, 512, 256}, sh[3] = {256, 256, 256};
  have = {false, false, false};
  for (int k = 0; k < 3; ++k) {
    const int w = s.render_w[k], h = s.render_h[k];
    if (s.render_rgba[k].empty() || w <= 0 || h <= 0 || s.render_rgba[k].size() < size_t(w) * h * 4) continue;
    const int dw = sd ? sw[k] : hw[k], dh = sd ? sh[k] : hh[k];
    r[size_t(k)] = io::BpeIdentity(bc3::Dds(bc3::Resize(s.render_rgba[k], w, h, dw, dh), dw, dh));
    have[size_t(k)] = true;
  }
}

// Session 101 (review S4): <mods>\superstars\build.lock, created exclusively (fails when it exists), removed when the build ends. One
// older than 10 minutes is a build that died (a full pack takes ~14 s on the PC, about a minute on a phone): taken over.
struct BuildLock {
  fs::path file;
  bool ok = false;
  std::string why;
  explicit BuildLock(const fs::path& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    file = root / "build.lock";
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
#ifdef _WIN32
      FILE* f = _wfopen(file.c_str(), L"wx");
#else
      FILE* f = std::fopen(file.c_str(), "wx");
#endif
      if (f) {
        std::fprintf(f, "superstars build\n");
        std::fclose(f);
        ok = true;
        break;
      }
      std::error_code e2;
      const auto t = fs::last_write_time(file, e2);
      const bool stale = !e2 && fs::file_time_type::clock::now() - t > std::chrono::minutes(10);
      if (attempt == 0 && stale) fs::remove(file, e2);  // (a build that died: taken over once)
      else break;
    }
    if (!ok) why = "another build of your superstars is running (the importer or the launcher): try again when it has finished";
  }
  ~BuildLock() {
    std::error_code ec;
    if (ok) fs::remove(file, ec);
  }
  BuildLock(const BuildLock&) = delete;
  BuildLock& operator=(const BuildLock&) = delete;
};

uint64_t FileTimeNow() {
  using namespace std::chrono;
  const auto unix100ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
  return uint64_t(unix100ns) + 116444736000000000ull;
}

}  // namespace

const std::vector<std::string>& LastBuildLog() { return g_log; }
std::vector<std::string> LastBuildMoves() {
  std::lock_guard lock(g_log_mutex);
  return g_moves;
}

// ------------------------------------------------------------------ sources
SourceInfo InspectSource(const std::string& path) {
  std::vector<SourceInfo> all = InspectSources(path);
  for (SourceInfo& s : all)
    if (s.ok) return s;
  if (!all.empty()) return all.front();
  SourceInfo s;
  s.err = "no character pac found";
  return s;
}

static std::vector<SourceInfo> InspectSourcesImpl(const std::string& path) {
  std::vector<SourceInfo> out;
  const fs::path src = U8Path(path);
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) {
    SourceInfo s;
    s.err = "no such file";
    out.push_back(s);
    return out;
  }
  if (!IsZipFile(src)) {
    Bytes d;
    SourceInfo s;
    if (fs::file_size(src, ec) > assets::char_mods::kMaxPac) s.err = "not a character pac (too big)";
    else if (!ReadAll(src, d)) s.err = "read failed";
    else s = InspectBytes(std::move(d), PathU8(src.filename()));
    if (s.suggested_name.empty()) s.suggested_name = SuggestName(PathU8(src.filename()));
    out.push_back(s);
    return out;
  }
  std::vector<Member> members;
  std::string err;
  if (!ListZip(src, members, err)) {
    SourceInfo s;
    s.err = err;
    out.push_back(s);
    return out;
  }
  int seen = 0;
  uint64_t total = 0;
  for (const Member& m : members) {
    if (m.name.size() < 4 || Lower(m.name.substr(m.name.size() - 4)) != ".pac") continue;  // only .pac members are looked at
    SourceInfo s;
    s.entry_name = m.name;
    s.suggested_name = SuggestName(m.name);
    if (++seen > assets::char_mods::kMaxZipPacs) {
      s.err = "the zip holds more than " + std::to_string(assets::char_mods::kMaxZipPacs) + " pac files: the rest was not read";
      out.push_back(s);
      break;
    }
    if (total + m.usize > assets::char_mods::kMaxZipTotal) {
      s.err = "the zip unpacks to more than 2 GB of pac files: the rest was not read";
      out.push_back(s);
      break;
    }
    total += m.usize;
    Bytes d;
    s.err = ReadMember(src, m, d);
    if (s.err.empty()) {
      SourceInfo t = InspectBytes(std::move(d), m.name);
      t.entry_name = s.entry_name, t.suggested_name = s.suggested_name;
      s = t;
    }
    out.push_back(s);
  }
  if (out.empty()) {
    SourceInfo s;
    s.err = "no .pac file in the zip";
    out.push_back(s);
  }
  return out;
}

static std::string ExtractSourceImpl(const std::string& zip_path, const std::string& entry_name, const std::string& mods_dir, std::string* err) {
  std::string e;
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return std::string();
  };
  if (mods_dir.empty()) return fail("no mods folder");
  const fs::path src = U8Path(zip_path), dir = U8Path(mods_dir) / "superstars" / "sources";
  std::error_code ec;
  Bytes d;
  std::string label = PathU8(src.filename());
  if (entry_name.empty() || !IsZipFile(src)) {
    if (fs::file_size(src, ec) > assets::char_mods::kMaxPac || !ReadAll(src, d)) return fail("cannot read the file");
  } else {
    std::vector<Member> members;
    if (!ListZip(src, members, e)) return fail(e);
    const Member* m = nullptr;
    for (const Member& x : members)
      if (x.name == entry_name) m = &x;
    if (!m) return fail("no " + entry_name + " in the zip");
    e = ReadMember(src, *m, d);
    if (!e.empty()) return fail(e);
    label = entry_name;
  }
  ModPac mp;
  e = ParseModPac(d, mp, nullptr);
  if (!e.empty()) return fail(e);
  std::string safe = assets::char_mods::SafeName(label);
  if (safe.empty()) safe = "character";
  fs::create_directories(dir, ec);
  fs::path dst = dir / U8Path(safe + ".pac");
  for (int k = 2; fs::exists(dst, ec); ++k) {  // never over another source (same bytes: reuse it)
    Bytes old;
    if (ReadAll(dst, old) && old == d) return PathU8(dst);
    dst = dir / U8Path(safe + " (" + std::to_string(k) + ").pac");
  }
  if (!WriteAll(dst, d)) return fail("could not write into the mods folder");
  return PathU8(dst);
}

// The ids in 397..499 the player's own packs use (every pack in the DLC folder, on or off; and the top pack's MUSIC ids). Session 101
// (review S5): a slot is MOVED by a build only when one of these uses it; kReservedSlots only keeps new imports off 399.
static std::set<int> UsedByPacks(const std::string& dlc_dir) {
  std::set<int> used;
  std::error_code ec;
  if (!dlc_dir.empty() && fs::is_directory(U8Path(dlc_dir), ec))
  for (const wwe2k14::dlc::Pack& p : wwe2k14::dlc::Scan(U8Path(dlc_dir), false, false)) {
    if (p.status != wwe2k14::dlc::Status::kReady) continue;
    bool ours = false;
    // (a pack of ours in the DLC folder) S110: one type-21 entry whose bundle rows hold slots >= kFirstSlot. The old form, one type-1
    // entry per character, is read only for packs built before this change, so a rebuild still finds them as ours.
    for (const auto& e : p.entries) ours = ours || (e.pack >= 6 && e.type == 1 && e.char_id >= kFirstSlot);
    for (const auto& e : p.entries)
      if (e.pack >= 6 && e.type == 21)
        for (int c : p.bundle_chars) ours = ours || c >= kFirstSlot;
    if (ours && p.pack_id >= 6) continue;
    for (const auto& e : p.entries) used.insert(e.char_id);
    for (int c : p.char_ids) used.insert(c);
  }
  // (review) the game's own MUSIC list (/ENEX/LSD list 2 in the highest pack's DLC pac: s16 music id per record,
  // docs\systems\music.md): a real theme id in 397..499 would be remapped for an imported slot, so such a slot is not free.
  // Jarrod's files: 134 records, none in that range (ids up to 904, plus 9998 / 9999).
  {
    const std::vector<RealPack> packs = RealPacks(dlc_dir, {});
    for (auto it = packs.rbegin(); it != packs.rend(); ++it) {
      const std::string f = "pac\\DLC_dlc" + it->Pp() + ".pac";
      Bytes d;
      io::Pac p;
      std::string e;
      if (!it->Has(f) || !it->Read(f, d, 64 << 20) || !io::ReadPac(d, p, e)) continue;
      const io::Entry* lsd = p.Find("ENEX", "LSD ");
      std::vector<io::Leaf> lists;
      if (!lsd || !io::ReadPach(io::Decode(lsd->blob), lists)) break;
      for (const io::Leaf& l : lists) {
        if (l.id != 2) continue;
        const Bytes m = io::Decode(l.raw);
        const size_t n = m.size() >= 2 ? io::L16(&m[0]) : 0;
        for (size_t i = 0; i < n && 2 + 8 * (i + 1) <= m.size(); ++i) {
          const int v = int16_t(io::L16(&m[2 + 8 * i]));
          if (v >= kFirstSlot && v <= kLastSlot) used.insert(v);
        }
      }
      break;  // (the highest pack's list is the cumulative one)
    }
  }
  return used;
}

static std::vector<int> FreeSlotsImpl(const launcher::GameFiles& game, const std::string& dlc_dir) {
  (void)game;
  // Session 101 (lane 199): the ids the real 2K14 packs use in 397..499 are never handed out, whether or not THIS player has those
  // packs (399: pack 4's "SuperStar Pack" entry, docs\systems\dlc.md item 0). Before, a player without pack 4 got 399 for his third
  // import; adding pack 4 later made every rebuild fail ("slot 399 is not free").
  std::set<int> used = UsedByPacks(dlc_dir);
  used.insert(std::begin(kReservedSlots), std::end(kReservedSlots));
  std::vector<int> out;
  for (int id = kFirstSlot; id <= kLastSlot; ++id)
    if (!used.count(id)) out.push_back(id);
  return out;
}

// S110 BANKID: the numeric names of every EVP / EVZ / GMEV / PCE6 directory in the files the game gets: the disc's entrance / winning / event
// pacs, gm.pac, prtcls.pac and the offered real packs' pacs of the same kinds (the dedicated bank numbers are the ones none of these names).
static std::set<int> UsedNames(Builder& b) {
  std::set<int> used;
  auto kind = [](const std::string& l) {  // l: lowercase, no folder: is it a pac that holds these directories?
    if (l.size() < 5 || l.compare(l.size() - 4, 4, ".pac") != 0) return false;
    for (const char* p : {"entrance", "winning", "season", "universe", "wsd", "gm", "prtcls"})
      if (l.rfind(p, 0) == 0) return true;
    return false;
  };
  Bytes d;
  for (const std::string& f : b.game.ListRoot("pac\\evt"))
    if (kind(Lower(f)) && b.ReadGame("pac\\evt\\" + f, d)) asset_ids::AddNames(d, used);
  for (const char* f : {"pac\\gm.pac", "pac\\prtcls.pac"})
    if (b.ReadGame(f, d)) asset_ids::AddNames(d, used);
  for (const RealPack& rp : b.packs)
    for (const io::CatFile& cf : rp.files) {
      const std::string l = Lower(cf.name);
      const bool evt = l.rfind("pac\\evt\\", 0) == 0 && l.find('\\', 8) == std::string::npos && kind(l.substr(8));
      const bool top = l.rfind("pac\\", 0) == 0 && l.find('\\', 4) == std::string::npos && (l.rfind("pac\\gm", 0) == 0 || l.rfind("pac\\prtcls", 0) == 0) && kind(l.substr(4));
      if ((evt || top) && rp.Read(cf.name, d)) asset_ids::AddNames(d, used);
    }
  return used;
}

// ------------------------------------------------------------------ the list: the library's cards (S110 SEAT-A)
// The library and the frame are included here, after everything they use (library.h, frame_pack.h, library_migrate.h are unity includes).
#include "library.h"
#include "frame_pack.h"
#include "library_migrate.h"
#include "host_alias.h"  // S110 LIBSIZE-2: the bind cases' host finder (test support)

namespace sc = wwe2k14::seat_card;

// Is a card not instantiated for the frame on disk? Every card has ONE file for the reference identity (seat_card.h kRefSeat / kRefBank, `file_form 2`) and rows.bin for
// the frame (inst.frame_sig); a seat change is no change (the game re-targets). A card in the old per-seat form, or one whose rows.bin has no relocation list, is not made.
// (cheap: the card files, the frame's digest and the rows.bin framing; no game file is read)
static bool CardsNeedBuild(const fs::path& root, const std::map<std::string, sc::Card>& cards) {
  std::error_code ec;
  if (cards.empty()) return false;
  frame::Info f;
  if (!frame::ReadInfo(root, f) || !fs::is_regular_file(frame::Dir(root) / "info" / "catalog.dlc", ec)) return true;
  for (const auto& [k, c] : cards) {
    if (!lib::InOneForm(c) || c.inst.frame_sig != f.sig) return true;
    if (c.seat >= 0 && !f.bank.count(c.seat)) return true;  // (a seat the frame does not hold: the build moves him)
    if (!lib::InstanceFilesThere(root, c)) return true;     // (review: a missing rows.bin / pac = the game holds him: make him again)
    if (lib::SourceChanged(root, c)) return true;            // (review: his source replaced under the same name)
  }
  return false;
}

// portraits = false (review N3): the list without the PNG portraits, for the check before every PLAY (cheap).
static bool LoadPackImpl(const std::string& mods_dir, PackState& out, std::string& err, bool portraits = true) {
  out = {};
  const fs::path root = U8Path(mods_dir) / "superstars";
  if (!lib::LibraryExists(root)) {
    if (migrate::Legacy(root)) return migrate::LoadLegacy(root, out, portraits, err);  // an old pack: its list; the next build migrates it
    return true;
  }
  std::map<std::string, sc::Card> cards;
  if (!lib::ReadAllCards(root, cards, err, &out.problems)) return false;  // (review: a folder that is not a readable card is listed, the rest load)
  for (const auto& [key, c] : cards) {
    Superstar s;
    lib::FillSuperstar(root, c, s, portraits);
    out.stars.push_back(std::move(s));
  }
  out.retired = lib::ReadRetired(root);
  out.enabled = lib::ReadEnabled(root);
  out.needs_build = CardsNeedBuild(root, cards) || migrate::Legacy(root);
  return true;
}

static bool SavePackImpl(const std::string& mods_dir, PackState& st, std::string& err) {
  if (mods_dir.empty()) return err = "no mods folder", false;
  const fs::path root = U8Path(mods_dir) / "superstars";
  // (no cap: the library holds any number of cards; the seats are given out by BuildPack, a card past them is stored unseated)
  std::vector<std::string> keys;
  if (!lib::SaveCards(root, st, &keys, err)) return false;
  for (size_t i = 0; i < st.stars.size() && i < keys.size(); ++i) st.stars[i].key = keys[i];  // (S110 SEAT-C review: the caller's list keeps them)
  return true;
}

// ------------------------------------------------------------------ BuildPack
// S110 SEAT-A: the library + frame build. What it does now (the old one rebuilt ONE pack holding every character):
//   * the cards are written (only what changed: lib::SaveCards), seats are given out;
//   * the FRAME is built only when it does not fit the DLC the game gets (frame::Fresh);
//   * each seated card whose files were not made for his seat / bank / the frame gets his folder written (lib::Instantiate), nobody else.
// An old pack (pack.json + pack6\) migrates on the way: the frame is built aside, every card is read back and compared with the old pack
// (migrate::Verify), and only then is the old pack moved aside and the frame put in place.
static bool BuildPackImpl(const std::string& mods_dir, const launcher::GameFiles& game_in, const std::string& dlc_dir, const PackState& st_in,
               std::function<void(float, const char*)> progress, std::string& err, const std::string& dlc_off) {
  std::lock_guard lock(g_log_mutex);
  g_log.clear(), g_moves.clear();
  Builder b;
  b.log = &g_log;
  b.progress = std::move(progress);
  if (mods_dir.empty()) return err = "no mods folder", false;
  const fs::path root = U8Path(mods_dir) / "superstars";
  // Session 101 (review S4): one build at a time across processes: an exclusive lock file, released on every return.
  BuildLock held(root);
  if (!held.ok) return err = held.why, false;
  b.game.SetRoot(game_in.root());
  if (!b.game.image_error().empty()) return err = "game files: " + b.game.image_error(), false;
  std::error_code ec;
  // Session 101 (lane 199): built on the packs the game gets (USE MY DLC, the switched-off ones, packages: pack_fit.h GamePacks)
  b.packs = RealPacks(dlc_dir, root, dlc_off);
  for (const RealPack& p : b.packs)
    if (p.Roster()) b.top = &p;
  PackState st = st_in;
  for (Superstar& s : st.stars) s.source_pac = ResolveSource(root, s.source_pac);
  int max_real = 0;
  for (const RealPack& p : b.packs) max_real = std::max(max_real, p.id);
  b.pack_id = std::max(6, max_real + 1);
  char ppb[8];
  std::snprintf(ppb, sizeof ppb, "%02d", b.pack_id);
  b.pp = ppb;
  frame::Info want;
  want.pack_id = b.pack_id;
  want.revision = uint16_t(b.top ? b.top->revision + 1 : 32);  // (no DLC: 32, the value proven in the game beside pack 5's 31)
  want.top_id = b.top ? b.top->id : 0, want.top_sig = b.top ? b.top->sig : 0;
  if (b.top) b.built_on.insert(b.top->id);
  b.Log("packs " + std::to_string(b.packs.size()) + ", cumulative files from " + b.BaseName() + (b.top ? " (" + PathU8(b.top->root) + ")" : std::string()) +
        ", our pack id " + std::to_string(b.pack_id) + " revision " + std::to_string(want.revision));
  const bool legacy = migrate::Legacy(root);  // an old pack still in place: this build migrates it
  if (const int n = migrate::RemoveHolds(root, U8Path(mods_dir)); n > 0)  // (review) the old pack kept since the migration: the game booted on the library
    b.Log("the old pack kept since the migration (legacy.old) is removed: the game has booted on the library");
  // (S110 SEAT-A review) a source pac that is gone is recovered from a pac already made for him (the old pack's model pac while it migrates, else
  // his own library pac): no character is lost because a source file was deleted after the import
  std::set<int> recovered;  // the seats whose source was recovered from the OLD pack's pac (migrate::Verify: the same model by another file name)
  {
    const wwe2k14::superstars_fit::Built old = legacy ? migrate::LegacyBuilt(root) : wwe2k14::superstars_fit::Built();
    char opp[8];
    std::snprintf(opp, sizeof opp, "%02d", old.pack_id > 0 ? old.pack_id : 6);
    for (Superstar& s : st.stars) {
      std::error_code e2;
      if (sc::ValidKey(s.key)) lib::RestoreOldFile(lib::CardDir(root, s.key));  // (S110 LIBSIZE-2 review) a build stopped between Publish's two renames: his old file back
      if (s.source_pac.empty() || fs::is_regular_file(U8Path(s.source_pac), e2)) continue;
      fs::path from;
      bool from_old = false;
      if (legacy && s.slot >= 0 && old.music.count(s.slot)) {
        from = root / "pack6" / "pac" / "ch" / ("ch" + std::to_string(s.slot) + "_dlc" + opp + ".pac");
        from_old = fs::is_regular_file(from, e2);
      }
      if (!from_old) from = lib::LibraryPacOf(root, s.key);
      if (from.empty() || !fs::is_regular_file(from, e2)) continue;  // (nothing to recover from: said when he is made)
      if (!from_old && Lower(PathU8(from)) == Lower(PathU8(lib::CharFile(root, s.key)))) {  // (S110 LIBSIZE-2) his own file in the one form is a source (lib::ModelOf)
        g_moves.push_back(s.name + ": his source file " + s.source_pac + " is gone; his model is taken from his own library file.");
        b.warnings.push_back(g_moves.back());
        s.source_pac = PathU8(from);
        continue;
      }
      std::string why;
      const std::string got = lib::RecoverSource(root, from, s.name, why);
      if (got.empty()) {
        b.warnings.push_back(s.name + ": his source pac " + s.source_pac + " is gone and could not be recovered: " + why);
        continue;
      }
      g_moves.push_back(s.name + ": his source file " + s.source_pac + " is gone; his model was recovered from " + (from_old ? "the old pack" : "his library folder") +
                        " into " + got + ".");
      b.warnings.push_back(g_moves.back());
      if (from_old) recovered.insert(s.slot);
      s.source_pac = got;
    }
  }
  fs::path stage = root / "frame.building";
  auto fail = [&](const std::string& why) {
    std::error_code e2;
    fs::remove_all(stage, e2);
    err = why;
    return false;
  };

  // ---- no characters: the frame (and an old pack) come out of the game
  if (st.stars.empty()) {
    std::error_code e2;
    std::string why;
    if (!lib::SaveCards(root, st, nullptr, why)) return fail(why);
    fs::remove_all(root / "frame.old", e2);
    fs::remove_all(stage, e2);
    if (legacy) {
      if (!migrate::Retire(root, why)) return fail(why);
      migrate::Drop(root);
    }
    fs::remove_all(frame::Dir(root), e2);
    if (e2) return err = "the old pack could not be removed (close the game first): " + e2.message(), false;
    fs::remove(wwe2k14::superstars_fit::StatusFile(U8Path(mods_dir)), e2);
    b.Log("no characters: the frame is removed (the game's own roster only)");
    return true;
  }

  // ---- check the characters
  // Session 101 (lane 199): a base / entrance wrestler from DLC the game does not get now - said plainly, with what to do. (S110 SEAT-A review)
  // It no longer stops the build: a SEATED character with such a wrestler is not made (his card keeps its seat and is not instantiated, so the
  // game holds him) and the sentence says why; an unseated one never blocks anything.
  std::vector<std::string> dlc_why(st.stars.size());
  {
    std::set<int> dlc_ids, offered_ids;  // every pack in the DLC folder (on or off) / the packs the game gets
    std::error_code e2;
    if (!dlc_dir.empty() && fs::is_directory(U8Path(dlc_dir), e2))
      for (const wwe2k14::dlc::Pack& p : wwe2k14::dlc::Scan(U8Path(dlc_dir), false, false)) {
        for (int c : p.char_ids) dlc_ids.insert(c);
        for (const auto& e : p.entries)
          if (e.type == 1 && e.char_id > 0) dlc_ids.insert(e.char_id);
      }
    for (const RealPack& p : b.packs) {
      for (const auto& e : p.entries)
        if (e.type == 1 && e.char_id > 0) offered_ids.insert(e.char_id);
      for (int c : dlc_ids)
        if (b.PackOfChar(c) == &p) offered_ids.insert(c);
    }
    for (size_t i = 0; i < st.stars.size(); ++i) {
      const Superstar& s = st.stars[i];
      for (int w : {s.base_id, s.entrance_from, s.tron_slot > 0 ? 0 : s.tron_from}) {  // (S110 TRONSLOT: the wrestler whose tron he uses too)
        if (w <= 0 || !dlc_ids.count(w) || offered_ids.count(w) || !dlc_why[i].empty()) continue;
        dlc_why[i] = s.name + " is not in the game: the " + (w == s.base_id ? "base" : w == s.entrance_from ? "entrance" : "tron") + " wrestler (" + std::to_string(w) +
                     ") comes with DLC the game does not get now. Switch that DLC on (USE MY DLC, and the pack itself), or pick "
                     "another wrestler in the Superstar Importer";
      }
    }
  }
  std::map<std::string, sc::Card> before;  // the cards as the library holds them now (their inst = what was built)
  if (!lib::ReadAllCards(root, before, err)) return false;
  std::vector<int> built_before;
  for (const auto& [k, c] : before)
    if (c.inst.valid && c.seat >= 0) built_before.push_back(c.seat);  // (S110 LIBSIZE-2: inst is the reference identity for every card: the seat he holds)
  if (legacy)
    for (const auto& kv : migrate::LegacyBuilt(root).music) built_before.push_back(kv.first);  // (the old pack's slots: a removed one is retired)
  // ---- the seats: 397..499 minus 399 minus the ids real packs use; the frame offers exactly these
  want.seats = FreeSlots(game_in, dlc_dir);
  // (S110 SEAT-A review) a reserved id (399: pack 4's bundle entry) is a seat ONLY for a character already on it while no pack in the DLC folder
  // uses it, as the old build kept him there (the reservation is for new imports); it is never handed to a new import or a mover (`order`)
  std::set<int> reserved_kept;
  {
    const std::set<int> pack_used = UsedByPacks(dlc_dir);
    for (int r : kReservedSlots)
      for (const Superstar& s : st.stars)
        if (s.slot == r && !pack_used.count(r) && reserved_kept.insert(r).second) want.seats.push_back(r);
  }
  std::sort(want.seats.begin(), want.seats.end());
  if (want.seats.empty()) return err = "no seat is free: your DLC packs use every id 397..499", false;
  {
    // Session 101 (lane 199; review S5): a character whose seat is not one of these (a pack added since uses it, it is used twice, it is 399 or
    // outside 397..499) is MOVED to a free seat, not refused; one with no seat takes a free one. A card past the seats stays unseated (S110 SEAT-A:
    // there is no cap, the library holds him). His id changes, so a save that used the old one shows someone else there; the player is told.
    std::vector<int> retired = st.retired;
    {
      const std::vector<int> disk = lib::ReadRetired(root);
      retired.insert(retired.end(), disk.begin(), disk.end());
      if (legacy) {
        PackState old;
        std::string e2;
        if (migrate::LoadLegacy(root, old, false, e2)) retired.insert(retired.end(), old.retired.begin(), old.retired.end());
      }
    }
    // S105 IMP: a character that has to move never takes a retired seat while a never-used one is free (a save's old row there)
    std::vector<int> handed;  // (the seats a mover / a new import may get: not a reserved id kept for its resident)
    for (int s : want.seats)
      if (!reserved_kept.count(s)) handed.push_back(s);
    const std::vector<int> order = importer::slots::NeverUsedFirst(handed, retired, built_before);
    const std::set<int> seat_set(want.seats.begin(), want.seats.end());
    std::set<int> kept;
    std::vector<size_t> move;
    for (size_t i = 0; i < st.stars.size(); ++i) {
      const int slot = st.stars[i].slot;
      if (slot >= 0 && seat_set.count(slot) && kept.insert(slot).second) continue;
      move.push_back(i);
    }
    for (size_t i : move) {
      int to = -1;
      for (int f : order)  // (never a seat another character keeps or was just given)
        if (!kept.count(f)) {
          to = f;
          break;
        }
      Superstar& s = st.stars[i];
      if (s.slot >= 0 && to >= 0) {
        b.Log("seat " + std::to_string(s.slot) + " of " + s.name + " is not free (a DLC pack uses it, or it was used twice): moved to " + std::to_string(to));
        g_moves.push_back(s.name + " moved from slot " + std::to_string(s.slot) + " to " + std::to_string(to) + " (saves that used him may show someone else).");
        b.warnings.push_back(g_moves.back());
      } else if (to >= 0) {
        b.Log(s.name + " had no seat: now seat " + std::to_string(to));
      } else {
        if (s.slot >= 0) {
          g_moves.push_back(s.name + " has no seat now (all " + std::to_string(want.seats.size()) + " are taken): kept in your library until one is free.");
          b.warnings.push_back(g_moves.back());
        }
        b.Log(s.name + ": no free seat, stored without one");
      }
      s.slot = to;
      if (to >= 0) kept.insert(to);
    }
  }
  std::set<int> slots;
  for (size_t i = 0; i < st.stars.size(); ++i) {
    const Superstar& s = st.stars[i];
    const std::string who = "character " + std::to_string(i + 1) + " (" + s.name + ")";
    if (s.slot >= 0 && !slots.insert(s.slot).second) return err = who + ": seat " + std::to_string(s.slot) + " used twice", false;
    if (s.name.empty()) return err = who + ": no name", false;
    if (s.base_id < kMinBase || s.base_id > kMaxBase || (s.entrance_from >= 0 && (s.entrance_from < kMinBase || s.entrance_from > kMaxBase)))
      return err = who + ": the base / entrance wrestler must be a roster wrestler (" + std::to_string(kMinBase) + ".." + std::to_string(kMaxBase) + ")", false;
    if (s.tron_from > 0 && (s.tron_from < kMinBase || s.tron_from > kMaxBase))  // S110 TRONSLOT (<= 0: none, as entrance_from's -1)
      return err = who + ": the wrestler whose tron he uses must be a roster wrestler (" + std::to_string(kMinBase) + ".." + std::to_string(kMaxBase) + ")", false;
    if (s.tron_slot > 0 && !wwe2k14::tron_slots_core::IsSlotId(s.tron_slot))
      return err = who + ": custom tron " + std::to_string(s.tron_slot) + " is not a custom tron number (823-843, 845-899, 910-949)", false;
    // (a source pac is read only for a card that is made: Instantiate reads and checks it)
  }
  // ---- the library holds the list (a card written only where it changed)
  b.Prog(0.02f, "saving the library");
  std::vector<std::string> keys;
  if (!lib::SaveCards(root, st, &keys, err)) return false;
  for (size_t i = 0; i < st.stars.size(); ++i) st.stars[i].key = keys[i];
  std::map<std::string, sc::Card> cards;
  std::vector<std::string> card_problems;  // (review) folders that are not readable cards: left alone, said
  if (!lib::ReadAllCards(root, cards, err, &card_problems)) return false;
  // ---- the frame: kept while it fits the DLC the game gets, else built aside (frame.building)
  frame::Info stored;
  lib::Ctx ctx(b);
  want.sig = frame::Sig(want);
  const bool fresh = frame::Fresh(root, want, stored);
  if (fresh) {
    want = stored;  // (the numbers it holds stay: a card made for them stays valid)
    asset_ids::Config().assigned = want.bank;
  }
  // ---- what each card needs (S110 LIBSIZE-2: every card is made ONCE for the reference identity, so the game binds him to any seat; a frame change makes his rows only)
  std::vector<size_t> todo;
  std::vector<std::string> problems = card_problems;  // (review) one sentence per character not made this time (LastBuildMoves): the others are made
  for (size_t i = 0; i < st.stars.size(); ++i) {
    const Superstar& s = st.stars[i];
    sc::Card& c = cards.at(s.key);
    if (!dlc_why[i].empty()) {  // (review) not made; an instance made before is withdrawn, so the game holds him instead of seating him without that DLC
      problems.push_back(dlc_why[i]);
      if (c.inst.valid) {
        sc::Card w = c;
        w.inst = {};
        std::string e2;
        if (sc::WriteCard(lib::SeatFile(lib::CardDir(root, c.key) / "card.txt"), w, &e2)) c.inst = {};
      }
      continue;
    }
    const bool ok = fresh && lib::InOneForm(c) && c.inst.frame_sig == want.sig && (c.seat < 0 || want.bank.count(c.seat)) &&
                    lib::InstanceFilesThere(root, c) &&  // (review: a folder whose rows.bin / pac went missing is made again)
                    !lib::SourceChanged(root, c);        // (review: a source replaced under the same name: size / time differ)
    if (!ok) todo.push_back(i);
  }
  // ---- free space: the sources and the renders of each card to make, and the frame's copies
  {
    uint64_t need = 0;
    std::error_code e3;
    for (size_t i : todo) {  // (a card whose file is made: his source's size + the renders; a card made again for a new frame writes a few KB)
      const sc::Card& c = cards.at(st.stars[i].key);
      if (!c.inst.valid || !lib::InstanceFilesThere(root, c)) need += fs::file_size(U8Path(st.stars[i].source_pac), e3) * (e3 ? 0 : 1) + (4ull << 20);
    }
    if (!fresh) need += 64ull << 20;
    need += need / 10;
    std::error_code e2;
    fs::create_directories(root, e2);
    const fs::space_info sp = fs::space(root, e2);
    if (!e2 && sp.available < need)
      return err = "not enough free space for the pack: " + std::to_string(need >> 20) + " MB needed, " + std::to_string(sp.available >> 20) + " MB free", false;
  }
  if (!fresh) {
    b.Prog(0.05f, "choosing the entrance numbers");
    wwe2k14::superstars_fit::Built recorded;
    {
      frame::Info old;
      if (frame::ReadInfo(root, old)) recorded = wwe2k14::superstars_fit::ReadBuilt(frame::Dir(root) / "superstars.txt");
      else if (legacy) recorded = migrate::LegacyBuilt(root);
    }
    if (!frame::Plan(b, ctx, recorded, want, err)) return false;
    want.sig = frame::Sig(want);
    b.Prog(0.15f, "writing the frame");
    fs::remove_all(stage, ec);
    if (!frame::Write(b, stage, want, err)) return fail(err);
  }
  // ---- the characters
  if (!ctx.LoadChEtc(err)) return fail(err);
  std::vector<lib::FilePlan> plans(todo.size());  // (S110 LIBSIZE-2) per card: is his FILE stale (its file_sig), or only his rows
  {
    std::vector<const Superstar*> prime;
    for (size_t n = 0; n < todo.size(); ++n) {
      const Superstar& s = st.stars[todo[n]];
      plans[n] = lib::PlanFile(ctx, root, cards.at(s.key), s);
      if (plans[n].stale) prime.push_back(&s);
    }
    b.Prog(0.20f, "reading the entrances");
    ctx.PrimeFor(prime);
  }
  // (S110 SEAT-A review: the order) each character's new files are made into his folder's stage\ first; his card, rows.bin and pac are not
  // touched until the frame is committed (Publish, below). One character that cannot be made does not stop the others: he keeps his card and
  // seat, not instantiated for this frame, so the game holds him, and the player is told why.
  const wwe2k14::superstars_fit::Built old_built = legacy ? migrate::LegacyBuilt(root) : wwe2k14::superstars_fit::Built();
  struct Made {
    sc::Card card;  // with the new inst (for the checks); Publish writes it last
    sc::Inst inst;
  };
  std::vector<Made> made;
  size_t n_made = 0;
  for (size_t n = 0; n < todo.size(); ++n) {
    Superstar s = st.stars[todo[n]];
    const int card_slot = s.slot;  // (Instantiate makes him for the reference identity: s.slot is put back below)
    const lib::FilePlan& plan = plans[n];
    b.Prog(0.25f + 0.65f * float(n) / float(todo.size()), "making the characters");
    sc::SeatRows rows;
    io::Pac pac;
    std::string why;
    if (!lib::Instantiate(ctx, s, ctx.base_rank + 1, plan.stale, plan.own, lib::CharFile(root, s.key), rows, pac, why)) {
      std::error_code e2;
      if (why.rfind(s.name + ": ", 0) == 0) why = why.substr(s.name.size() + 2);  // (some reasons name him already)
      // (while an old pack migrates: a character whose only model is in the old pack is never left behind; the migration stops, the old pack stays)
      if (legacy && card_slot >= 0 && old_built.music.count(card_slot) && !fs::is_regular_file(U8Path(s.source_pac), e2))
        return fail("migration stopped, your old pack is untouched: " + s.name + ": " + why);
      problems.push_back(s.name + " is not in the game: " + why);
      continue;
    }
    sc::Card c = cards.at(s.key);
    sc::Inst inst;
    inst.valid = true, inst.seat = sc::kRefSeat, inst.bank = sc::kRefBank, inst.src_bank = s.src_bank;
    inst.music = s.entrance_from >= 0 ? s.entrance_from : s.base_id, inst.frame_sig = want.sig, inst.movie = s.movie;
    {  // the packs his base / entrance / tron wrestlers' files come from (a launcher check: the game must still get them)
      std::set<int> on;
      for (int w : {s.base_id, s.entrance_from >= 0 ? s.entrance_from : s.base_id, s.tron_slot > 0 ? 0 : s.tron_from})
        if (w > 0)
          if (const RealPack* p = b.PackOfChar(w)) on.insert(p->id);
      std::string v;
      for (int id : on) v += (v.empty() ? "" : " ") + std::to_string(id);
      lib::SetExtra(c, "built_on", v);
    }
    lib::StampFile(c, plan.model_sig, plan.sig);  // `file_form 2`, `file_sig`, `model_sig` (Publish puts his own file as his source and drops source_stat)
    if (!lib::WriteStage(root, c.key, rows, plan.stale ? &pac : nullptr, why)) return fail(s.name + ": " + why);
    c.inst = inst;
    if (const std::string w = lib::CheckInstance(lib::StageDir(root, c.key), plan.stale ? Under(lib::StageDir(root, c.key), lib::CharRel()) : lib::CharFile(root, c.key), plan.stale, c); !w.empty())
      return fail(s.name + ": " + w);
    // (S110 LIBSIZE-2 review) his new file, read back, holds his source whole: after Publish it is his only copy (his source becomes his own file, the sources\ copy goes)
    if (plan.stale)
      if (const std::string w = lib::HoldsSource(U8Path(s.source_pac), plan.own, Under(lib::StageDir(root, c.key), lib::CharRel())); !w.empty())
        return fail(s.name + ": his new file does not hold his source: " + w);
    made.push_back({std::move(c), inst});
    s.slot = card_slot;
    st.stars[todo[n]] = s;  // (src_bank / movie)
  }
  // ---- read everything back (migration step 4); nothing of an old pack is touched before this passes
  b.Prog(0.92f, "checking");
  if (legacy) {
    std::vector<sc::Card> mc;
    for (const Made& m : made) mc.push_back(m.card);
    int checked = 0, skipped = 0;
    if (const std::string why = migrate::Verify(root, mc, want, b.pp, old_built, checked, &skipped, &recovered); !why.empty())
      return fail("migration stopped, your old pack is untouched: " + why);
    b.Log("migration: " + std::to_string(checked) + " of " + std::to_string(made.size()) + " characters compared with the old pack: rows, names and models equal" +
          (skipped ? " (" + std::to_string(skipped) + " changed since the old pack was built: not compared)" : std::string()));
  }
  // ---- commit: the old pack aside, the frame in place, then each character's new files and card, then the old pack's folder kept aside
  b.Prog(0.96f, "finishing");
  if (legacy) {
    std::string why;
    if (!migrate::Retire(root, why)) return fail(why);
  }
  if (!fresh) {
    std::string why;
    if (!frame::Commit(root, why)) {
      if (legacy) migrate::Restore(root);
      return fail(why);
    }
  }
  for (Made& m : made) {  // (a failure here leaves that card without an inst: held, and made again by the next build)
    std::string why;
    if (lib::Publish(root, m.card, m.inst, why)) ++n_made;
    else problems.push_back(m.card.name + " is not in the game: " + why);
  }
  // (S110 LIBSIZE-2) his own file is his source now: a sources\ file no card names any more goes (the inbox is empty after the build). Never while a folder of the
  // library could not be read as a card (it may name one).
  if (card_problems.empty()) {
    std::map<std::string, sc::Card> after;
    std::string e5;
    auto norm = [](const fs::path& p) {
      std::string t = Lower(PathU8(p.lexically_normal()));
      for (char& ch : t)
        if (ch == '/') ch = '\\';
      return t;
    };
    if (lib::ReadAllCards(root, after, e5)) {
      // (S110 LIBSIZE-2 review) only a file a card named when this build began goes: a file no card ever named (a pick the importer has not added to its list yet;
      // on a phone its only copy) is not the build's to delete
      std::set<std::string> named, was_named;
      for (const auto& [k, c] : after)
        if (!c.source.empty()) named.insert(norm(lib::SourceOf(root, c)));
      for (const std::map<std::string, sc::Card>* m : {&before, &cards})
        for (const auto& [k, c] : *m)
          if (!c.source.empty()) was_named.insert(norm(lib::SourceOf(root, c)));
      std::vector<fs::path> gone;
      for (const auto& f : fs::directory_iterator(root / "sources", ec))
        if (f.is_regular_file(ec) && was_named.count(norm(f.path())) && !named.count(norm(f.path()))) gone.push_back(f.path());
      for (const fs::path& f : gone) fs::remove(f, ec);
      if (!gone.empty()) b.Log(std::to_string(gone.size()) + " source file(s) in sources\\ that no card names any more removed");
    }
  }
  if (legacy) migrate::Drop(root);
  fs::remove(wwe2k14::superstars_fit::StatusFile(U8Path(mods_dir)), ec);  // (Session 101: the game's old verdict is about the old pack)
  for (const std::string& p : problems) g_moves.push_back(p), b.warnings.push_back(p);
  b.Log(std::to_string(n_made) + " character(s) made, " + std::to_string(problems.size()) + " not made, " + std::to_string(st.stars.size() - todo.size()) + " kept" +
        (fresh ? "; the frame was kept" : "; the frame was built"));
  for (const std::string& w : b.warnings) b.Log("warning: " + w);
  b.Prog(1.f, "done");
  return true;
}


// ------------------------------------------------------------------ the public calls: nothing throws out of them (review: the UI thread)
std::vector<SourceInfo> InspectSources(const std::string& path) {
  try {
    return InspectSourcesImpl(path);
  } catch (const std::exception& e) {
    SourceInfo s;
    s.err = std::string("could not read the file: ") + e.what();
    return {s};
  }
}
std::string ExtractSource(const std::string& zip_path, const std::string& entry_name, const std::string& mods_dir, std::string* err) {
  try {
    return ExtractSourceImpl(zip_path, entry_name, mods_dir, err);
  } catch (const std::exception& e) {
    if (err) *err = std::string("could not copy it: ") + e.what();
    return {};
  }
}
bool LoadPack(const std::string& mods_dir, PackState& out, std::string& err) {
  try {
    return LoadPackImpl(mods_dir, out, err);
  } catch (const std::exception& e) {
    out = {};
    return err = std::string("could not read the pack list: ") + e.what(), false;
  }
}
bool SavePack(const std::string& mods_dir, PackState& st, std::string& err) {
  try {
    return SavePackImpl(mods_dir, st, err);
  } catch (const std::exception& e) {
    return err = std::string("could not save the pack list: ") + e.what(), false;
  }
}
std::vector<int> BuiltPackSlots(const std::string& mods_dir) {  // S105 IMP review M1
  try {
    if (mods_dir.empty()) return {};
    const fs::path root = U8Path(mods_dir) / "superstars";
    std::vector<int> v;
    if (lib::LibraryExists(root)) {  // the seats his files were made for (a card with an `inst`)
      std::map<std::string, sc::Card> cards;
      std::string e;
      if (lib::ReadAllCards(root, cards, e))
        for (const auto& [k, c] : cards)
          if (c.inst.valid && c.seat >= 0) v.push_back(c.seat);  // (S110 LIBSIZE-2 review: inst is the reference identity for every card: the seat he holds)
    }
    // (an old pack not migrated yet, also once a save made the library: its slots were built; the file goes with the migration)
    for (const auto& kv : wwe2k14::superstars_fit::ReadBuilt(root / "superstars.txt").music) v.push_back(kv.first);
    std::sort(v.begin(), v.end()), v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  } catch (...) {
  }
  return {};
}
std::vector<int> RetiredSlots(const std::string& mods_dir) {  // S105 IMP
  try {
    if (mods_dir.empty()) return {};
    const fs::path root = U8Path(mods_dir) / "superstars";
    if (lib::LibraryExists(root)) return lib::ReadRetired(root);
    PackState old;
    std::string e;
    if (migrate::LoadLegacy(root, old, false, e)) return old.retired;
  } catch (...) {
  }
  return {};
}
int SeatCount(const std::string& mods_dir) {
  try {
    frame::Info f;
    if (!mods_dir.empty() && frame::ReadInfo(U8Path(mods_dir) / "superstars", f)) return int(f.seats.size());
  } catch (...) {
  }
  return kLastSlot - kFirstSlot + 1 - int(sizeof kReservedSlots / sizeof kReservedSlots[0]);  // (no frame built yet: 397..499 minus 399)
}
Migration MigrationState(const std::string& mods_dir) {
  try {
    if (mods_dir.empty()) return Migration::None;
    const fs::path root = U8Path(mods_dir) / "superstars";
    std::error_code ec;
    if (migrate::Legacy(root)) return Migration::Pending;
    if (fs::is_directory(root / "legacy.old", ec)) return Migration::Kept;
  } catch (...) {
  }
  return Migration::None;
}
std::vector<int> FreeSlots(const launcher::GameFiles& game, const std::string& dlc_dir) {
  try {
    return FreeSlotsImpl(game, dlc_dir);
  } catch (const std::exception&) {
    return {};
  }
}
// Session 101 (lane 199): the game's own rule (pack_fit.h Problem, which src\dlc.h applies at boot) on the packs the game will get.
std::string PackProblem(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off) {
  try {
    if (mods_dir.empty()) return {};
    return wwe2k14::superstars_fit::ProblemFor(U8Path(mods_dir), dlc_dir.empty() ? fs::path() : U8Path(dlc_dir), dlc_off);
  } catch (...) {
    return {};
  }
}
bool PackOutOfDate(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off) {
  return !PackProblem(mods_dir, dlc_dir, dlc_off).empty();
}

PlayCheck CheckBeforePlay(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off) {
  PlayCheck c;
  try {
    PackState st;
    std::string err;
    if (mods_dir.empty()) return c;
    if (!LoadPackImpl(mods_dir, st, err, /*portraits=*/false))  // (review N3: no PNG decodes before every PLAY)
      return c.why = "Your superstars list could not be read (" + err + "): open the Superstar Importer.", c;
    c.stars = int(st.stars.size());
    const fs::path root = U8Path(mods_dir) / "superstars";
    std::error_code ec;
    const bool frame_there = fs::is_regular_file(frame::Dir(root) / "info" / "catalog.dlc", ec) && fs::is_regular_file(frame::Dir(root) / "superstars.txt", ec);
    const bool old_pack = migrate::Legacy(root) && (fs::is_regular_file(root / "pack6" / "info" / "catalog.dlc", ec) || fs::is_regular_file(root / "pack.json", ec));
    if (!c.stars) {  // (every superstar removed: the frame / the old pack, still holding them, comes out with a build)
      // (review S6) only when the library or pack.json says so: a pack with no list (copied in by hand) is never removed
      // (review: an old pack6\ only when pack.json exists, the old rule: pack6\ holds the characters' models)
      if (frame_there || (old_pack && fs::is_regular_file(root / "pack.json", ec) && fs::is_regular_file(root / "pack6" / "info" / "catalog.dlc", ec)))
        c.build = true, c.why = "You removed every superstar, but the old pack is still in the game.";
      return c;
    }
    if (old_pack) {
      c.build = true, c.why = "Your superstars move into the new library: they are built once more (nothing is lost).";
    } else if (!frame_there) {
      c.build = true, c.why = "Your superstars are not built into the game yet.";
    } else if (st.needs_build) {
      c.build = true, c.why = "Your superstars list changed since the pack was last built.";
    } else if (const std::string p = PackProblem(mods_dir, dlc_dir, dlc_off); !p.empty()) {
      c.build = true, c.why = "Your superstars pack no longer fits your DLC: " + p + ".";
    } else {
      // a card made on a DLC pack the game does not get now (its base / entrance / tron wrestler's files)
      std::map<std::string, sc::Card> cards;
      std::string e;
      std::set<int> offered;
      for (const wwe2k14::dlc::Pack& p : wwe2k14::superstars_fit::GamePacks(dlc_dir.empty() ? fs::path() : U8Path(dlc_dir), dlc_off)) offered.insert(p.pack_id);
      if (lib::ReadAllCards(root, cards, e))
        for (const auto& [k, card] : cards)
          for (const auto& kv : card.extra)
            if (kv.first == "built_on") {
              std::istringstream in(kv.second);
              for (int id; in >> id;)
                if (!offered.count(id) && !c.build) c.build = true, c.why = card.name + " was made with DLC pack " + std::to_string(id) + ", which the game does not get now.";
            }
    }
    if (!c.build && !st.problems.empty())  // (review) a library folder left out: said (the launchers show `why` when there is nothing to build)
      c.why = "Your superstars library has " + std::to_string(st.problems.size()) + " folder(s) it cannot use. " + st.problems.front();  // (SEAT-C: the sentence ends itself)
  } catch (const std::exception& e) {
    c.why = std::string("Your superstars could not be checked: ") + e.what();
  }
  return c;
}

std::string LastGameProblem(const std::string& mods_dir) {
  try {
    if (mods_dir.empty()) return {};
    // S110 SEAT-C: the game now writes "in N, M ... need a rebuild", "out N ... need converting" too (screen_text.h words all of them)
    return screen::GameStatusSentence(wwe2k14::superstars_fit::ReadStatus(U8Path(mods_dir)));
  } catch (...) {
    return {};
  }
}

int HostPackOfChar(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const std::string& dlc_off, int id) {
  try {
    return HostPackOfCharImpl(mods_dir, game, dlc_dir, dlc_off, id);
  } catch (...) {
    return -1;
  }
}
std::vector<HostPac> HostGamePacs(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const std::string& dlc_off, bool sd) {
  try {
    return HostGamePacsImpl(mods_dir, game, dlc_dir, dlc_off, sd);
  } catch (...) {
    return {};
  }
}
std::string FileHoldsSource(const std::string& source_pac, bool own, const std::string& file) {
  try {
    return lib::HoldsSource(U8Path(source_pac), own, U8Path(file));
  } catch (const std::exception& e) {
    return std::string("the check failed: ") + e.what();
  }
}

bool BuildPack(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const PackState& st,
               std::function<void(float, const char*)> progress, std::string& err, const std::string& dlc_off) {
  try {
    return BuildPackImpl(mods_dir, game, dlc_dir, st, std::move(progress), err, dlc_off);
  } catch (const std::exception& e) {
    std::error_code ec;
    fs::remove_all(U8Path(mods_dir) / "superstars" / "frame.building", ec);
    return err = std::string("the build stopped: ") + e.what(), false;
  }
}

}  // namespace importer