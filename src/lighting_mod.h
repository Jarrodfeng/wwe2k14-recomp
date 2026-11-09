// wwe2k14 - lighting mods (Session 98 step 13, game side; default off: --wwe2k14_mods_lighting=true).
//
// The viewer (or a hand-written file) saves light colours / directions to <mods>\lighting\arena_NN.json and
// entrance_NNN.json; this header applies them in the game. Format and how the viewer writes it: docs\systems\lighting_mod.md.
// Design and evidence: out\runtime\s98\lighting_mod.md (main checkout). Nothing from the game is stored: the JSON holds
// the hash of the player's own key and OUR patch numbers; the patched key is built in memory from the player's own
// game data. Additive (CLAUDE.md "Mods are additive"): flag off = no hook does anything (each returns before touching a
// register); no file matches = the original runs with the original key; files live only in <mods>\lighting\.
//
// ARENA (hook 1) - pre-hook on sub_827F7BA0 = CLightSceneManager::SetStageMotionKey(unsigned char* key, CData* lightpac)
//   Address evidence: the log string at 0x827F7DD4 "void __cdecl CLightSceneManager::SetStageMotionKey(unsigned char *,
//   class CData *)" (tools\q fn 0x827F7BA0); sub_824E8E38 (string "CStage::LightSetupPlatform", loads pac entry 30000) is the
//   caller that matters. Listing (generated\default\wwe2k14_recomp.39.cpp, DEFINE_REX_FUNC(sub_827F7BA0)): r3 = the manager
//   (saved in r24), r4 = the key (`stw r4,0(r11)` with r11 = *(r3+100): the key pointer is stored in the manager's key holder
//   and every later per-frame evaluation reads it from there), r5 = the light pac (saved in r20).
//   r4 = the decoded entry-30000 key (docs: out\runtime\s97\stage_key.md, tools\assets\stagekey.py): "YMKs" + 12 bytes, a 256
//   byte table, N at +0x110, N 16-byte records {4 bytes, u8 track, u8 0, u16 act, u32 offset, u32 size (steps)}, token
//   streams at +0x110 + 16 N + 4 + offset.
//   BYTE ORDER (review fix): sub_824E8E38 calls sub_823D1080 on the key right before this function (wwe2k14_recomp.62.cpp:4330);
//   sub_823D1080 (wwe2k14_recomp.286.cpp:2281) reverses IN PLACE the u32 words at +0 ("YMKs" -> "sKMY"), +4, and from +0x110
//   up to the data base (N, then the words of every record, plus the word just before the data); the token streams are
//   untouched. The hook accepts both orders ("YMKs" = as in the file, "sKMY" = swapped) and works on a host copy with the
//   header words put back to the FILE layout. All hashes are over the file layout, which is what tools\assets\lighting_mod_fnv.py
//   hashes.
//   The size of the key is not stored where the hook can see it, so it is WALKED from the index: the last record's token
//   stream is followed (token < 96 skip, 96 = chunk with a u16 length, else a chunk of token-96 bytes; one step per chunk)
//   until its `size` steps are used. Checked offline on the real keys (arenas 1-6, 277 records each): the walked end equals the
//   entry length (404505 / 525661) and every record's walked end equals the next record's offset. Every page the walk and the
//   copy touch is checked readable first (page table), and the walk is capped at 16 MiB.
//   The hook hashes the copy (FNV-1a 64 of the whole key, and of the token streams alone = `key_fnv_data`), finds a
//   mods\lighting\arena_*.json whose key_fnv (or key_fnv_data) and key_size match (the arena identity, no stage id needed), and
//   overwrites the 25-byte payloads of the patched chunks IN PLACE in the guest buffer at r4. No allocation and no r4 swap
//   (review fix): sub_827F7BA0 stores r4 in the key holder (39.cpp:10213) and the teardown sub_827F8218 frees it with the
//   game's CYMem::Free, so the buffer must stay the game's own; it is the game's fresh copy made by sub_82252758 on both
//   paths, so the cached source is untouched. The game then animates our numbers like its own (animation, cue blending,
//   wiring untouched); chunk sizes and counts never change.
//
// ENTRANCE (hooks 2 and 3), PHASE 1, static per-cue override - UNCONFIRMED parts are marked [U]:
//   Hook 2: post-hook on sub_827F70D0(r3 = manager, r4 = light object, r5 = source object) - copies one light set's blended
//   source into its light object. Listing (wwe2k14_recomp.184.cpp:7641): light object +400 / +416 = two 16-byte blocks from
//   source +4848 / +4928 [U which is sky and which is ground: sky = +400, ground = +416 assumed]; direction vec4 k at
//   object +64+16k and +960+16k (from source +3488+16k); colour k (3 floats) at object +144+16k (from source +5008+80k).
//   Callers: sub_827F8730 (loop over the manager arrays: r4 = (mgr+112)[i], r5 = (mgr+104)[i], count at mgr+124; after the
//   loop it scales sky / ground by the float at mgr+452 unless that is 1.0 - the "brightness" knob) and sub_827F8F48
//   (the blend targets mgr+364 / +368, not in the array: skipped here). Arrays are made by sub_827F7BA0: mgr+104 sources,
//   mgr+112 light objects, mgr+116 set ids (u32, low 16 bits used), mgr+124 = number of distinct sets (stores seen in the
//   listing). The set id of object r4 is therefore (mgr+116)[i] for the i with (mgr+112)[i] == r4.
//   Hook 3: post-hook on sub_8233B418(r3, r4, r5 = S) latches the entrance state: S+4 = the act (25011 in the trace,
//   25000 + roster value in some paths), S+8 = the cue (0, 5, 15 seen). Source: out\runtime\s97\entrance_light.md lines 9-13
//   and the listing (wwe2k14_recomp.113.cpp:1395: `stw r11,4(r31)` / `stw r10,8(r31)` with r31 = r5). It ran in the
//   `entrances` scenario only [coverage.json]. [U] the wrestler is not identified (nothing in S names the bank): the file is
//   chosen by --wwe2k14_lighting_entrance=NNN (default: the lowest entrance_NNN.json). [U] whether sub_8233B418 runs every
//   frame or per cue; the latch is cleared when it sees an act outside 25000..25999 or an arena key is set again.
//   [U] pool slot -> set id: a slot entry applies to the light object whose set id equals its optional "set" field; without
//   "set", slot N means the object at array index N, but only for the documented entrance-rig set ids 450..454
//   (stage_key.md sec. 1) so an arena light is never hit by a wrong index.
//   Brightness: mgr+452 is set to the file's value only while it is 1.0, and put back to 1.0 when the entrance state ends.
//
// Log lines (one per decision, never per frame):
//   [wwe2k14][mods] lighting: arena key fnv=0x... size=N matched arena_05.json (K patches applied, S skipped)
//   [wwe2k14][mods] lighting: arena key fnv=0x... size=N no mod
//   [wwe2k14][mods] lighting: entrance cue C act A file entrance_161.json
//   [wwe2k14][mods] lighting: entrance object set S (index I) cue C slot N applied   (first time per cue/set, max 64)

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest_read.h"
#include "mods.h"

