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
#include "../xctd_file.h"
#include "bc3.h"
#include "pack_fit.h"  // Session 101: the game's DLC view + the "does the pack fit" rule (shared with src\dlc.h and the launchers)
#include "pack_io.h"
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
std::string Esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o;
}

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
bool PatchChEtc(Builder& b, io::Pac& etc, const std::vector<Superstar>& stars, std::string& err) {
  io::Entry* dat = etc.Find("CHAR", "DAT ");
  io::Entry* dsbd = etc.Find("CHAR", "DSBD");
  io::Entry* pro = etc.Find("CHAR", "PRO ");
  std::vector<io::Leaf> ldat, ldsbd, lpro;
  if (!dat || !dsbd || !pro || !io::ReadPach(dat->blob, ldat) || !io::ReadPach(dsbd->blob, ldsbd) || !io::ReadPach(pro->blob, lpro))
    return err = "the chEtc of " + b.BaseName() + " is not as expected (CHAR DAT / DSBD / PRO)", false;
  auto find = [](std::vector<io::Leaf>& v, uint32_t id) -> io::Leaf* {
    for (auto& l : v)
      if (l.id == id) return &l;
    return nullptr;
  };
  // rank (+2): unique among the active rows 100..499 (pack 5: 1..140)
  int rank = 0;
  std::map<uint32_t, Bytes> rows;
  for (const io::Leaf& l : ldat) {
    Bytes r = io::Decode(l.raw);
    if (r.size() == 208 && l.id >= 100 && l.id < 500 && r[kRecHead + 16] == 1) rank = std::max<int>(rank, io::L16(&r[kRecHead + 2]));
    rows[l.id] = std::move(r);
  }
  for (const Superstar& s : stars) {
    const uint32_t src = uint32_t(s.base_id), slot = uint32_t(s.slot);
    io::Leaf *ld = find(ldat, slot), *ls = find(ldsbd, src), *lsd = find(ldsbd, slot), *lp = find(lpro, src), *lpd = find(lpro, slot);
    const auto rit = rows.find(src);
    if (!ld || !ls || !lsd || !lp || !lpd || rit == rows.end() || rit->second.size() != 208 || io::L16(&rit->second[kRecHead]) != src)
      return err = "base wrestler " + std::to_string(src) + " (or slot " + std::to_string(slot) + ") has no usable roster row", false;
    if (!b.top && rit->second[kRecHead + 16] != 1)  // (no DLC: a DLC wrestler's row exists on the disc but he is not in this game)
      return err = "base wrestler " + std::to_string(src) + " comes with DLC the game does not get now (USE MY DLC is off, or you have no "
                   "DLC): switch USE MY DLC on, or pick another base wrestler in the Superstar Importer", false;
    const uint16_t att_lo = uint16_t(src * 100), att_hi = uint16_t(src * 100 + 99);
    auto att = [&](uint16_t v) { return uint16_t(v - att_lo + slot * 100); };
    Bytes rec = rit->second;
    uint8_t* R = &rec[kRecHead];
    for (int off : {0, 4, 200}) io::W16(R + off, uint16_t(slot));
    io::W16(R + 2, uint16_t(++rank));
    io::W16(R + 6, uint16_t(60000 + slot)), io::W16(R + 8, uint16_t(62000 + slot)), io::W16(R + 10, uint16_t(64000 + slot));
    io::W32(R + 12, 99999);  // no Twitter text (the base's would show his handle)
    R[16] = 1;                // roster active
    R[17] = uint8_t(b.pack_id);  // pack code = our catalog pack id (availability through the DLC manager)
    if (!s.copy_ratings) std::fill(R + 32, R + 46, uint8_t(75));
    std::string atts;
    for (int off = 116; off < 200; off += 2) {
      const uint16_t v = io::L16(R + off);
      if (v >= att_lo && v <= att_hi) io::W16(R + off, att(v)), atts += " " + std::to_string(att(v));
    }
    ld->raw = io::BpeIdentity(rec, VersionWord(ld->raw));
    // DSBD: raw 80-byte rows, the attire id list first
    Bytes ds = ls->raw;
    for (size_t off = 0; off + 1 < ds.size(); off += 2) {
      const uint16_t v = io::L16(&ds[off]);
      if (v >= att_lo && v <= att_hi) io::W16(&ds[off], att(v));
    }
    lsd->raw = ds;
    // PRO (1276 bytes): the character's own id in the u16 slots +576 .. +602 (384, 385, 393 and the blank 397 alike; disc
    // characters also hold their id at +150 / +152: left alone, meaning [U])
    Bytes pr = io::Decode(lp->raw);
    if (pr.size() < 604) return err = "base wrestler " + std::to_string(src) + ": profile row too short", false;
    int self = 0;
    for (size_t off = 576; off < 604; off += 2)
      if (io::L16(&pr[off]) == src) io::W16(&pr[off], uint16_t(slot)), ++self;
    lpd->raw = io::BpeIdentity(pr, VersionWord(lp->raw));
    b.Log("chEtc row " + std::to_string(slot) + " = row " + std::to_string(src) + ": rank " + std::to_string(rank) + ", texts " +
          std::to_string(60000 + slot) + "/" + std::to_string(62000 + slot) + "/" + std::to_string(64000 + slot) + ", pack code " +
          std::to_string(b.pack_id) + ", ratings " + (s.copy_ratings ? "copied" : "75") + ", attire ids" + atts + ", " +
          std::to_string(self) + " profile self ids");
  }
  dat->blob = io::WritePach(ldat), dsbd->blob = io::WritePach(ldsbd), pro->blob = io::WritePach(lpro);
  return true;
}

