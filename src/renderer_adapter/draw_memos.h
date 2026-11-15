// S102 T14 (R13 swap-thread profile, the per-draw recording cost): three small memos in front of work the reader repeats for every draw. Each is
// behind its own per-frame switch (rdr_switches.h; default off), each answers exactly what the plain path answers (the offline proof,
// tools\draw_memos\draw_memos_test.cpp, replays synthetic draw streams with the switch off and on and compares everything the reader records).
// No game or SDK includes: reader.h, thin_draw.h and the offline test include this file.
//
//   1. NoteMemo   (wwe2k14_rdr_note_memo)    Note() / NoteCounted(): the census add of a name the reader has counted before is two pointer
//                                            increments, not a std::string made from the name and two std::map finds. R13 / profile: DecodeDynamic's
//                                            "viewport_z_inverted" note alone (kit reader.h:3850, now 3922) was 0.42 ms a frame of the swap thread in Bryan's heavy stretch.
//   2. ShaderMemo (wwe2k14_rdr_shader_memo)  GetVertexShader / GetPixelShader: a table of the shaders already resolved (object, declaration, strides,
//                                            microcode address + size) validated by comparing the WHOLE microcode with the copy kept, in place of
//                                            hashing it and finding it in the unordered_map when the one-entry "last shader" memo misses.
//   3. FlatSet64  (wwe2k14_rdr_pin_hash)     The thin-draw pin set's key / triple membership by an open-addressing table instead of a binary search
//                                            over a sorted vector, once a pin set is published.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace rdr_adapter::draw_memos {

// Planted faults for the offline proof (tools\draw_memos\draw_memos_test.cpp, built with -DT14_PLANTS): each one must make a check FAIL. Not compiled into the game.
#ifdef T14_PLANTS
inline int g_plant = 0;
#define T14_PLANT(n) (::rdr_adapter::draw_memos::g_plant == (n))
#else
#define T14_PLANT(n) false
#endif

// ---- 1. the census add ---------------------------------------------------------------------------------------------------------------
inline std::atomic<bool> g_note_on{false};   // set once a frame by the reader (rdr_sw::kNoteMemo)
struct NoteStats {
  uint64_t hits = 0, misses = 0, remembered = 0, declined_long = 0;   // per census window
};

// Caches, per name, the two census cells (window and seen) the name counts into. The cells are nodes of the census' two std::map<std::string, uint64_t>:
// a node stays where it is until it is erased, so a cached pointer is good until the window map is cleared (Invalidate). The cache key is the
// name's POINTER (the notes are string literals) AND its characters: a buffer reused for another text is a miss, never a wrong cell.
class NoteMemo {
 public:
  static constexpr uint32_t kSets = 128, kWays = 2, kText = 48;
  NoteStats stats;
  // True: counted into both cells. False: the caller takes the plain path and then calls Remember.
  bool Count(const char* name, uint64_t n) {
    Slot* set = &slots_[Index(name) * kWays];
    for (uint32_t w = 0; w < kWays; ++w) {
      Slot& s = set[w];
      if (s.ptr == name && s.epoch == epoch_ && (T14_PLANT(1) || std::strcmp(s.text, name) == 0)) {   // plant 1: the pointer alone is trusted
        *s.window += n;
        *s.seen += n;
        ++stats.hits;
        return true;
      }
    }
    ++stats.misses;
    return false;
  }
  // After the plain path counted `name`: remember its two cells (the plain path holds them already: no map find here). A name whose seen cell is still 0 is not
  // remembered (the plain path logs "first seen" while the count is 0).
  void Remember(const char* name, uint64_t* window_cell, uint64_t* seen_cell) {
    const size_t len = std::strlen(name);
    if (len >= kText) {
      ++stats.declined_long;
      return;
    }
    if (*seen_cell == 0 && !T14_PLANT(2)) return;   // plant 2: a never-counted name is cached (its "first seen" line is lost)
    Slot* set = &slots_[Index(name) * kWays];
    Slot* take = nullptr;
    for (uint32_t i = 0; i < kWays; ++i)
      if (set[i].epoch != epoch_ || set[i].ptr == name) {
        take = &set[i];
        break;
      }
    if (!take) take = &set[(flip_++) % kWays];
    take->ptr = name;
    take->epoch = epoch_;
    take->window = window_cell;
    take->seen = seen_cell;
    std::memcpy(take->text, name, len + 1);
    ++stats.remembered;
  }
  void Invalidate() { ++epoch_; }   // the window map was cleared: its nodes are gone
 private:
  struct Slot {
    const char* ptr = nullptr;
    uint64_t epoch = 0;
    uint64_t* window = nullptr;
    uint64_t* seen = nullptr;
    char text[kText] = {};
  };
  static uint32_t Index(const char* p) {
    const uint64_t v = uint64_t(reinterpret_cast<uintptr_t>(p));
    return uint32_t(((v >> 3) ^ (v >> 10) ^ (v >> 17)) % kSets);
  }
  Slot slots_[kSets * kWays];
  uint64_t epoch_ = 1;
  uint32_t flip_ = 0;
};