REXCVAR_DEFINE_INT32(wwe2k14_lighting_entrance, -1, "wwe2k14",
                     "Session 98: which <mods>\\lighting\\entrance_NNN.json the entrance lighting uses (NNN = the wrestler's "
                     "bank id). -1 = the lowest NNN present. The game side cannot tell the wrestler yet (unconfirmed). Needs "
                     "--wwe2k14_mods_lighting");

REX_EXTERN(__imp__sub_827F7BA0);
REX_EXTERN(__imp__sub_827F70D0);
REX_EXTERN(__imp__sub_8233B418);

namespace wwe2k14::lighting_mod {

// ---------------------------------------------------------------------------------------------------------------------
// A small JSON reader (objects, arrays, numbers, strings, true / false / null). The files are tiny and written by us.

struct JV {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  bool b = false;
  std::string str;
  std::vector<JV> arr;
  std::vector<std::pair<std::string, JV>> obj;
  const JV* Get(std::string_view k) const {
    if (type != Obj) return nullptr;
    for (const auto& kv : obj)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
  double Number(double def = 0) const { return type == Num ? num : def; }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}
  bool Parse(JV& out) {
    Ws();
    if (!Value(out, 0)) return false;
    Ws();
    return p_ == s_.size();
  }

 private:
  const std::string& s_;
  size_t p_ = 0;
  void Ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
  }
  bool Lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(p_, n, w) != 0) return false;
    p_ += n;
    return true;
  }
  bool String(std::string& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return false;
    ++p_;
    out.clear();
    while (p_ < s_.size() && s_[p_] != '"') {
      char c = s_[p_++];
      if (c == '\\' && p_ < s_.size()) {
        const char e = s_[p_++];
        switch (e) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'u':  // \uXXXX: kept as '?' (names only; the game side never reads them)
            p_ = std::min(s_.size(), p_ + 4);
            c = '?';
            break;
          default: c = e; break;
        }
      }
      out.push_back(c);
    }
    if (p_ >= s_.size()) return false;
    ++p_;
    return true;
  }
  bool Value(JV& v, int depth) {
    if (depth > 32 || p_ >= s_.size()) return false;
    const char c = s_[p_];
    if (c == '{') {
      v.type = JV::Obj;
      ++p_;
      Ws();
      if (p_ < s_.size() && s_[p_] == '}') return ++p_, true;
      for (;;) {
        Ws();
        std::string key;
        if (!String(key)) return false;
        Ws();
        if (p_ >= s_.size() || s_[p_++] != ':') return false;
        Ws();
        JV child;
        if (!Value(child, depth + 1)) return false;
        v.obj.emplace_back(std::move(key), std::move(child));
        Ws();
        if (p_ >= s_.size()) return false;
        if (s_[p_] == ',') {
          ++p_;
          continue;
        }
        if (s_[p_] == '}') return ++p_, true;
        return false;
      }
    }
    if (c == '[') {
      v.type = JV::Arr;
      ++p_;
      Ws();
      if (p_ < s_.size() && s_[p_] == ']') return ++p_, true;
      for (;;) {
        Ws();
        JV child;
        if (!Value(child, depth + 1)) return false;
        v.arr.push_back(std::move(child));
        Ws();
        if (p_ >= s_.size()) return false;
        if (s_[p_] == ',') {
          ++p_;
          continue;
        }
        if (s_[p_] == ']') return ++p_, true;
        return false;
      }
    }
    if (c == '"') {
      v.type = JV::Str;
      return String(v.str);
    }
    if (Lit("true")) return v.type = JV::Bool, v.b = true, true;
    if (Lit("false")) return v.type = JV::Bool, v.b = false, true;
    if (Lit("null")) return v.type = JV::Null, true;
    char* end = nullptr;
    const double d = std::strtod(s_.c_str() + p_, &end);
    if (end == s_.c_str() + p_) return false;
    p_ = size_t(end - s_.c_str());
    v.type = JV::Num;
    v.num = d;
    return true;
  }
};

