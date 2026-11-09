// Session 98 (custom characters): the CHARACTERS part of the mods folder. Portable C++ (std only, no game data, no Win32).
// A community character is one uncompressed character pac (EPAC / EPK8 header, PACH archives at 0x4000: the layout of a game
// pac\ch\chNNN.pac minus the XCTD wrapper). Mods are additive (CLAUDE.md): an import COPIES the file to <mods>\characters\<safe name>.pac;
// nothing is written over game files, and the list is empty while the mods toggle is off.
// The pac is untrusted community data: nothing in it is executed; imports check the header, zip members are checked for
// path tricks and size before they are written, and the loader (asset_load.h LoadWrestlerFile) bounds-checks every read.
// Virtual ids: a custom character is addressed as kBase + n (n = order of first sight in this run, never reused) so that the
// viewer's wrestler id plumbing (place / picker / labels) works unchanged. Real game ids are < 1000.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mods_lookup.h"

namespace assets {
namespace char_mods {

namespace fs = std::filesystem;
inline constexpr int kBase = 900000;
inline constexpr uint64_t kMaxPac = 400ull << 20;  // size cap for one character pac (a game ch pac unpacks to ~13 MB)

inline fs::path Dir() { return mods_lookup::Cfg().dir.empty() ? fs::path() : mods_lookup::Cfg().dir / "characters"; }

// "..\\x/Mr Kennedy 09.pac" -> "Mr Kennedy 09" ("" when nothing usable is left). Keeps letters, digits, space, - _ . ( ) and non-ASCII
// bytes; everything else becomes '_'. No path parts, no leading dots.
inline std::string SafeName(const std::string& in) {
  size_t at = in.find_last_of("\\/");
  std::string s = at == std::string::npos ? in : in.substr(at + 1);
  if (s.size() > 4 && mods_lookup::Lower(s.substr(s.size() - 4)) == ".pac") s.resize(s.size() - 4);
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7F) continue;
    const bool ok = std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')' || c >= 0x80;
    o += ok ? char(c) : '_';
  }
  while (!o.empty() && (o.front() == '.' || o.front() == ' ')) o.erase(o.begin());
  while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
  if (o.size() > 80) o.resize(80);
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  // Windows reserved device names
  std::string l = mods_lookup::Lower(o);
  for (const char* r : {"con", "prn", "aux", "nul", "com1", "com2", "com3", "lpt1", "lpt2", "lpt3"})
    if (l == r) return "";
  return o;
}

// The header check an import does: big enough, EPAC / EPK8 tag, a PACH at 0x4000 with a sane count.
inline std::string CheckHead(const uint8_t* d, size_t n) {
  if (n < 0x4000 + 8) return "not a character pac (too small)";
  if (n > kMaxPac) return "not a character pac (too big)";
  if (std::memcmp(d, "EPAC", 4) != 0 && std::memcmp(d, "EPK8", 4) != 0) {
    if (n >= 4 && d[0] == 0x0F && d[1] == 0xF5 && d[2] == 0x12 && d[3] == 0xED) return "this pac is compressed like a game file: unpack it first";
    return "not a character pac (no EPAC / EPK8 tag)";
  }
  if (std::memcmp(d + 0x4000, "PACH", 4) != 0) return "not a character pac (no PACH at 0x4000)";
  uint32_t cnt;
  std::memcpy(&cnt, d + 0x4004, 4);
  if (cnt == 0 || cnt > 2000 || 0x4008 + 12ull * cnt > n) return "not a character pac (bad PACH table)";
  return "";
}