// The whole NoteCounted body, shared by reader.h and the offline test (F4: what is tested is what ships). `on_first` is called while the seen count is 0.
template <class Map, class Fn>
inline void CountNote(NoteMemo& memo, Map& window, Map& seen, const char* name, uint64_t n, bool on, Fn&& on_first) {
  if (on && memo.Count(name, n)) return;
  uint64_t& w = window[name];
  w += n;
  uint64_t& sn = seen[name];
  if (sn == 0) on_first();
  sn += n;
  if (on) memo.Remember(name, &w, &sn);
}

// ---- 2. the shader table -------------------------------------------------------------------------------------------------------------
inline std::atomic<bool> g_shader_on{false};   // set once a frame by the reader (rdr_sw::kShaderMemo)
// S102 T14b (R20 brief C; switch wwe2k14_rdr_shader_serial, set once a frame by the reader): a table entry whose whole microcode was compared with the source in this
// frame part (the reader's g_shader_repeat_epoch, bumped at every frame-table Reset) answers its next hits with the head compare the one-entry last-shader memo already
// trusts (the first 32 bytes) and no whole-microcode compare. The first hit of an entry in an epoch, and any hit when this is off, compares everything as before.
// There is no shader creation hook to carry a real serial (none is known: docs\renderer\DRAW_INDEX.md), so the epoch is the validity window; see that page for the case it accepts.
inline std::atomic<bool> g_shader_serial_on{false};
#ifdef T14B_PLANTS
inline int g_plant_b = 0;
#define T14B_PLANT(n) (::rdr_adapter::draw_memos::g_plant_b == (n))
#else
#define T14B_PLANT(n) false
#endif
struct ShaderStats {
  uint64_t hits = 0, misses = 0, remembered = 0, declined_big = 0;   // per census window
  uint64_t trusted = 0, verified = 0;   // S102 T14b: hits answered by the epoch rule / by the whole compare
};

// The shader as the reader's plain path identifies it. A hit is returned only when every field AND the whole microcode equal the entry's (the
// plain path hashes the microcode and finds (object, declaration, variant bit, strides, hash) in its map: the same identity, with the hash
// replaced by the bytes). The infos the pointers lead to are the reader's map values: never moved, never erased.
template <class Info>
class ShaderTable {
 public:
  static constexpr uint32_t kSets = 128, kWays = 2, kMaxBytes = 8192;
  struct Key {
    uint32_t object = 0, decl = 0, code_addr = 0, bytes = 0;
    uint8_t seven_c = 0;
    uint8_t strides[24] = {};
  };
  ShaderStats stats;
  // `epoch` = the reader's frame-part number (never 0); 0 = no epoch rule (the plain whole compare at every hit).
  const Info* Find(const Key& k, const uint8_t* source, uint64_t epoch = 0) {
    Entry* set = &entries_[Index(k) * kWays];
    for (uint32_t w = 0; w < kWays; ++w) {
      Entry& e = set[w];
      if (!e.info || !Same(e.key, k)) continue;
      // S102 T14b: verified in this epoch -> the head compare only (<= 32 bytes, the same rule as the one-entry memo)
      if (epoch && (e.verified == epoch || T14B_PLANT(1)) && std::memcmp(e.code.data(), source, T14B_PLANT(2) ? 0 : (k.bytes < 32 ? k.bytes : 32)) == 0) {
        ++stats.hits;
        ++stats.trusted;
        return e.info;
      }
      if (std::memcmp(e.code.data(), source, T14_PLANT(3) && k.bytes > 32 ? 32 : k.bytes) == 0) {   // plant 3: the head only (the one-entry memo's rule)
        e.verified = epoch;
        ++stats.hits;
        ++stats.verified;
        return e.info;
      }
    }
    ++stats.misses;
    return nullptr;
  }
  void Remember(const Key& k, const uint8_t* source, const Info* info, uint64_t epoch = 0) {
    if (!info) return;
    if (k.bytes > kMaxBytes) {
      ++stats.declined_big;
      return;
    }
    Entry* set = &entries_[Index(k) * kWays];
    Entry* take = nullptr;
    for (uint32_t w = 0; w < kWays; ++w)
      if (!set[w].info || Same(set[w].key, k)) {
        take = &set[w];
        break;
      }
    if (!take) take = &set[(flip_++) % kWays];
    take->key = k;
    take->code.assign(source, source + k.bytes);
    take->info = info;
    take->verified = epoch;   // S102 T14b: the copy was just made from the source: verified for this epoch
    ++stats.remembered;
  }
  void Clear() {
    for (Entry& e : entries_) e.info = nullptr;
  }
 private:
  struct Entry {
    Key key;
    std::vector<uint8_t> code;
    const Info* info = nullptr;
    uint64_t verified = 0;   // S102 T14b: the epoch of the last whole-microcode compare (or copy)
  };
  static bool Same(const Key& a, const Key& b) {
    return a.object == b.object && a.decl == b.decl && a.code_addr == b.code_addr && a.bytes == b.bytes && a.seven_c == b.seven_c &&
           std::memcmp(a.strides, b.strides, sizeof(a.strides)) == 0;
  }
  // F1: the declaration, variant bit and strides are part of the index, so an object drawn with several declarations spreads over sets (2 ways would thrash on 3).
  static uint32_t Index(const Key& k) {
    uint64_t h = (uint64_t(k.object) << 32 | k.code_addr) * 0x9E3779B97F4A7C15ull;
    if (!T14_PLANT(4)) {   // plant 4: the index of the first version (object and address only)
      uint64_t sh = 1469598103934665603ull;
      for (uint32_t i = 0; i < 24; ++i) sh = (sh ^ k.strides[i]) * 1099511628211ull;
      h ^= (uint64_t(k.decl) << 8 | k.seven_c) * 0xC2B2AE3D27D4EB4Full ^ sh * 0x165667B19E3779F9ull;
      h *= 0x9E3779B97F4A7C15ull;
    }
    return uint32_t(h >> 40) & (kSets - 1);
  }
 public:
  static Key MakeKey(uint32_t object, uint32_t decl, uint32_t code_addr, uint32_t bytes, bool seven_c, const uint8_t* strides) {   // F4: the one place a key is made (reader.h and the test)
    Key k;
    k.object = object, k.decl = decl, k.code_addr = code_addr, k.bytes = bytes, k.seven_c = seven_c ? 1 : 0;
    if (strides) std::memcpy(k.strides, strides, sizeof(k.strides));
    return k;
  }
 private:
  Entry entries_[kSets * kWays];
  uint32_t flip_ = 0;
};