inline bool ReadJson(const std::filesystem::path& file, JV& out) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  if (text.size() > (4u << 20)) return false;
  JsonParser parser(text);
  return parser.Parse(out);
}

// n numbers from an array value into out[]; false if absent or short.
inline bool Vec(const JV* v, int n, double* out) {
  if (!v || v->type != JV::Arr || int(v->arr.size()) < n) return false;
  for (int i = 0; i < n; ++i) {
    if (v->arr[size_t(i)].type != JV::Num) return false;
    out[i] = v->arr[size_t(i)].num;
  }
  return true;
}

// Files <mods>\lighting\<prefix>*.json, sorted by name.
inline std::vector<std::filesystem::path> ListFiles(const char* prefix) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  const size_t plen = std::strlen(prefix);
  for (const auto& e : std::filesystem::directory_iterator(mods::g_dir / "lighting", ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = e.path().filename().string();
    if (name.size() > plen + 5 && name.compare(0, plen, prefix) == 0 && name.compare(name.size() - 5, 5, ".json") == 0)
      out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// The stage motion key ("YMKs", entry 30000) - the same layout as src\assets\stage_key.h, over a plain byte vector.

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
inline void PutLeF32(uint8_t* p, float f) {
  const uint32_t v = std::bit_cast<uint32_t>(f);
  p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}

inline constexpr uint32_t kKeyIndex = 0x110;        // the record table (entry 30000 starts "YMKs" + 12 bytes)
inline constexpr uint32_t kMaxKeyBytes = 16u << 20;  // sanity cap while walking (real keys are 0.4-0.5 MB)

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24; }

// May the guest word at `guest + off` be read? (the per-frame probes' page-table check, src\guest_read.h)
inline bool GuestReadable(const uint8_t* base, uint32_t guest, uint32_t off) {
  const uint32_t a = (guest + off) & ~3u;
  return a >= 0x10000 && wwe2k14::GuestWordReadable(a, base + a);
}

struct KeyHead {
  bool swapped = false;  // header words reversed by sub_823D1080 (see the file header)
  uint32_t n = 0;        // number of records
  uint32_t data = 0;     // base of the token streams
  uint32_t size = 0;     // walked length of the whole key
};

// The head and walked byte length of the key at guest address `key`; false = not a plausible key. Reads only pages the
// page table says are readable and never past kMaxKeyBytes.
inline bool ReadKeyHead(const uint8_t* base, uint32_t key, KeyHead& h) {
  if (key < 0x10000 || key > 0xFFF00000u) return false;
  if (!GuestReadable(base, key, 0) || !GuestReadable(base, key, kKeyIndex + 12)) return false;
  const uint8_t* d = base + key;
  if (std::memcmp(d, "YMKs", 4) == 0) h.swapped = false;
  else if (std::memcmp(d, "sKMY", 4) == 0) h.swapped = true;
  else return false;
  auto w32 = [&](const uint8_t* p) { return h.swapped ? Be32(p) : Le32(p); };  // header words in the file's meaning
  h.n = w32(d + kKeyIndex);
  if (h.n == 0 || h.n > 4096) return false;
  h.data = kKeyIndex + 16 * h.n + 4;
  if (!GuestReadable(base, key, h.data)) return false;
  const uint8_t* last = d + kKeyIndex + 16 * (h.n - 1);
  const uint32_t off = w32(last + 8), steps = w32(last + 12);
  if (off > kMaxKeyBytes || steps > kMaxKeyBytes) return false;
  uint32_t p = h.data + off, step = 0, checked_page = 0xFFFFFFFFu;
  auto ensure = [&](uint32_t at) {  // every page the walk touches must be readable
    const uint32_t page = (key + at) >> 12;
    if (page == checked_page) return true;
    if (!GuestReadable(base, key, at)) return false;
    checked_page = page;
    return true;
  };
  while (step < steps) {
    if (p + 3 >= kMaxKeyBytes || !ensure(p) || !ensure(p + 3)) return false;
    const uint8_t t = d[p];
    if (t < 96) {
      step += t, ++p;
      continue;
    }
    uint32_t len, head;
    if (t == 96) len = d[p + 1] | d[p + 2] << 8, head = 3;
    else len = uint32_t(t) - 96, head = 1;
    p += head + len;
    ++step;
  }
  if (p <= h.data || p >= kMaxKeyBytes) return false;
  for (uint32_t at = 0; at < p; at += 4096)  // the copy reads all of it: every page must be readable
    if (!GuestReadable(base, key, at)) return false;
  if (!GuestReadable(base, key, p - 1)) return false;
  h.size = p;
  return true;
}

// Puts the header words back to the file layout in a host copy (no-op when the key was not swapped).
inline void NormalizeHeader(std::vector<uint8_t>& v, const KeyHead& h) {
  if (!h.swapped) return;
  auto flip = [&](size_t at) { std::swap(v[at], v[at + 3]), std::swap(v[at + 1], v[at + 2]); };
  flip(0), flip(4);
  for (size_t at = kKeyIndex; at + 4 <= h.data; at += 4) flip(at);  // N, the records, the word before the data
}

inline uint64_t Fnv1a64(const uint8_t* data, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
  return h;
}

struct Rec {
  uint8_t track = 0;
  uint16_t act = 0;
  uint32_t off = 0, end = 0;
};
struct KeyIndex {
  uint32_t data = 0;  // base of the token streams
  std::vector<Rec> rec;
  bool Parse(const std::vector<uint8_t>& b) {
    if (b.size() < kKeyIndex + 20 || std::memcmp(b.data(), "YMKs", 4) != 0) return false;
    const uint32_t n = Le32(b.data() + kKeyIndex);
    if (n == 0 || n > 4096 || kKeyIndex + 16 * size_t(n) + 4 > b.size()) return false;
    data = kKeyIndex + 16 * n + 4;
    rec.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* p = b.data() + kKeyIndex + 16 * i;
      rec[i].track = p[4], rec[i].act = Le16(p + 6), rec[i].off = Le32(p + 8);
    }
    for (uint32_t i = 0; i < n; ++i)
      if (data + size_t(rec[i].off) > b.size()) return false;
    for (uint32_t i = 0; i < n; ++i) rec[i].end = i + 1 < n ? rec[i + 1].off : uint32_t(b.size() - data);
    return true;
  }
  const Rec* Find(int act, int track) const {
    for (const Rec& r : rec)
      if (r.act == act && r.track == track) return &r;
    return nullptr;
  }
};