// ------------------------------------------------------------------ the list
struct Entry {
  std::string name;  // file stem, UTF-8
  fs::path file;
  int id = 0;        // virtual id
  uint64_t bytes = 0;
};
inline std::map<std::string, int>& IdMap() {
  static std::map<std::string, int> m;  // lower-case name -> virtual id
  return m;
}
inline int IdFor(const std::string& name) {
  auto& m = IdMap();
  const std::string k = mods_lookup::Lower(name);
  auto it = m.find(k);
  if (it != m.end()) return it->second;
  const int id = kBase + int(m.size());
  m[k] = id;
  return id;
}
// The imported characters (empty while the mods toggle is off or there is no mods folder). `even_off` lists them regardless of the toggle.
inline std::vector<Entry> List(bool even_off = false) {
  std::vector<Entry> v;
  if (Dir().empty() || (!mods_lookup::Cfg().on && !even_off)) return v;
  std::error_code ec;
  if (!fs::is_directory(Dir(), ec)) return v;
  for (const auto& e : fs::directory_iterator(Dir(), ec)) {
    std::error_code e2;
    if (!e.is_regular_file(e2) || mods_lookup::Lower(e.path().extension().string()) != ".pac") continue;
    Entry c;
    c.name = mods_lookup::U8(e.path().stem()), c.file = e.path(), c.id = IdFor(c.name);
    c.bytes = fs::file_size(e.path(), e2);
    v.push_back(c);
  }
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return mods_lookup::Lower(a.name) < mods_lookup::Lower(b.name); });
  return v;
}
inline bool IsCustom(int id) { return id >= kBase && id < kBase + 100000; }
inline bool Find(int id, Entry& out) {
  for (const Entry& e : List())
    if (e.id == id) return out = e, true;
  return false;
}
inline bool FindName(const std::string& name, Entry& out) {
  const std::string k = mods_lookup::Lower(name);
  for (const Entry& e : List())
    if (mods_lookup::Lower(e.name) == k) return out = e, true;
  return false;
}

// ------------------------------------------------------------------ inflate (raw deflate, RFC 1951), bounds-checked
namespace inflate_detail {
struct Huff {
  uint16_t count[16] = {}, sym[320] = {};
};
struct St {
  const uint8_t* in;
  size_t n, pos = 0;
  uint32_t bits = 0;
  int nb = 0;
  std::vector<uint8_t>* out;
  size_t cap;
  bool bad = false;
  int Bit() {
    if (nb == 0) {
      if (pos >= n) return bad = true, 0;
      bits = in[pos++], nb = 8;
    }
    const int b = int(bits & 1);
    bits >>= 1, --nb;
    return b;
  }
  uint32_t Bits(int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; ++i) v |= uint32_t(Bit()) << i;
    return v;
  }
};
inline void Build(Huff& h, const uint8_t* len, int n) {
  std::memset(&h, 0, sizeof h);
  for (int i = 0; i < n; ++i) ++h.count[len[i]];
  h.count[0] = 0;
  uint16_t offs[16] = {};
  for (int i = 1; i < 15; ++i) offs[i + 1] = uint16_t(offs[i] + h.count[i]);
  for (int i = 0; i < n; ++i)
    if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
}
inline int Decode(St& s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; ++len) {
    code |= s.Bit();
    if (s.bad) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.sym[index + (code - first)];
    index += count, first += count, first <<= 1, code <<= 1;
  }
  return -1;
}
inline bool Codes(St& s, const Huff& lit, const Huff& dist) {
  static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint16_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint16_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = Decode(s, lit);
    if (sym < 0) return false;
    if (sym < 256) {
      if (s.out->size() >= s.cap) return false;
      s.out->push_back(uint8_t(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = lbase[sym] + int(s.Bits(lext[sym]));
      const int ds = Decode(s, dist);
      if (ds < 0 || ds >= 30) return false;
      const size_t d = size_t(dbase[ds]) + s.Bits(dext[ds]);
      if (s.bad || d > s.out->size() || s.out->size() + size_t(len) > s.cap) return false;
      for (int i = 0; i < len; ++i) s.out->push_back((*s.out)[s.out->size() - d]);
    }
    if (s.bad) return false;
  }
}
}  // namespace inflate_detail