// ---- 3. the pin set's membership -----------------------------------------------------------------------------------------------------
inline std::atomic<bool> g_pin_hash_on{false};   // set once a frame by the reader (rdr_sw::kPinHash)
struct PinStats {
  uint64_t taken = 0, declined = 0;   // per census window: pin-set queries the reader made with / without a table (the reader thread only)
};
inline PinStats g_pin_stats;
class FlatSet64 {
 public:
  void Build(const std::vector<uint64_t>& values) {
    built_ = true;
    has_zero_ = false;
    uint64_t cap = 16;
    bits_ = 4;
    while (cap < values.size() * 2 + 2) cap <<= 1, ++bits_;
    tab_.assign(cap, 0);
    mask_ = cap - 1;
    size_t at = 0;
    for (uint64_t v : values) {
      if (T14_PLANT(5) && ++at == values.size()) break;   // plant 5: the last value is never inserted
      if (!v) {
        has_zero_ = !T14_PLANT(6);   // plant 6: zero is never a member
        continue;
      }
      uint64_t i = Slot(v);
      while (tab_[i] && tab_[i] != v) i = (i + 1) & mask_;
      tab_[i] = v;
    }
  }
  bool built() const { return built_; }
  uint32_t LongestRun() const {   // the longest probe run a lookup can take (the offline test caps it)
    uint32_t best = 0, run = 0;
    for (uint64_t i = 0; i < 2 * tab_.size(); ++i) {
      if (tab_[i & mask_]) best = std::max(best, ++run);
      else run = 0;
    }
    return best;
  }
  bool Contains(uint64_t v) const {
    if (!built_) return false;   // F6: an unbuilt set has no members (and no table to read)
    if (!v) return has_zero_;
    uint64_t i = Slot(v);
    for (;;) {
      const uint64_t t = tab_[i];
      if (t == v) return true;
      if (!t) return false;
      i = (i + 1) & mask_;
    }
  }
 private:
  // F6: the HIGH bits of the product (the low ones of v * G never see v's high bits: keys that differ only above bit 44 would share one run)
  uint64_t Slot(uint64_t v) const { return T14_PLANT(7) ? ((v * 0x9E3779B97F4A7C15ull) >> 29) & mask_ : (v * 0x9E3779B97F4A7C15ull) >> (64 - bits_); }   // plant 7: the old slot function
  std::vector<uint64_t> tab_;
  uint64_t mask_ = 0;
  uint32_t bits_ = 4;
  bool has_zero_ = false, built_ = false;
};

}  // namespace rdr_adapter::draw_memos