// Absolute byte offset of the body (count byte first) of the `ordinal`-th chunk of at least 25 body bytes of a record, or
// -1. Same enumeration as the viewer's light_keys (tools\assets\stagekey.py): chunks shorter than 25 bytes are not counted.
inline int64_t FindChunk(const std::vector<uint8_t>& b, const KeyIndex& ix, const Rec& r, int ordinal) {
  size_t p = ix.data + r.off;
  const size_t e = std::min<size_t>(b.size(), ix.data + size_t(r.end));
  int seen = 0;
  while (p < e) {
    const uint8_t t = b[p];
    if (t < 96) {
      ++p;
      continue;
    }
    size_t len, q;
    if (t == 96) {
      if (p + 3 > e) return -1;
      len = b[p + 1] | b[p + 2] << 8, q = p + 3;
    } else {
      len = size_t(t) - 96, q = p + 1;
    }
    if (q + len > b.size()) return -1;
    size_t at = q, l = len;
    if (l && (b[at] & 0x80)) {  // optional prefix: 0x80|n then n more bytes
      const size_t skip = size_t(b[at] & 0x7F) + 1;
      if (skip > l) return -1;
      at += skip, l -= skip;
    }
    if (l >= 25) {
      if (seen == ordinal) return int64_t(at);
      ++seen;
    }
    p = q + len;
  }
  return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Arena mod files.

struct Patch {
  int act = 0, track = 0, chunk = 0;
  bool has_pos = false, has_angle = false, has_scale = false;
  float pos[3] = {};
  int angle[3] = {}, scale[3] = {};  // int16 units: angle 65536 = 360 degrees, scale = colour * 4095
};
struct ArenaMod {
  std::string file, name;
  uint64_t fnv = 0, fnv_data = 0;
  bool has_fnv = false, has_fnv_data = false;  // key_fnv: whole key; key_fnv_data: the token streams alone
  uint32_t key_size = 0;  // 0 = not checked
  std::vector<Patch> patches;
};

inline bool ParseHex64(const std::string& s, uint64_t& out) {
  size_t i = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
  if (i >= s.size()) return false;
  uint64_t v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = v << 4 | uint64_t(d);
  }
  out = v;
  return true;
}