// Raw deflate -> out (at most `cap` bytes). False on any error.
inline bool Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t cap) {
  using namespace inflate_detail;
  out.clear();
  St s{in, n};
  s.out = &out, s.cap = cap;
  int last;
  do {
    last = s.Bit();
    const uint32_t type = s.Bits(2);
    if (s.bad) return false;
    if (type == 0) {
      s.bits = 0, s.nb = 0;
      if (s.pos + 4 > n) return false;
      const uint32_t len = in[s.pos] | (uint32_t(in[s.pos + 1]) << 8);
      const uint32_t nlen = in[s.pos + 2] | (uint32_t(in[s.pos + 3]) << 8);
      s.pos += 4;
      if ((len ^ 0xFFFFu) != nlen || s.pos + len > n || out.size() + len > cap) return false;
      out.insert(out.end(), in + s.pos, in + s.pos + len);
      s.pos += len;
    } else if (type == 1) {
      uint8_t l[320];
      int i = 0;
      for (; i < 144; ++i) l[i] = 8;
      for (; i < 256; ++i) l[i] = 9;
      for (; i < 280; ++i) l[i] = 7;
      for (; i < 288; ++i) l[i] = 8;
      Huff lit, dist;
      Build(lit, l, 288);
      uint8_t dl[30];
      for (int k = 0; k < 30; ++k) dl[k] = 5;
      Build(dist, dl, 30);
      if (!Codes(s, lit, dist)) return false;
    } else if (type == 2) {
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = int(s.Bits(5)) + 257, ndist = int(s.Bits(5)) + 1, ncode = int(s.Bits(4)) + 4;
      if (s.bad || nlen > 286 || ndist > 30) return false;
      uint8_t lens[320] = {};
      for (int i = 0; i < ncode; ++i) lens[order[i]] = uint8_t(s.Bits(3));
      Huff lencode;
      Build(lencode, lens, 19);
      uint8_t ll[320] = {};
      int idx = 0;
      while (idx < nlen + ndist) {
        const int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
          ll[idx++] = uint8_t(sym);
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (idx == 0) return false;
            val = ll[idx - 1], rep = 3 + int(s.Bits(2));
          } else if (sym == 17) {
            rep = 3 + int(s.Bits(3));
          } else {
            rep = 11 + int(s.Bits(7));
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) ll[idx++] = uint8_t(val);
        }
        if (s.bad) return false;
      }
      Huff lit, dist;
      Build(lit, ll, nlen);
      Build(dist, ll + nlen, ndist);
      if (!Codes(s, lit, dist)) return false;
    } else {
      return false;
    }
  } while (!last);
  return true;
}

