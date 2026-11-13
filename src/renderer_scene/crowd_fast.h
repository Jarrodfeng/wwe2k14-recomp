// S101 lane 270: allocation-free helpers for the crowd injection (docs\renderer\CROWD_COST.md). Our code only; no game data.
//  - ParseUInt / ParseInt: exactly what sscanf("%u") / sscanf("%d") read from the start of a string (leading white space, an optional sign, decimal digits), without a copy of the string.
//  - FlatU64Map: a reusable open-addressing map (u64 -> u32, "first insert wins" like unordered_map::emplace), cleared in O(1) by a generation stamp. One per call site, reused every frame.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace rdr_scene::fast {

// Reads like sscanf("%u") at `at`: skips white space, takes an optional '+'/'-', then decimal digits; false when no digit follows. A value past 32 bits saturates; a '-' negates modulo 2^32 (strtoul).
inline bool ParseUInt(std::string_view s, size_t at, uint32_t& out) {
  size_t i = at;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i] == '-', ++i;
  if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
  uint64_t v = 0;
  bool over = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    v = v * 10 + uint64_t(s[i] - '0');
    if (v > 0xFFFFFFFFull) over = true, v = 0xFFFFFFFFull;
  }
  out = over ? 0xFFFFFFFFu : neg ? uint32_t(0u - uint32_t(v)) : uint32_t(v);
  return true;
}

// sscanf("%d") likewise (the arena number of "pac\bg\bgNN.pac").
inline bool ParseInt(std::string_view s, size_t at, int& out) {
  size_t i = at;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i] == '-', ++i;
  if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
  int64_t v = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    v = v * 10 + (s[i] - '0');
    if (v > 0x7FFFFFFFll) v = 0x7FFFFFFFll;
  }
  out = int(neg ? -v : v);
  return true;
}

class FlatU64Map {
 public:
  // Empties the map; sized for `expected` entries (it grows by itself when more come).
  void Reset(size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2) cap <<= 1;
    if (cap > slots_.size()) {
      slots_.assign(cap, Slot{});
      gen_ = 1;
    } else if (++gen_ == 0) {
      for (Slot& s : slots_) s.gen = 0;
      gen_ = 1;
    }
    count_ = 0;
  }
  // false when the key is already there (the old value stays: unordered_map::emplace).
  bool Emplace(uint64_t key, uint32_t value) {
    if ((count_ + 1) * 2 > slots_.size()) Grow();
    const size_t mask = slots_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      Slot& s = slots_[i];
      if (s.gen != gen_) {
        s = {key, value, gen_};
        ++count_;
        return true;
      }
      if (s.key == key) return false;
    }
  }
  const uint32_t* Find(uint64_t key) const {
    if (slots_.empty()) return nullptr;
    const size_t mask = slots_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      const Slot& s = slots_[i];
      if (s.gen != gen_) return nullptr;
      if (s.key == key) return &s.value;
    }
  }
  bool Contains(uint64_t key) const { return Find(key) != nullptr; }
  // unordered_map-shaped spellings so call sites read the same: find / end / count / emplace / insert (a set).
  struct Iter {
    bool ok = false;
    uint32_t second = 0;
    const Iter* operator->() const { return this; }
    bool operator==(const Iter& o) const { return ok == o.ok; }
    bool operator!=(const Iter& o) const { return ok != o.ok; }
  };
  Iter find(uint64_t key) const {
    const uint32_t* v = Find(key);
    return v ? Iter{true, *v} : Iter{};
  }
  Iter end() const { return Iter{}; }
  size_t count(uint64_t key) const { return Find(key) ? 1 : 0; }
  void emplace(uint64_t key, uint32_t value) { Emplace(key, value); }
  void insert(uint64_t key) { Emplace(key, 0); }
  size_t size() const { return count_; }

 private:
  struct Slot {
    uint64_t key = 0;
    uint32_t value = 0, gen = 0;
  };
  static size_t Hash(uint64_t k) { return size_t((k * 0x9E3779B97F4A7C15ull) >> 20); }
  void Grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    const uint32_t old_gen = gen_;
    slots_.assign(old.empty() ? 16 : old.size() * 2, Slot{});
    gen_ = 1;
    count_ = 0;
    for (const Slot& s : old)
      if (s.gen == old_gen) Emplace(s.key, s.value);
  }
  std::vector<Slot> slots_;
  uint32_t gen_ = 1;
  size_t count_ = 0;
};

}  // namespace rdr_scene::fast