inline int ClampI16(double v) { return int(std::lround(std::clamp(v, -32768.0, 32767.0))); }

inline bool LoadArenaMod(const std::filesystem::path& file, ArenaMod& m) {
  JV j;
  if (!ReadJson(file, j) || j.type != JV::Obj) return false;
  const JV* kind = j.Get("kind");
  if (kind && kind->type == JV::Str && kind->str != "arena") return false;
  const JV* fnv = j.Get("key_fnv");
  m.has_fnv = fnv && fnv->type == JV::Str && ParseHex64(fnv->str, m.fnv);
  const JV* fd = j.Get("key_fnv_data");
  m.has_fnv_data = fd && fd->type == JV::Str && ParseHex64(fd->str, m.fnv_data);
  if (!m.has_fnv && !m.has_fnv_data) return false;
  m.file = file.filename().string();
  if (const JV* n = j.Get("name"); n && n->type == JV::Str) m.name = n->str;
  if (const JV* s = j.Get("key_size"); s && s->type == JV::Num && s->num > 0) m.key_size = uint32_t(s->num);
  const JV* pl = j.Get("patches");
  if (!pl || pl->type != JV::Arr) return false;
  for (const JV& e : pl->arr) {
    if (e.type != JV::Obj) continue;
    const JV *act = e.Get("act"), *track = e.Get("track");
    if (!act || !track || act->type != JV::Num || track->type != JV::Num) continue;
    Patch p;
    p.act = int(act->num), p.track = int(track->num);
    if (const JV* c = e.Get("chunk"); c && c->type == JV::Num) p.chunk = int(c->num);
    double v[3];
    if (Vec(e.Get("pos"), 3, v)) {
      p.has_pos = true;
      for (int i = 0; i < 3; ++i) p.pos[i] = float(v[i]);
    }
    if (Vec(e.Get("angle"), 3, v)) {
      p.has_angle = true;
      for (int i = 0; i < 3; ++i) p.angle[i] = ClampI16(v[i]);
    }
    if (Vec(e.Get("scale"), 3, v)) {
      p.has_scale = true;
      for (int i = 0; i < 3; ++i) p.scale[i] = ClampI16(v[i]);
    }
    m.patches.push_back(p);
  }
  return true;
}