// ------------------------------------------------------------------ import
namespace imp_detail {
inline bool ReadAt(std::ifstream& f, uint64_t off, void* dst, size_t n) {
  f.clear();
  f.seekg(std::streamoff(off), std::ios::beg);
  return bool(f.read(reinterpret_cast<char*>(dst), std::streamsize(n)));
}
inline uint32_t L32(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t L16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// A zip member name is refused when it is absolute, has a drive, a ".." part, or a NUL.
inline bool BadMemberPath(const std::string& n) {
  if (n.empty() || n.find('\0') != std::string::npos) return true;
  if (n[0] == '/' || n[0] == '\\' || (n.size() > 1 && n[1] == ':')) return true;
  size_t i = 0;
  while (i <= n.size()) {
    size_t j = n.find_first_of("/\\", i);
    if (j == std::string::npos) j = n.size();
    if (n.compare(i, j - i, "..") == 0) return true;
    i = j + 1;
  }
  return false;
}

// Writes bytes to <dir>\<name>.pac through a .part file; never overwrites an existing different name silently (an existing same
// name is replaced: re-importing an updated file).
// check(path, warning): "" = usable (a warning may still be set, e.g. an unusual skeleton), else why the file is refused.
using Check = std::function<std::string(const std::string& path_u8, std::string& warning)>;
inline std::string WriteChar(const std::string& stem, const uint8_t* d, size_t n, std::string& name_out, const Check& check, std::string* warn_out) {
  const std::string head = CheckHead(d, n);
  if (!head.empty()) return head;
  const std::string safe = SafeName(stem);
  if (safe.empty()) return "the file name has nothing usable";
  std::error_code ec;
  fs::create_directories(Dir(), ec);
  const fs::path fin = Dir() / fs::u8path(safe + ".pac"), part = Dir() / fs::u8path(safe + ".pac.part");
  {
    std::ofstream o(part, std::ios::binary | std::ios::trunc);
    if (!o || !o.write(reinterpret_cast<const char*>(d), std::streamsize(n))) {
      o.close();
      fs::remove(part, ec);
      return "could not write into the mods folder";
    }
  }
  if (check) {  // (the viewer's own load of the copy, before it takes the real name: a bad file never replaces a good one)
    std::string why, warn;
    try {
      why = check(mods_lookup::U8(part), warn);
    } catch (...) {
      why = "not a usable character pac (damaged)";
    }
    if (!why.empty()) return fs::remove(part, ec), why;
    if (warn_out) *warn_out = warn;
  }
  fs::rename(part, fin, ec);
  if (ec) {
    fs::remove(fin, ec);
    ec.clear();
    fs::rename(part, fin, ec);
    if (ec) return fs::remove(part, ec), "could not finish the copy into the mods folder";
  }
  name_out = safe;
  return "";
}
}  // namespace imp_detail

// Imports a .pac or a .zip holding .pac members. On success `names` lists what was added. Returns "" or the reason it failed.
// `warnings` (optional) gets "<name>: <warning>" for imported files the check had a remark about.
inline constexpr uint64_t kMaxZipTotal = 2ull << 30;  // all .pac members of one zip, unpacked
inline constexpr int kMaxZipPacs = 64;
inline std::string Import(const std::string& path_u8, std::vector<std::string>& names, const imp_detail::Check& check = {},
                          std::vector<std::string>* warnings = nullptr) {
  using namespace imp_detail;
  names.clear();
  if (warnings) warnings->clear();
  auto note = [&](const std::string& nm, const std::string& w) {
    if (warnings && !w.empty()) warnings->push_back(nm + ": " + w);
  };
  if (mods_lookup::Cfg().dir.empty()) return "no mods folder (start the viewer with --mods, or pick one in the launcher)";
  const fs::path src = fs::u8path(path_u8);
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) return "no such file";
  const uint64_t sz = fs::file_size(src, ec);
  std::string ext = mods_lookup::Lower(src.extension().string());
  std::ifstream f(src, std::ios::binary);
  if (!f) return "cannot read the file";
  if (ext != ".pac" && ext != ".zip") {  // (a file without the usual name ending: what its first bytes say)
    uint8_t m4[4] = {};
    if (sz >= 4 && ReadAt(f, 0, m4, 4)) {
      if (m4[0] == 'P' && m4[1] == 'K' && m4[2] == 3 && m4[3] == 4) ext = ".zip";
      else if (std::memcmp(m4, "EPAC", 4) == 0 || std::memcmp(m4, "EPK8", 4) == 0) ext = ".pac";
    }
  }
  if (ext == ".pac") {
    if (sz > kMaxPac) return "not a character pac (too big)";
    std::vector<uint8_t> d;
    d.resize(size_t(sz));
    if (!d.empty() && !ReadAt(f, 0, d.data(), d.size())) return "read failed";
    std::string nm, warn;
    const std::string why = WriteChar(mods_lookup::U8(src.stem()), d.data(), d.size(), nm, check, &warn);
    if (!why.empty()) return why;
    names.push_back(nm);
    note(nm, warn);
    return "";
  }
  if (ext != ".zip") return "pick a .pac file or a .zip that holds one";
  // --- zip: end of central directory
  if (sz < 22) return "not a zip";
  const size_t tail = size_t(std::min<uint64_t>(sz, 22 + 65535));
  std::vector<uint8_t> t;
  t.resize(tail);
  if (!ReadAt(f, sz - tail, t.data(), tail)) return "read failed";
  size_t e = std::string::npos;
  for (size_t i = tail - 22 + 1; i-- > 0;)
    if (L32(&t[i]) == 0x06054b50u) {
      e = i;
      break;
    }
  if (e == std::string::npos) return "not a zip (no end record)";
  const uint32_t cd_size = L32(&t[e + 12]), cd_off = L32(&t[e + 16]);
  const uint16_t total = L16(&t[e + 10]);
  if (cd_size > (64u << 20) || uint64_t(cd_off) + cd_size > sz) return "damaged zip";
  std::vector<uint8_t> cd;
  cd.resize(cd_size);
  if (cd_size && !ReadAt(f, cd_off, cd.data(), cd_size)) return "read failed";
  size_t p = 0;
  int seen = 0, bad = 0;
  uint64_t unpacked = 0;
  bool cut = false;  // a cap stopped the reading: said as a warning when something was imported
  std::vector<std::string> used;  // lower-case names given in this zip: two members with the same file name get " (2)", " (3)", ..
  std::string last_err;
  for (uint16_t k = 0; k < total && p + 46 <= cd.size(); ++k) {
    if (L32(&cd[p]) != 0x02014b50u) break;
    const uint16_t flags = L16(&cd[p + 8]), method = L16(&cd[p + 10]);
    const uint32_t csize = L32(&cd[p + 20]), usize = L32(&cd[p + 24]), lho = L32(&cd[p + 42]);
    const size_t nl = L16(&cd[p + 28]), xl = L16(&cd[p + 30]), cl = L16(&cd[p + 32]);
    if (p + 46 + nl + xl + cl > cd.size()) break;
    const std::string mname(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    p += 46 + nl + xl + cl;
    // only .pac members are looked at at all; anything else in the zip is ignored (never extracted)
    if (mname.size() < 4 || mods_lookup::Lower(mname.substr(mname.size() - 4)) != ".pac") continue;
    if (BadMemberPath(mname)) {
      last_err = "refused an unsafe path in the zip: " + mname;
      ++bad;
      continue;
    }
    if (++seen > kMaxZipPacs) { last_err = "the zip holds more than " + std::to_string(kMaxZipPacs) + " pac files: the rest was not read", cut = true; break; }
    if (flags & 1) { last_err = "the zip is password protected"; continue; }
    if (usize > kMaxPac || csize > kMaxPac) { last_err = "a member of the zip is too big"; continue; }
    if (method != 0 && method != 8) { last_err = "unsupported zip compression"; continue; }
    if (unpacked + usize > kMaxZipTotal) { last_err = "the zip unpacks to more than 2 GB of pac files: the rest was not read", cut = true; break; }
    unpacked += usize;
    uint8_t lh[30];
    if (!ReadAt(f, lho, lh, 30) || L32(lh) != 0x04034b50u) { last_err = "damaged zip"; continue; }
    const uint64_t data_off = uint64_t(lho) + 30 + L16(lh + 26) + L16(lh + 28);
    if (data_off + csize > sz) { last_err = "damaged zip"; continue; }
    std::vector<uint8_t> raw, full;
    raw.resize(csize);
    if (csize && !ReadAt(f, data_off, raw.data(), csize)) { last_err = "read failed"; continue; }
    if (method == 0) full = std::move(raw);
    else if (!Inflate(raw.data(), raw.size(), full, size_t(usize) + 1) ) { last_err = "damaged zip member " + mname; continue; }
    if (full.size() != usize) { last_err = "damaged zip member " + mname; continue; }
    const std::string base = SafeName(mname);  // (only the file name's stem)
    if (base.empty()) { last_err = mname + ": the file name has nothing usable"; continue; }
    std::string stem = base, nm, warn;
    for (int k = 2; std::find(used.begin(), used.end(), mods_lookup::Lower(stem)) != used.end(); ++k) stem = base + " (" + std::to_string(k) + ")";
    const std::string why = WriteChar(stem, full.data(), full.size(), nm, check, &warn);
    if (!why.empty()) { last_err = mname + ": " + why; continue; }
    used.push_back(mods_lookup::Lower(nm));
    names.push_back(nm);
    note(nm, warn);
  }
  if (!names.empty()) {
    if (cut && warnings) warnings->push_back(last_err);
    return "";
  }
  if (!seen && !bad) return "no .pac file in the zip";
  return last_err.empty() ? "no usable character pac in the zip" : last_err;
}

}  // namespace char_mods
}  // namespace assets