// ---- string pac: the name tables (every SDB entry whose ZLIB table holds text ids 60100..60999: one per language)
bool PatchStrings(Builder& b, io::Pac& str, const std::map<uint32_t, std::string>& texts, std::string& err) {
  io::Dir* sdb = str.FindDir("SDB ");
  if (!sdb) return err = "the string pac has no SDB directory", false;
  int tables = 0;
  for (io::Entry& e : sdb->ents) {
    std::vector<io::Leaf> leaves;
    if (!io::ReadPach(e.blob, leaves)) continue;
    bool changed = false;
    for (io::Leaf& l : leaves) {
      if (l.raw.size() < 18 || std::memcmp(l.raw.data(), "ZLIB", 4) != 0) continue;
      const Bytes d = io::Decode(l.raw);
      if (d.size() < 8 || io::L32(&d[0]) != 0) continue;
      const uint32_t cnt = io::L32(&d[4]);
      if (cnt > 200000 || 8 + 12ull * cnt > d.size()) continue;
      std::map<uint32_t, Bytes> recs;
      bool names = false, bad = false;
      for (uint32_t i = 0; i < cnt; ++i) {
        const uint8_t* r = &d[8 + 12 * size_t(i)];
        const uint32_t off = io::L32(r), len = io::L32(r + 4), id = io::L32(r + 8);
        if (uint64_t(off) + len > d.size()) { bad = true; break; }
        names = names || (id >= 60100 && id < 61000);
        recs[id].assign(d.begin() + off, d.begin() + off + len);
      }
      if (bad || !names) continue;
      for (const auto& [id, s] : texts) {
        Bytes v(s.begin(), s.end());
        v.push_back(0);
        recs[id] = v;
      }
      // rebuild: header, records sorted by id, strings in record order right after the table (the real tables' layout)
      Bytes t;
      io::P32(t, 0), io::P32(t, uint32_t(recs.size()));
      uint32_t off = uint32_t(8 + 12 * recs.size());
      for (const auto& [id, v] : recs) io::P32(t, off), io::P32(t, uint32_t(v.size())), io::P32(t, id), off += uint32_t(v.size());
      for (const auto& [id, v] : recs) t.insert(t.end(), v.begin(), v.end());
      l.raw = io::ZlibLeaf(t, io::L32(&l.raw[4]));
      changed = true;
      ++tables;
    }
    if (changed) e.blob = io::WritePach(leaves);
  }
  if (!tables) return err = "no name table found in the string pac", false;
  b.Log("string pac: " + std::to_string(texts.size()) + " texts written into " + std::to_string(tables) + " name tables (one per language)");
  return true;
}

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
std::map<int, BankSrc> FindBanks(Builder& b, const std::set<int>& wanted_chars) {
  std::set<std::string> want;
  for (int e : wanted_chars) {
    want.insert(std::to_string(e));
    for (int g = 0; g < 10; ++g) want.insert(std::to_string(10000 + 1000 * g + e));
  }
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
    if (l.size() == 14 && l.compare(0, 8, "entrance") == 0 && l.compare(10, 4, ".pac") == 0 && std::isdigit((unsigned char)l[8]))
      disc.push_back(f);
  }
  std::sort(disc.begin(), disc.end());
  // (each file is unpacked once; the scan stops when every wanted bank is found: a bank's variants sit in its own file, as in
  // Entrance00 (100 / 10100) and Entrance_dlc04 (385 / 10385..14385))
  auto all = [&] {
    for (int e : wanted_chars)
      if (!got.count(e)) return false;
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
    const std::string rel = "pac\\evt\\Entrance_dlc" + rp.Pp() + ".pac";
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
bool BaseRenders(Builder& b, int id, bool sd, Renders& r, std::map<std::string, std::unique_ptr<io::Pac>>& cache) {
  const char* tags[3] = {"SSFA", "SSFB", "SSFC"};
  char key[8];
  std::snprintf(key, sizeof key, "%04d", id);
  const RealPack* p = b.PackOfChar(id);
  const std::string file = p ? std::string("pac\\DLC_") + (sd ? "SD" : "HD") + "_dlc" + p->Pp() + ".pac"
                             : std::string("pac\\menu\\menu") + (sd ? "SD" : "HD") + ".pac";
  const std::string k = (p ? std::to_string(p->id) : "disc") + file;
  auto it = cache.find(k);
  if (it == cache.end()) {
    Bytes d;
    auto pac = std::make_unique<io::Pac>();
    std::string e;
    const bool ok = p ? b.ReadPackFile(*p, file, d) : b.ReadGame(file, d);
    if (!ok || !io::ReadPac(d, *pac, e)) pac.reset();
    it = cache.emplace(k, std::move(pac)).first;
  }
  for (int t = 0; t < 3; ++t) {
    io::Entry* e = it->second ? it->second->Find(tags[t], key) : nullptr;
    if (!e || e->blob.empty()) return false;
    r[size_t(t)] = e->blob;
  }
  return true;
}
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

std::string ManifestJson(const PackState& st, int pack_id, bool needs_build) {
  std::string j = "{\n  \"version\": 1,\n  \"enabled\": " + std::string(st.enabled ? "true" : "false") + ",\n  \"needs_build\": " +
                  (needs_build ? "true" : "false") + ",\n  \"pack_id\": " + std::to_string(pack_id) + ",\n  \"stars\": [";
  for (size_t i = 0; i < st.stars.size(); ++i) {
    const Superstar& s = st.stars[i];
    j += std::string(i ? "," : "") + "\n    {\"slot\": " + std::to_string(s.slot) + ", \"name\": \"" + Esc(s.name) + "\", \"short_name\": \"" +
         Esc(s.short_name) + "\", \"base_id\": " + std::to_string(s.base_id) + ", \"entrance_from\": " + std::to_string(s.entrance_from) +
         ", \"copy_ratings\": " + (s.copy_ratings ? "true" : "false") + ", \"source_pac\": \"" + Esc(s.source_pac) + "\", \"renders\": [";
    for (int k = 0; k < 3; ++k)
      j += std::string(k ? ", " : "") + "[" + std::to_string(s.render_rgba[k].empty() ? 0 : s.render_w[k]) + ", " +
           std::to_string(s.render_rgba[k].empty() ? 0 : s.render_h[k]) + "]";
    j += "]}";
  }
  return j + "\n  ]\n}\n";
}

const char* const kRenderTag[3] = {"A", "B", "C"};
fs::path RenderFile(const fs::path& root, int slot, int k) { return root / "renders" / (std::to_string(slot) + "_" + kRenderTag[k] + ".png"); }

// pack.json + renders\<slot>_A/B/C.png (PNG, our own writer); renders of characters no longer in the list are removed.
bool WriteManifest(const fs::path& root, const PackState& st, int pack_id, bool needs_build, std::string& err) {
  std::error_code ec;
  fs::create_directories(root / "renders", ec);
  std::set<std::string> keep;
  for (const Superstar& s : st.stars)
    for (int k = 0; k < 3; ++k) {
      const fs::path f = RenderFile(root, s.slot, k);
      if (s.render_rgba[k].empty() || s.render_w[k] <= 0 || s.render_h[k] <= 0) continue;
      const Bytes png = png::Write(s.render_rgba[k], s.render_w[k], s.render_h[k]);
      if (png.empty() || !WriteAll(f, png)) return err = "could not write " + PathU8(f), false;
      keep.insert(Lower(PathU8(f.filename())));
    }
  for (const auto& e : fs::directory_iterator(root / "renders", ec))
    if (!keep.count(Lower(PathU8(e.path().filename())))) fs::remove(e.path(), ec);
  if (!WriteText(root / "pack.json.part", ManifestJson(st, pack_id, needs_build))) return err = "could not write pack.json", false;
  fs::rename(root / "pack.json.part", root / "pack.json", ec);
  if (ec) {
    fs::remove(root / "pack.json", ec);
    fs::rename(root / "pack.json.part", root / "pack.json", ec);
    if (ec) return err = "could not write pack.json", false;
  }
  return true;
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

// ------------------------------------------------------------------ manifest
// portraits = false (review N3): the list without the PNG portraits, for the check before every PLAY (cheap).
static bool LoadPackImpl(const std::string& mods_dir, PackState& out, std::string& err, bool portraits = true) {
  out = {};
  const fs::path root = U8Path(mods_dir) / "superstars";
  Bytes b;
  std::error_code ec;
  if (!fs::exists(root / "pack.json", ec)) return true;
  if (!ReadAll(root / "pack.json", b, 16 << 20)) return err = "cannot read pack.json", false;
  const std::string text(b.begin(), b.end());
  Json j;
  JsonParser p{text};
  if (!p.Val(j) || j.t != Json::kObj) return err = "pack.json is damaged", false;
  out.enabled = j.Bool("enabled", true);
  if (const Json* a = j.Get("stars"); a && a->t == Json::kArr)
    for (const Json& x : a->a) {
      Superstar s;
      s.slot = x.Int("slot", -1), s.name = x.Str("name"), s.short_name = x.Str("short_name");
      s.base_id = x.Int("base_id", 100), s.entrance_from = x.Int("entrance_from", -1);
      s.copy_ratings = x.Bool("copy_ratings", true), s.source_pac = ResolveSource(root, x.Str("source_pac"));  // (Session 101: a moved folder)
      for (int k = 0; k < 3 && portraits; ++k) {  // the portraits kept by SavePack / BuildPack
        Bytes f;
        int w = 0, h = 0;
        if (ReadAll(RenderFile(root, s.slot, k), f, 64 << 20) && png::Read(f, s.render_rgba[k], w, h)) s.render_w[k] = w, s.render_h[k] = h;
        else s.render_rgba[k].clear();
      }
      out.stars.push_back(std::move(s));
    }
  out.needs_build = j.Bool("needs_build", false) ||
                    (!out.stars.empty() && !fs::is_regular_file(root / "pack6" / "info" / "catalog.dlc", ec));
  return true;
}

static bool SavePackImpl(const std::string& mods_dir, const PackState& st, std::string& err) {
  if (mods_dir.empty()) return err = "no mods folder", false;
  if (st.stars.size() > size_t(kMaxStars)) return err = "more than " + std::to_string(kMaxStars) + " characters", false;
  const fs::path root = U8Path(mods_dir) / "superstars";
  // the pack id stays what the last build wrote (pack.json), else 6
  int pack_id = 6;
  Bytes b;
  if (ReadAll(root / "pack.json", b, 16 << 20)) {
    const std::string text(b.begin(), b.end());
    Json j;
    JsonParser p{text};
    if (p.Val(j)) pack_id = j.Int("pack_id", 6);
  }
  return WriteManifest(root, st, pack_id, st.needs_build, err);
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
    for (const auto& e : p.entries) ours = ours || (e.pack >= 6 && e.type == 1 && e.char_id >= kFirstSlot);  // (a pack of ours in the DLC folder)
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

// ------------------------------------------------------------------ BuildPack
static bool BuildPackImpl(const std::string& mods_dir, const launcher::GameFiles& game_in, const std::string& dlc_dir, const PackState& st_in,
               std::function<void(float, const char*)> progress, std::string& err, const std::string& dlc_off) {
  std::lock_guard lock(g_log_mutex);
  g_log.clear(), g_moves.clear();
  Builder b;
  b.log = &g_log;
  b.progress = std::move(progress);
  if (mods_dir.empty()) return err = "no mods folder", false;
  const fs::path root = U8Path(mods_dir) / "superstars";
  // Session 101 (review S4): one build at a time across processes (the importer, the PC launcher, the phone's launcher and viewer
  // all build the same pack6.building): an exclusive lock file, released on every return.
  BuildLock held(root);
  if (!held.ok) return err = held.why, false;
  b.game.SetRoot(game_in.root());
  if (!b.game.image_error().empty()) return err = "game files: " + b.game.image_error(), false;
  std::error_code ec;
  // Session 101 (lane 199): built on the packs the game gets (USE MY DLC, the switched-off ones, packages: pack_fit.h GamePacks)
  b.packs = RealPacks(dlc_dir, root, dlc_off);
  for (const RealPack& p : b.packs)
    if (p.Roster()) b.top = &p;
  // Session 101: the build's own copy of the list - sources found again after a moved folder, slots no longer free moved (below)
  PackState st = st_in;
  for (Superstar& s : st.stars) s.source_pac = ResolveSource(root, s.source_pac);
  // No DLC pack with a roster: the disc's own cumulative files are the base (b.top stays null; built_on stays empty).
  int max_real = 0, max_entry = 0;
  for (const RealPack& p : b.packs) {
    max_real = std::max(max_real, p.id);
    for (const auto& e : p.entries) max_entry = std::max<int>(max_entry, e.id);
  }
  b.pack_id = std::max(6, max_real + 1);
  char ppb[8];
  std::snprintf(ppb, sizeof ppb, "%02d", b.pack_id);
  b.pp = ppb;
  const uint16_t revision = uint16_t(b.top ? b.top->revision + 1 : 32);  // (no DLC: 32, the value proven in the game beside pack 5's 31)
  if (b.top) b.built_on.insert(b.top->id);
  b.Log("packs " + std::to_string(b.packs.size()) + ", cumulative files from " + b.BaseName() + (b.top ? " (" + PathU8(b.top->root) + ")" : std::string()) +
        ", our pack id " + std::to_string(b.pack_id) + " revision " + std::to_string(revision));

  // ---- check the characters
  if (st.stars.empty()) {
    // Session 101 (lane 199): the last one was removed - the old pack (still holding him) comes out of the game; before, the build
    // refused an empty list and the removed characters stayed in the game.
    std::error_code e2;
    fs::remove(root / "superstars.txt", e2);
    fs::remove_all(root / "pack6", e2);
    if (e2) return err = "the old pack could not be removed (close the game first): " + e2.message(), false;
    if (!WriteManifest(root, st, b.pack_id, /*needs_build=*/false, err)) return false;
    fs::remove(wwe2k14::superstars_fit::StatusFile(U8Path(mods_dir)), e2);
    b.Log("no characters: the pack is removed (the game's own roster only)");
    return true;
  }
  {
    // Session 101 (lane 199): a base / entrance wrestler from DLC the game does not get now (USE MY DLC off, that pack switched off or
    // gone) - said plainly, with what to do, instead of a missing-render or roster-row error further on.
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
      for (int w : {s.base_id, s.entrance_from}) {
        if (w < 0 || !dlc_ids.count(w) || offered_ids.count(w)) continue;
        return err = s.name + ": his " + (w == s.base_id ? "base" : "entrance") + " wrestler (" + std::to_string(w) +
                     ") comes with DLC the game does not get now. Switch that DLC on (USE MY DLC, and the pack itself), or pick "
                     "another wrestler for him in the Superstar Importer", false;
      }
    }
  }
  if (st.stars.size() > size_t(kMaxStars)) return err = "more than " + std::to_string(kMaxStars) + " characters", false;
  const std::vector<int> free = FreeSlots(game_in, dlc_dir);
  const std::set<int> pack_used = UsedByPacks(dlc_dir);
  {
    // Session 101 (lane 199; review S5): a slot one of the player's packs now uses (a pack added since the last build), one used
    // twice or one outside 397..499 is MOVED to a free slot, not refused: before, such a rebuild failed and the player had to remove
    // and import the character again. Only those: a character already on 399 (kReservedSlots) with no pack using it stays (the
    // reservation is for new imports). His id changes, so a save that used the old one (Universe, a match card) shows someone else
    // there; pack.json follows, and the player is told (LastBuildMoves).
    std::set<int> kept;
    std::vector<size_t> move;
    for (size_t i = 0; i < st.stars.size(); ++i) {
      const int slot = st.stars[i].slot;
      if (slot >= kFirstSlot && slot <= kLastSlot && !pack_used.count(slot) && kept.insert(slot).second) continue;
      move.push_back(i);
    }
    for (size_t i : move) {
      int to = -1;
      for (int f : free)  // (never a slot another character keeps or was just given; a slot a mover leaves is a pack's: never free)
        if (!kept.count(f)) {
          to = f;
          break;
        }
      Superstar& s = st.stars[i];
      if (to < 0) return err = "character " + std::to_string(i + 1) + " (" + s.name + "): no free slot left for him", false;
      b.Log("slot " + std::to_string(s.slot) + " of " + s.name + " is not free (a DLC pack uses it, or it was used twice): moved to " +
            std::to_string(to));
      g_moves.push_back(s.name + " moved from slot " + std::to_string(s.slot) + " to " + std::to_string(to) +
                        " (saves that used him may show someone else).");
      b.warnings.push_back(g_moves.back());
      s.slot = to;
      kept.insert(to);
    }
  }
  std::set<int> slots;
  std::vector<ModPac> mods(st.stars.size());
  uint64_t source_bytes = 0;
  for (size_t i = 0; i < st.stars.size(); ++i) {
    const Superstar& s = st.stars[i];
    const std::string who = "character " + std::to_string(i + 1) + " (" + s.name + ")";
    if (s.slot < kFirstSlot || s.slot > kLastSlot || pack_used.count(s.slot)) return err = who + ": slot " + std::to_string(s.slot) + " is not free", false;
    if (!slots.insert(s.slot).second) return err = who + ": slot " + std::to_string(s.slot) + " used twice", false;
    if (s.name.empty()) return err = who + ": no name", false;
    if (s.base_id < kMinBase || s.base_id > kMaxBase || (s.entrance_from >= 0 && (s.entrance_from < kMinBase || s.entrance_from > kMaxBase)))
      return err = who + ": the base / entrance wrestler must be a roster wrestler (" + std::to_string(kMinBase) + ".." +
                   std::to_string(kMaxBase) + ")", false;
    Bytes d;
    if (!ReadAll(U8Path(s.source_pac), d, assets::char_mods::kMaxPac)) return err = who + ": cannot read " + s.source_pac, false;
    source_bytes += d.size();
    const std::string why = ParseModPac(std::move(d), mods[i], nullptr);
    if (!why.empty()) return err = who + ": " + why, false;
    mods[i].pac = {};  // (review: only the claimed id / attires are kept here; step 4 reads each source again, one at a time)
    if (const RealPack* p = b.PackOfChar(s.base_id)) b.built_on.insert(p->id);
    if (const RealPack* p = b.PackOfChar(s.entrance_from >= 0 ? s.entrance_from : s.base_id)) b.built_on.insert(p->id);
  }

  // ---- free space (review): the sources + the top pack's pacs we copy + a tron each, + 10%; x2 while the old pack is kept
  //      until the new one is swapped in
  {
    uint64_t need = source_bytes;
    if (b.top) {
      for (const auto& f : b.top->files) {
        const std::string l = Lower(f.name);
        if (l.rfind("pac\\", 0) == 0 && l.find("\\ch\\ch") == std::string::npos && l.find("\\evt\\") == std::string::npos)
          need += b.top->Size(f.name);
      }
    } else {
      need += 64ull << 20;  // (the disc's string, DLC, sound, audience, AI and roster pacs, unpacked)
    }
    need += uint64_t(st.stars.size()) * ((8ull << 20) /* a tron */ + (2ull << 20) /* entrance banks, renders */);
    need += need / 10;
    std::error_code e2;
    if (fs::exists(root / "pack6", e2)) need *= 2;
    fs::create_directories(root, e2);
    const fs::space_info sp = fs::space(root, e2);
    if (!e2 && sp.available < need)
      return err = "not enough free space for the pack: " + std::to_string(need >> 20) + " MB needed, " + std::to_string(sp.available >> 20) +
                   " MB free", false;
  }

  // ---- the output folder: built aside, swapped in at the end
  b.out = root / "pack6.building";
  fs::remove_all(b.out, ec);
  fs::create_directories(b.out / "info", ec);
  fs::create_directories(b.out / "pac" / "ch", ec);
  fs::create_directories(b.out / "pac" / "evt", ec);
  auto fail = [&](const std::string& why) {
    std::error_code e2;
    fs::remove_all(b.out, e2);
    err = why;
    return false;
  };
  std::vector<std::pair<std::string, io::Pac>> written;  // (catalog name, the pac's directory with blobs cleared): the arcs
  auto keep = [&](const std::string& rel, io::Pac& p) {
    for (io::Dir& d : p.dirs)
      for (io::Entry& e : d.ents) e.blob.clear(), e.blob.shrink_to_fit();
    written.emplace_back(rel, std::move(p));
  };
  auto emit = [&](const std::string& rel, io::Pac& p) {
    const Bytes bytes = io::WritePac(p);
    if (bytes.empty() || !WriteAll(Under(b.out, rel), bytes)) return false;
    keep(rel, p);
    return true;
  };
  std::vector<std::string> plain_files;  // listed (-1, -1): trons

  // 1. cumulative byte copies (DLC, ExclusiveAI, audience, sound) of the top pack's
  b.Prog(0.02f, "copying your DLC's shared files");
  for (const char* base : {"DLC", "ExclusiveAI", "audience", "sound"}) {
    const std::string src = std::string("pac\\") + base, from = b.CumulativeName(src), to = b.Rel(src);
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative(src, d) || !io::ReadPac(d, p, e)) return fail("cannot read " + from + " of " + b.BaseName());
    if (!WriteAll(Under(b.out, to), d)) return fail("cannot write " + to);
    keep(to, p);
    b.Log(to + " = copy of " + b.BaseName() + "'s");
  }
  // 2. string: the top pack's with every character's own name / short / long text
  b.Prog(0.06f, "writing the names");
  {
    const std::string from = b.CumulativeName("pac\\string");
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative("pac\\string", d) || !io::ReadPac(d, p, e)) return fail("cannot read " + from);
    std::map<uint32_t, std::string> texts;
    for (const Superstar& s : st.stars) {
      texts[uint32_t(60000 + s.slot)] = s.name;
      texts[uint32_t(62000 + s.slot)] = s.short_name.empty() ? s.name : s.short_name;
      texts[uint32_t(64000 + s.slot)] = s.name;
    }
    if (!PatchStrings(b, p, texts, e)) return fail(e);
    if (!emit(b.Rel("pac\\string"), p)) return fail("cannot write the string pac");
  }
  // 3. chEtc
  b.Prog(0.10f, "writing the roster rows");
  {
    const std::string from = b.CumulativeName("pac\\ch\\chEtc");
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative("pac\\ch\\chEtc", d) || !io::ReadPac(d, p, e)) return fail("cannot read " + from);
    if (!PatchChEtc(b, p, st.stars, e)) return fail(e);
    if (!emit(b.Rel("pac\\ch\\chEtc"), p)) return fail("cannot write the roster pac");
  }
  // 4. the character pacs: EMD 000<claimed>ak -> 000<slot>ak; attires the base has and the mod lacks point at the mod's
  //    attire 0 (directory aliases, no copy: any attire the game asks for loads the mod's model)
  std::map<int, std::set<int>> base_attires;  // base id -> attire digits of his own model pac
  for (size_t i = 0; i < st.stars.size(); ++i) {
    const Superstar& s = st.stars[i];
    b.Prog(0.12f + 0.30f * float(i) / float(st.stars.size()), "relabelling the models");
    if (!base_attires.count(s.base_id)) {
      std::set<int>& a = base_attires[s.base_id];
      Bytes d;
      io::Pac p;
      std::string e;
      const RealPack* rp = b.PackOfChar(s.base_id);
      const std::string rel = "pac\\ch\\ch" + std::to_string(s.base_id) + (rp ? "_dlc" + rp->Pp() : std::string()) + ".pac";
      if ((rp ? b.ReadPackFile(*rp, rel, d) : b.ReadGame(rel, d)) && io::ReadPac(d, p, e))
        if (io::Dir* emd = p.FindDir("EMD "))
          for (const io::Entry& x : emd->ents)
            if (IsEmdName(x.name) && std::atoi(x.name.substr(3, 3).c_str()) == s.base_id) a.insert(x.name[6] - '0');
    }
    // (review: one source in memory at a time) read and check it again: it may have changed since the check above
    ModPac m;
    {
      Bytes d;
      if (!ReadAll(U8Path(s.source_pac), d, assets::char_mods::kMaxPac)) return fail(s.name + ": cannot read " + s.source_pac);
      const std::string why = ParseModPac(std::move(d), m, nullptr);
      if (!why.empty() || m.claimed != mods[i].claimed)
        return fail(s.name + ": " + (why.empty() ? std::string("the source pac changed during the build") : why));
      mods[i] = {};
    }
    io::Dir* emd = m.pac.FindDir("EMD ");
    char from[8], to[8];
    std::snprintf(from, sizeof from, "000%03d", m.claimed), std::snprintf(to, sizeof to, "000%03d", s.slot);
    std::string renamed;
    for (io::Entry& x : emd->ents)
      if (x.name.compare(0, 6, from) == 0) x.name.replace(0, 6, to), renamed += " " + x.name;
    // aliases: the attires the base has, for each kind the mod's first attire has
    const auto& first = m.att.begin()->second;
    std::string aliased;
    for (int a : base_attires[s.base_id]) {
      if (m.att.count(a)) continue;
      for (const auto& [kind, idx] : first) {
        io::Entry al;
        al.name = std::string(to) + char('0' + a) + char('0' + kind);
        const int target = emd->ents[size_t(idx)].alias;  // (an alias of an alias points at the real entry)
        al.alias = target >= 0 ? target : idx;
        emd->ents.push_back(al);
        aliased += " " + al.name;
      }
    }
    const std::string rel = b.Rel("pac\\ch\\ch" + std::to_string(s.slot));
    if (!emit(rel, m.pac)) return fail("cannot write " + rel);
    m.pac = {};
    b.Log(rel + ": models" + renamed + (aliased.empty() ? "" : "; attire aliases" + aliased) + " (from " + s.source_pac + ", claimed " +
          std::to_string(m.claimed) + ")");
  }
  // 5. entrances
  b.Prog(0.45f, "copying the entrances");
  {
    std::set<int> want;
    for (const Superstar& s : st.stars) want.insert(s.entrance_from >= 0 ? s.entrance_from : s.base_id);
    const std::map<int, BankSrc> banks = FindBanks(b, want);
    io::Pac ent;
    ent.magic = "EPK8";
    ent.dirs.resize(2);
    ent.dirs[0].tag = "EVP ", ent.dirs[1].tag = "EVZ ";
    ent.dirs[0].kind = ent.dirs[1].kind = 1;
    // One pac's directory holds at most ~447 banks (two 16-byte rows each, 14 KB: io::kMaxDirBytes). Banks are taken in
    // tiers over all characters - every character's own bank first, then the variants 10000 + 1000 G + id by G - so a
    // full pack (102 characters x 6 banks for The Rock) keeps everyone's own and first variants and drops the rest (logged).
    // [U] whether the game finds a bank in a second entrance pac of the same pack: not tried, so one pac only.
    struct Cand {
      int tier, star, from, to;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < st.stars.size(); ++i) {
      const Superstar& s = st.stars[i];
      const int e = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
      bool any = false;
      for (int g = -1; g < 10; ++g) {
        const int from = g < 0 ? e : 10000 + 1000 * g + e, to = g < 0 ? s.slot : 10000 + 1000 * g + s.slot;
        if (banks.count(from)) cands.push_back({g + 1, int(i), from, to}), any = true;
      }
      if (!any) b.warnings.push_back(s.name + ": no entrance bank " + std::to_string(e) + " found (entrances may not play)");
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.tier < y.tier; });
    const size_t room = (io::kMaxDirBytes - 2 * 12) / 32;
    std::vector<std::pair<int, BankSrc>> out;  // (new bank id, data)
    std::map<int, std::string> ids;
    std::map<int, int> actors_total;
    std::string dropped;
    for (const Cand& c : cands) {
      if (out.size() >= room) {
        dropped += " " + std::to_string(c.to);
        continue;
      }
      const Superstar& s = st.stars[size_t(c.star)];
      int actors = 0;
      BankSrc n;
      n.evp = RetargetEv(banks.at(c.from).evp, s.entrance_from >= 0 ? s.entrance_from : s.base_id, c.to, s.slot, actors);
      n.evz = banks.at(c.from).evz;
      if (n.evp.empty()) continue;
      actors_total[c.star] += actors;
      out.emplace_back(c.to, std::move(n));
      ids[c.star] += " " + std::to_string(c.to);
    }
    for (size_t i = 0; i < st.stars.size(); ++i)
      if (ids.count(int(i)))
        b.Log("entrance banks" + ids[int(i)] + " = wrestler " + std::to_string(st.stars[i].entrance_from >= 0 ? st.stars[i].entrance_from : st.stars[i].base_id) +
              "'s (" + std::to_string(actors_total[int(i)]) + " actor model keys -> " + std::to_string(st.stars[i].slot) + "xx)");
    if (!dropped.empty()) b.warnings.push_back("the entrance pac is full: variant banks left out:" + dropped);
    if (!out.empty()) {
      std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first < y.first; });  // ids ascending, as the DLC files
      for (auto& [id, d] : out) {
        io::Entry p, z;
        p.name = z.name = io::Pad(std::to_string(id), 8);
        p.blob = std::move(d.evp), z.blob = std::move(d.evz);
        ent.dirs[0].ents.push_back(std::move(p)), ent.dirs[1].ents.push_back(std::move(z));
      }
      if (!emit(b.Rel("pac\\evt\\Entrance"), ent)) return fail("cannot write the entrance pac");
    }
  }
  // 6. trons: the entrance wrestler's movies\titantron\<id>.bik2 (disc or pack) as <slot>.bik2
  b.Prog(0.60f, "copying the trons");
  for (const Superstar& s : st.stars) {
    const int e = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
    const std::string src = "movies\\titantron\\" + std::to_string(e) + ".bik2", dst = "movies\\titantron\\" + std::to_string(s.slot) + ".bik2";
    const fs::path to = Under(b.out, dst);
    fs::create_directories(to.parent_path(), ec);
    bool found = false, ok = false;
    const RealPack* rp = b.PackOfChar(e);
    const fs::path f = rp && !rp->pkg && rp->Has(src) ? FindUnder(rp->root, src) : fs::path();
    if (!f.empty()) {
      found = true;
      ok = fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec) && !ec;
    } else if (rp && rp->pkg && rp->Has(src)) {  // (Session 101: from inside an Xbox 360 package)
      Bytes d;
      found = true;
      ok = rp->Read(src, d) && WriteAll(to, d);
    } else if (b.game.Exists(src)) {
      found = true;
      std::ofstream o(to, std::ios::binary | std::ios::trunc);
      ok = o && b.game.Stream(src, [&](const uint8_t* p, size_t n) { o.write(reinterpret_cast<const char*>(p), std::streamsize(n)); }) && o.flush();
    }
    if (found && !ok) return fail("could not copy " + src + " into the pack (disk full?)");  // (review: a write failure is fatal)
    if (ok) plain_files.push_back(dst), b.Log(dst + " = " + src);
    else b.warnings.push_back(s.name + ": no titantron " + src + " found");
  }
  // 7. DLC_HD / DLC_SD: SSFA / SSFB / SSFC <slot> + the top pack's banners with one leaf per entry
  b.Prog(0.70f, "making the roster pictures");
  // (no DLC: the disc's banner table has its own leaf ids, 0..4, 90, 91 on Jarrod's disc; ours start above them. A clash = two leaves
  // with one id, seen in the S98 no-DLC check.)
  if (!b.top) {
    for (const char* res : {"pac\\DLC_HD", "pac\\DLC_SD"}) {
      Bytes d;
      io::Pac p;
      std::string e;
      std::vector<io::Leaf> leaves;
      if (!b.ReadCumulative(res, d) || !io::ReadPac(d, p, e)) return fail(std::string("cannot read ") + res + ".pac");
      if (io::Entry* tex = p.Find("IGST", "TEX "); tex && io::ReadPach(tex->blob, leaves))
        for (const io::Leaf& l : leaves) max_entry = std::max<int>(max_entry, int(l.id));
    }
  }
  const uint16_t first_entry = uint16_t(max_entry + 1);
  {
    for (int sd = 0; sd < 2; ++sd) {
      const std::string res = sd ? "SD" : "HD";
      std::map<std::string, std::unique_ptr<io::Pac>> cache;  // (this resolution's base pacs only)
      std::vector<Renders> rs(st.stars.size());
      for (size_t i = 0; i < st.stars.size(); ++i) {
        const Superstar& s = st.stars[i];
        std::array<bool, 3> have;
        UiRenders(s, sd != 0, rs[i], have);
        Renders base;
        const bool need_base = !have[0] || !have[1] || !have[2];
        if (need_base && !BaseRenders(b, s.base_id, sd != 0, base, cache))
          return fail(s.name + ": no renders of wrestler " + std::to_string(s.base_id) + " to copy");
        std::string what;
        for (int k = 0; k < 3; ++k) {
          if (!have[size_t(k)]) rs[i][size_t(k)] = base[size_t(k)];
          what += std::string(k ? ", " : "") + "SSF" + char('A' + k) + (have[size_t(k)] ? " from the importer" : " = " + std::to_string(s.base_id) + "'s");
        }
        if (!sd) b.Log("renders " + std::to_string(s.slot) + ": " + what);
      }
      io::Pac topp;
      Bytes d;
      std::string e;
      const std::string from = b.CumulativeName("pac\\DLC_" + res);
      if (!b.ReadCumulative("pac\\DLC_" + res, d) || !io::ReadPac(d, topp, e)) return fail("cannot read " + from);
      io::Pac p;
      p.magic = "EPAC", p.version = topp.version, p.trailer = topp.trailer;
      const char* tags[3] = {"SSFA", "SSFB", "SSFC"};
      for (int k = 0; k < 3; ++k) {
        io::Dir dir;
        dir.tag = tags[k];
        for (size_t i = 0; i < st.stars.size(); ++i) {
          io::Entry x;
          char key[8];
          std::snprintf(key, sizeof key, "%04d", st.stars[i].slot);
          x.name = key;
          x.blob = std::move(rs[i][size_t(k)]);
          dir.ents.push_back(std::move(x));
        }
        std::sort(dir.ents.begin(), dir.ents.end(), [](auto& x, auto& y) { return x.name < y.name; });
        p.dirs.push_back(std::move(dir));
      }
      io::Entry* tex = topp.Find("IGST", "TEX ");
      std::vector<io::Leaf> leaves;
      if (!tex || !io::ReadPach(tex->blob, leaves) || leaves.empty()) return fail(from + ": no store banners");
      Bytes generic = leaves.front().raw;  // leaf 0: the generic card
      for (const auto& l : leaves)
        if (l.id == 0) generic = l.raw;
      for (size_t i = 0; i < st.stars.size(); ++i) {
        io::Leaf l;
        l.id = first_entry + uint32_t(i);
        l.raw = generic;
        leaves.push_back(std::move(l));
      }
      std::sort(leaves.begin(), leaves.end(), [](auto& x, auto& y) { return x.id < y.id; });  // ids ascending, as the real PACHs
      io::Dir ig;
      ig.tag = "IGST";
      io::Entry te;
      te.name = "TEX ";
      te.blob = io::WritePach(leaves, /*dedupe=*/true);
      ig.ents.push_back(std::move(te));
      p.dirs.push_back(std::move(ig));
      if (!emit(b.Rel("pac\\DLC_" + res), p)) return fail("cannot write DLC_" + res);
    }
    b.Log("DLC_HD / DLC_SD: renders for " + std::to_string(st.stars.size()) + " characters + banner leaves " + std::to_string(first_entry) +
          ".." + std::to_string(first_entry + st.stars.size() - 1) + " (copies of leaf 0, shared data)");
  }
  // 8. catalog + arcs: DLC_HD (0, -1), DLC_SD (-1, 0), trons (-1, -1), every other pac a == b running in sorted order
  b.Prog(0.90f, "writing the catalog");
  {
    std::vector<io::CatFile> files;
    std::vector<std::pair<std::string, const io::Pac*>> sorted;
    for (auto& [n, p] : written) sorted.emplace_back(n, &p);
    std::sort(sorted.begin(), sorted.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::vector<const io::Pac*> hd, sd;
    hd.push_back(nullptr), sd.push_back(nullptr);
    int k = 1;
    for (auto& [n, p] : sorted) {
      io::CatFile f;
      f.name = n;
      if (n.find("DLC_HD_") != std::string::npos) f.a = 0, f.b = -1, hd[0] = p;
      else if (n.find("DLC_SD_") != std::string::npos) f.a = -1, f.b = 0, sd[0] = p;
      else f.a = f.b = int16_t(k++), hd.push_back(p), sd.push_back(p);
      files.push_back(f);
    }
    for (const std::string& n : plain_files) files.push_back({n, -1, -1});
    if (!hd[0] || !sd[0]) return fail("internal: no DLC_HD / DLC_SD written");
    std::vector<io::CatEntry> entries;
    for (size_t i = 0; i < st.stars.size(); ++i) {
      io::CatEntry e;
      e.id = uint16_t(first_entry + i), e.type = 1, e.pack = uint16_t(b.pack_id), e.bit = 0, e.char_id = st.stars[i].slot;
      e.name = Latin1(st.stars[i].name);
      entries.push_back(e);
    }
    const Bytes cat = io::WriteCatalog(uint16_t(b.pack_id), revision, entries, files, FileTimeNow());
    if (!WriteAll(b.out / "info" / "catalog.dlc", cat) || !WriteAll(b.out / "info" / "catalog_hd.arc", io::WriteArc(hd)) ||
        !WriteAll(b.out / "info" / "catalog_sd.arc", io::WriteArc(sd)))
      return fail("cannot write the catalog");
    // check it the way the game reads it (the gate opened for our pack id, as the game side does)
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    gate[0] = uint32_t(b.pack_id) << 16 | 1;
    io::CatInfo info;
    const std::string why = io::ParseCatalogLikeGame(cat, gate, info);
    if (!why.empty()) return fail("internal: the catalog does not parse: " + why);
    b.Log("info\\catalog.dlc: pack " + std::to_string(b.pack_id) + " revision " + std::to_string(revision) + ", " + std::to_string(entries.size()) +
          " type-1 entries (" + std::to_string(first_entry) + ".." + std::to_string(first_entry + entries.size() - 1) + "), " +
          std::to_string(files.size()) + " files");
  }
  // 9. swap in, then the manifest, the renders and the game's file. The game's file is written aside BEFORE the swap (review N1) and
  // renamed right after it, so the pack and it always agree (nothing slow between the two). No on / off in it: the launcher's
  // USE MY SUPERSTARS is the only switch (Jarrod, S98: one simple switch).
  b.Prog(0.96f, "finishing");
  {
    std::string txt = "# WWE 2K14 Superstar Importer: read by the game at boot (src\\superstars.h). Written by the importer.\npack " +
                      std::to_string(b.pack_id) + "\nbuilt_on";
    for (int id : b.built_on) txt += " " + std::to_string(id);
    txt += "\n";
    {  // Session 101: the top roster pack and which copy of it (pack_fit.h: the game, the importer and the launchers compare it)
      char top[48];
      std::snprintf(top, sizeof top, "top %d %08X\n", b.top ? b.top->id : 0, b.top ? unsigned(b.top->sig) : 0u);
      txt += top;
    }
    for (const Superstar& s : st.stars)
      txt += "star " + std::to_string(s.slot) + " music " + std::to_string(s.entrance_from >= 0 ? s.entrance_from : s.base_id) + "\n";
    if (!WriteText(root / "superstars.txt.part", txt)) return fail("superstars.txt could not be written");
  }
  const fs::path final_dir = root / "pack6", old = root / "pack6.old";
  fs::remove_all(old, ec);
  if (fs::exists(final_dir, ec)) {
    fs::rename(final_dir, old, ec);
    if (ec) {
      const std::string why = ec.message();
      std::error_code e2;
      fs::remove(root / "superstars.txt.part", e2);
      return fail("the old pack is in use (close the game first): " + why);
    }
  }
  fs::rename(b.out, final_dir, ec);
  if (ec) {
    std::error_code e2;
    fs::rename(old, final_dir, e2);
    fs::remove(root / "superstars.txt.part", e2);
    return fail("could not put the new pack in place: " + ec.message());
  }
  {
    std::error_code e2;
    fs::rename(root / "superstars.txt.part", root / "superstars.txt", e2);
    if (e2) {
      fs::remove(root / "superstars.txt", e2);
      fs::rename(root / "superstars.txt.part", root / "superstars.txt", e2);
      if (e2) return err = "the pack is built but superstars.txt could not be written", false;
    }
  }
  fs::remove_all(old, ec);
  if (!WriteManifest(root, st, b.pack_id, /*needs_build=*/false, err)) return err = "the pack is built but " + err, false;
  fs::remove(wwe2k14::superstars_fit::StatusFile(U8Path(mods_dir)), ec);  // (Session 101: the game's old verdict is about the old pack)
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
bool SavePack(const std::string& mods_dir, const PackState& st, std::string& err) {
  try {
    return SavePackImpl(mods_dir, st, err);
  } catch (const std::exception& e) {
    return err = std::string("could not save the pack list: ") + e.what(), false;
  }
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
    if (!c.stars) {  // (every superstar removed: the old pack, still holding them, comes out with a build)
      // (review S6) only when pack.json exists and says so: a pack with no list (copied in by hand, or pack.json lost) is never removed
      if (fs::is_regular_file(root / "pack.json", ec) && fs::is_regular_file(root / "pack6" / "info" / "catalog.dlc", ec))
        c.build = true, c.why = "You removed every superstar, but the old pack is still in the game.";
      return c;
    }
    if (!fs::is_regular_file(root / "pack6" / "info" / "catalog.dlc", ec) || !fs::is_regular_file(root / "superstars.txt", ec)) {
      c.build = true, c.why = "Your superstars are not built into the game yet.";
    } else if (st.needs_build) {
      c.build = true, c.why = "Your superstars list changed since the pack was last built.";
    } else if (const std::string p = PackProblem(mods_dir, dlc_dir, dlc_off); !p.empty()) {
      c.build = true, c.why = "Your superstars pack no longer fits your DLC: " + p + ".";
    }
  } catch (const std::exception& e) {
    c.why = std::string("Your superstars could not be checked: ") + e.what();
  }
  return c;
}

std::string LastGameProblem(const std::string& mods_dir) {
  try {
    if (mods_dir.empty()) return {};
    const std::string s = wwe2k14::superstars_fit::ReadStatus(U8Path(mods_dir));
    if (s.rfind("out ", 0) != 0) return {};
    return "In your last game your superstars were left out: " + s.substr(4) + ".";
  } catch (...) {
    return {};
  }
}

bool BuildPack(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const PackState& st,
               std::function<void(float, const char*)> progress, std::string& err, const std::string& dlc_off) {
  try {
    return BuildPackImpl(mods_dir, game, dlc_dir, st, std::move(progress), err, dlc_off);
  } catch (const std::exception& e) {
    std::error_code ec;
    fs::remove_all(U8Path(mods_dir) / "superstars" / "pack6.building", ec);
    return err = std::string("the build stopped: ") + e.what(), false;
  }
}

}  // namespace importer