// Overwrites the payloads in `b` (a private copy). Chunk body: count u8, pos 3 x f32 at +1, angle 3 x i16 at +13, scale
// 3 x i16 at +19 (25 bytes; stage_key.h). Returns how many patches were applied; `skipped` counts the others.
inline int ApplyPatches(std::vector<uint8_t>& b, const std::vector<Patch>& patches, int& skipped, std::vector<size_t>& touched) {
  KeyIndex ix;
  skipped = 0;
  if (!ix.Parse(b)) {
    skipped = int(patches.size());
    return 0;
  }
  int applied = 0;
  for (const Patch& p : patches) {
    const Rec* r = ix.Find(p.act, p.track);
    const int64_t at = r && p.chunk >= 0 ? FindChunk(b, ix, *r, p.chunk) : -1;
    if (at < 0 || size_t(at) + 25 > b.size()) {
      ++skipped;
      continue;
    }
    uint8_t* c = b.data() + at;
    if (p.has_pos)
      for (int i = 0; i < 3; ++i) PutLeF32(c + 1 + 4 * i, p.pos[i]);
    if (p.has_angle)
      for (int i = 0; i < 3; ++i) PutLe16(c + 13 + 2 * i, uint16_t(int16_t(p.angle[i])));
    if (p.has_scale)
      for (int i = 0; i < 3; ++i) PutLe16(c + 19 + 2 * i, uint16_t(int16_t(p.scale[i])));
    touched.push_back(size_t(at));
    ++applied;
  }
  return applied;
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrance mod files (phase 1: static per cue / slot).

struct EntSlot {
  int slot = -1, set = -1;  // set = optional explicit light-set id
  bool has_sky = false, has_ground = false;
  float sky[3] = {}, ground[3] = {};
  bool has_dir[5] = {}, has_col[5] = {};
  float dir[5][3] = {}, col[5][3] = {};
};
struct EntCue {
  int cue = 0;
  std::vector<EntSlot> slots;
};
struct EntranceMod {
  std::string file;
  int wrestler = -1;
  bool has_brightness = false;
  float brightness = 1.f;
  std::vector<EntCue> cues;
};

inline bool Vec3f(const JV* v, float out[3]) {
  double d[3];
  if (!Vec(v, 3, d)) return false;
  for (int i = 0; i < 3; ++i) out[i] = float(d[i]);
  return true;
}

inline bool LoadEntranceMod(const std::filesystem::path& file, EntranceMod& m) {
  JV j;
  if (!ReadJson(file, j) || j.type != JV::Obj) return false;
  const JV* kind = j.Get("kind");
  if (kind && kind->type == JV::Str && kind->str != "entrance") return false;
  m.file = file.filename().string();
  if (const JV* w = j.Get("wrestler"); w && w->type == JV::Num) m.wrestler = int(w->num);
  if (const JV* b = j.Get("brightness"); b && b->type == JV::Num) m.has_brightness = true, m.brightness = float(b->num);
  const JV* cues = j.Get("cues");
  if (!cues || cues->type != JV::Arr) return false;
  for (const JV& c : cues->arr) {
    const JV* cn = c.Get("cue");
    const JV* sl = c.Get("slots");
    if (!cn || cn->type != JV::Num || !sl || sl->type != JV::Arr) continue;
    EntCue ec;
    ec.cue = int(cn->num);
    for (const JV& s : sl->arr) {
      const JV* sn = s.Get("slot");
      if (!sn || sn->type != JV::Num) continue;
      EntSlot es;
      es.slot = int(sn->num);
      if (const JV* st = s.Get("set"); st && st->type == JV::Num) es.set = int(st->num);
      es.has_sky = Vec3f(s.Get("sky"), es.sky);
      es.has_ground = Vec3f(s.Get("ground"), es.ground);
      if (const JV* ls = s.Get("lights"); ls && ls->type == JV::Arr) {
        for (size_t k = 0; k < ls->arr.size() && k < 5; ++k) {
          es.has_dir[k] = Vec3f(ls->arr[k].Get("dir"), es.dir[k]);
          es.has_col[k] = Vec3f(ls->arr[k].Get("color"), es.col[k]);
        }
      }
      ec.slots.push_back(es);
    }
    m.cues.push_back(std::move(ec));
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// State.

inline std::mutex g_mutex;

inline std::shared_ptr<const EntranceMod> g_ent;  // the entrance file for the latched state (g_mutex)
inline std::atomic<int> g_cue{-1};                // latched cue, -1 = not in an entrance state
inline std::atomic<int> g_act{0};
inline std::atomic<uint32_t> g_bright_mgr{0};     // manager whose +452 we changed (0 = none)
inline std::atomic<float> g_bright_value{1.f};

// ---------------------------------------------------------------------------------------------------------------------
// Hook 1: the arena key.

inline void RestoreBrightness(uint8_t* base);

// Patches the game's key buffer at `key` in place when a lighting mod matches it. Allocates nothing.
inline void ApplyArenaKey(uint8_t* base, uint32_t key) {
  if (!mods::g_lighting_on) return;
  RestoreBrightness(base);  // a new arena starts a new light manager state: no entrance override carries over
  g_cue.store(-1, std::memory_order_relaxed);
  const auto files = ListFiles("arena_");
  if (files.empty()) {
    REXLOG_INFO("[wwe2k14][mods] lighting: arena key (no arena_*.json in {}) no mod", mods::U8(mods::g_dir / "lighting"));
    return;
  }
  KeyHead head;
  if (!ReadKeyHead(base, key, head)) {
    REXLOG_WARN("[wwe2k14][mods] lighting: arena key at 0x{:08X} is not a readable YMKs key: no mod", key);
    return;
  }
  const uint32_t size = head.size;
  std::vector<uint8_t> copy(base + key, base + key + size);
  NormalizeHeader(copy, head);
  const uint64_t fnv = Fnv1a64(copy.data(), copy.size());
  const uint64_t fnv_data = Fnv1a64(copy.data() + head.data, copy.size() - head.data);
  ArenaMod mod;
  bool found = false;
  for (const auto& f : files) {
    ArenaMod m;
    if (!LoadArenaMod(f, m)) {
      REXLOG_WARN("[wwe2k14][mods] lighting: {} is not a valid arena file, ignored", f.filename().string());
      continue;
    }
    const bool hash_ok = (m.has_fnv && m.fnv == fnv) || (m.has_fnv_data && m.fnv_data == fnv_data);
    if (hash_ok && (m.key_size == 0 || m.key_size == size)) {
      mod = std::move(m), found = true;
      break;
    }
  }
  if (!found) {
    REXLOG_INFO("[wwe2k14][mods] lighting: arena key fnv=0x{:016X} data_fnv=0x{:016X} size={} {} no mod", fnv, fnv_data, size,
                head.swapped ? "(header swapped)" : "(header as in the file)");
    return;
  }
  std::vector<size_t> touched;
  int skipped = 0;
  const int applied = ApplyPatches(copy, mod.patches, skipped, touched);
  for (size_t at : touched)  // only the 25-byte payloads: they lie in the token streams, which are in the file layout
    std::memcpy(base + key + at, copy.data() + at, 25);
  REXLOG_INFO("[wwe2k14][mods] lighting: arena key fnv=0x{:016X} size={} matched {} ({} patches applied, {} skipped)", fnv,
              size, mod.file, applied, skipped);
}

// ---------------------------------------------------------------------------------------------------------------------
// Hooks 2 and 3: the entrance.

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::byteswap(v);
}
inline float LdF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Ld32(base, a)); }
inline void StF(uint8_t* base, uint32_t a, float f) {
  const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f));
  std::memcpy(base + a, &v, 4);
}
inline void StF3(uint8_t* base, uint32_t a, const float v[3]) {
  for (int i = 0; i < 3; ++i) StF(base, a + 4 * uint32_t(i), v[i]);  // the 4th float (w / alpha) is left as the game made it
}

inline void RestoreBrightness(uint8_t* base) {
  const uint32_t mgr = g_bright_mgr.exchange(0);
  if (!mgr) return;
  if (GuestReadable(base, mgr, 452) && LdF(base, mgr + 452) == g_bright_value.load()) StF(base, mgr + 452, 1.0f);
}

inline void LoadEntranceFile(int cue, int act) {
  std::shared_ptr<EntranceMod> m = std::make_shared<EntranceMod>();
  const int want = REXCVAR_GET(wwe2k14_lighting_entrance);
  std::filesystem::path file;
  if (want >= 0) {
    char name[32];
    std::snprintf(name, sizeof(name), "entrance_%03d.json", want);
    file = mods::g_dir / "lighting" / name;
  } else {
    const auto files = ListFiles("entrance_");
    if (!files.empty()) file = files.front();
  }
  std::error_code ec;
  bool ok = !file.empty() && std::filesystem::is_regular_file(file, ec) && LoadEntranceMod(file, *m);
  {
    std::lock_guard lock(g_mutex);
    g_ent = ok ? std::shared_ptr<const EntranceMod>(m) : nullptr;
  }
  if (ok) REXLOG_INFO("[wwe2k14][mods] lighting: entrance cue {} act {} file {}", cue, act, m->file);
}

// Post sub_8233B418: latch the entrance state. S = the state block (r5).
inline void OnEntranceState(uint8_t* base, uint32_t S) {
  if (!mods::g_lighting_on) return;
  if (S < 0x10000 || !GuestReadable(base, S, 0) || !GuestReadable(base, S, 12)) return;
  const uint32_t act = Ld32(base, S + 4), cue = Ld32(base, S + 8);
  if (act < 25000 || act >= 26000 || cue > 100) {
    if (g_cue.exchange(-1) >= 0) RestoreBrightness(base);
    return;
  }
  if (g_cue.load(std::memory_order_relaxed) == int(cue) && g_act.load(std::memory_order_relaxed) == int(act)) return;
  g_act.store(int(act));
  g_cue.store(int(cue));
  LoadEntranceFile(int(cue), int(act));
}

// Post sub_827F70D0: re-apply our values to the light object the original just filled.
inline void OnLightObject(uint8_t* base, uint32_t mgr, uint32_t obj) {
  if (!mods::g_lighting_on) return;
  const int cue = g_cue.load(std::memory_order_relaxed);
  if (cue < 0 || mgr < 0x10000 || obj < 0x10000) return;
  std::shared_ptr<const EntranceMod> m;
  {
    std::lock_guard lock(g_mutex);
    m = g_ent;
  }
  if (!m) return;
  if (!GuestReadable(base, mgr, 452) || !GuestReadable(base, mgr, 124)) return;
  if (m->has_brightness && LdF(base, mgr + 452) == 1.0f) {  // only while the game itself leaves it neutral
    StF(base, mgr + 452, m->brightness);
    g_bright_value.store(m->brightness);
    g_bright_mgr.store(mgr);
  }
  const uint32_t objs = Ld32(base, mgr + 112), ids = Ld32(base, mgr + 116);
  const int n = int(Ld32(base, mgr + 124));
  if (!objs || !ids || n <= 0 || n > 64) return;
  if (!GuestReadable(base, objs, 0) || !GuestReadable(base, ids, 0)) return;
  int idx = -1;
  for (int i = 0; i < n; ++i)
    if (Ld32(base, objs + 4 * uint32_t(i)) == obj) {
      idx = i;
      break;
    }
  if (idx < 0) return;
  const int set = int(Ld32(base, ids + 4 * uint32_t(idx)) & 0xFFFF);
  const EntCue* ec = nullptr;
  for (const EntCue& c : m->cues)
    if (c.cue == cue) ec = &c;
  if (!ec) return;
  const EntSlot* es = nullptr;
  for (const EntSlot& s : ec->slots) {
    if (s.set >= 0 ? s.set == set : (s.slot == idx && set >= 450 && set <= 454)) {  // [U] slot -> set mapping, see header
      es = &s;
      break;
    }
  }
  if (!es) return;
  if (es->has_sky) StF3(base, obj + 400, es->sky);        // [U] +400 sky / +416 ground (header)
  if (es->has_ground) StF3(base, obj + 416, es->ground);
  for (uint32_t k = 0; k < 5; ++k) {
    if (es->has_dir[k]) {
      StF3(base, obj + 64 + 16 * k, es->dir[k]);
      StF3(base, obj + 960 + 16 * k, es->dir[k]);
    }
    if (es->has_col[k]) StF3(base, obj + 144 + 16 * k, es->col[k]);
  }
  static std::atomic<int> logged{0};
  static std::set<uint64_t> seen;
  if (logged.load(std::memory_order_relaxed) < 64) {
    std::lock_guard lock(g_mutex);
    if (seen.insert(uint64_t(cue) << 32 | uint32_t(set)).second) {
      ++logged;
      REXLOG_INFO("[wwe2k14][mods] lighting: entrance object set {} (index {}) cue {} slot {} applied", set, idx, cue, es->slot);
    }
  }
}

}  // namespace wwe2k14::lighting_mod

// SetStageMotionKey (see the header): patch the key in place when a lighting mod matches this arena's key.
REX_HOOK_RAW(sub_827F7BA0) {
  if (wwe2k14::mods::g_lighting_on) wwe2k14::lighting_mod::ApplyArenaKey(base, ctx.r4.u32);
  __imp__sub_827F7BA0(ctx, base);
}

// Light set -> light object copy (see the header): our entrance values over what it just wrote. ctx is not touched after
// the call (the callers keep using the registers the original left).
REX_HOOK_RAW(sub_827F70D0) {
  const uint32_t mgr = ctx.r3.u32, obj = ctx.r4.u32;
  __imp__sub_827F70D0(ctx, base);
  wwe2k14::lighting_mod::OnLightObject(base, mgr, obj);
}

// Entrance state builder (see the header): latch the cue.
REX_HOOK_RAW(sub_8233B418) {
  const uint32_t S = ctx.r5.u32;
  __imp__sub_8233B418(ctx, base);
  wwe2k14::lighting_mod::OnEntranceState(base, S);
}
