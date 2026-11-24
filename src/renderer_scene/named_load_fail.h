// S103 DLW (COV2 GAP 2, part 3): which NAMED wrestler models the loader could not load, so the record stops naming them and the GuestModel rows
// (the game's own finished geometry, guest_model_contrib.h) draw the body instead of a hole. Before DLW WrestlerContributor::RecordCanName answered
// "the record has a key", never "the key loads": a failed named load was a hole for ever and the GuestModel reader was never asked.
// Writers: ModelCache (record_frame.cpp) when a load of a pac\ch\ chNNN.pac asset fails (MissFailed, and the offline in-line loads). Readers: the wrestler
// contributor (RecordCanName and the emitter's link block: a failed link is written as an unnamed asset, pac 0). Cleared by ModelCache::ClearFailed (a new game folder).
// Census (a step check reads it): `[wwe2k14][wrestler_src] key K ...` once per distinct named ref: "loaded from disc|dlc|mod" (live_assets.cpp) or "loaded from guest" (here).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include "rows_inputs.h"   // S106 G4-2

namespace rdr_scene::namedfail {

namespace detail {
struct State {
  std::mutex mu;
  std::set<std::string> failed;       // "pac|entry|variant" (pac lower-cased, '/' as '\\')
  std::atomic<uint64_t> gen{0};       // bumps on every change
  std::atomic<uint64_t> chars[8] = {};   // bit per character id (< 512) with a failed ref: the game thread tests one bit before it builds a key string
  std::atomic<size_t> count{0};       // failed.size(), read without the lock: Failed() answers false at once while nothing is failed (the usual case, on the game thread per skeleton)
  std::set<std::string> holes;        // S103 ARM: failed refs that stay a HOLE (a composite / remapped attire; the guest rows draw garbage on them), marked by the loader before Note
  std::atomic<uint64_t> noted{0}, fell_back{0}, held{0};
};
inline State& S() { static State s; return s; }
inline std::string Key(std::string_view pac, std::string_view entry, uint32_t variant) {
  std::string k;
  for (char c : pac) k += c == '/' ? '\\' : char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  k += '|', k.append(entry), k += '|', k += std::to_string(variant);
  return k;
}
}  // namespace detail

// S110 LIBSIZE (docs\research\s110\LIBSIZE.md items 13-15): a library character's file is `<KEY>\pac\ch\ch497.pac` (KeyLink: every library file is CH497, his KEY folder tells
// them apart; resolved under the one "lib" overlay) = the disc form under ONE folder. True with `id` = the digits after `ch` (any case, '/' or '\\'); the disc form itself is false.
// GameFilesSource::Load opens THAT file (the id alone names no library file).
inline bool LibWrestlerPac(std::string_view pac, int& id) {
  const std::string k = detail::Key(pac, "", 0);
  const size_t cut = k.find('\\');
  if (cut == 0 || cut == std::string::npos || k.compare(cut + 1, 9, "pac\\ch\\ch") != 0) return false;
  size_t i = cut + 10;
  if (i >= k.size() || k[i] < '0' || k[i] > '9') return false;
  int v = 0;
  for (; i < k.size() && k[i] >= '0' && k[i] <= '9' && v < 100000; ++i) v = v * 10 + (k[i] - '0');
  id = v;
  return true;
}
inline bool IsWrestlerPac(std::string_view pac) {   // pac\ch\chNNN.pac, or a library character's <KEY>\pac\ch\ch497.pac (S110 LIBSIZE)
  std::string k = detail::Key(pac, "", 0);
  if (k.rfind("pac\\ch\\ch", 0) == 0) return true;
  const size_t cut = k.find('\\');
  return cut != 0 && cut != std::string::npos && k.compare(cut + 1, 9, "pac\\ch\\ch") == 0;
}
// Called by the loader side when a named wrestler asset failed. Returns true the first time (the caller logs the census line).
inline bool Note(std::string_view pac, std::string_view entry, uint32_t variant) {
  if (!IsWrestlerPac(pac)) return false;
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  const bool fresh = s.failed.insert(detail::Key(pac, entry, variant)).second;
  {  // pac\ch\chNNN.pac: the character id's bit
    std::string_view v = pac;
    const size_t at = v.find_last_of("\\/");
    unsigned ch = 0;
    bool parsed = false;   // "chNNN...": c/C, h/H, then decimal digits (a hand parse: MSVC's CRT marks sscanf deprecated and the unit tests build with -Werror)
    if (at != std::string_view::npos) {
      const std::string_view f = v.substr(at + 1);
      if (f.size() > 2 && (f[0] == 'c' || f[0] == 'C') && (f[1] == 'h' || f[1] == 'H') && f[2] >= '0' && f[2] <= '9') {
        parsed = true;
        for (size_t i = 2; i < f.size() && f[i] >= '0' && f[i] <= '9' && ch < 100000; ++i) ch = ch * 10 + unsigned(f[i] - '0');
      }
    }
    if (parsed && ch < 512) s.chars[ch / 64].fetch_or(1ull << (ch % 64), std::memory_order_release);
    else for (auto& w : s.chars) w.store(~0ull, std::memory_order_release);   // an id it cannot read: every character is "maybe failed" (the full test decides)
  }
  if (fresh) s.gen.fetch_add(1, std::memory_order_release), s.noted.fetch_add(1, std::memory_order_relaxed), s.count.store(s.failed.size(), std::memory_order_release);
  return fresh;
}
// One atomic bit test: false = no ref of this character ever failed (the usual case): skip building the key string.
inline bool MaybeFailedChar(uint32_t ch) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return false;
  return ch >= 512 || (s.chars[ch / 64].load(std::memory_order_acquire) >> (ch % 64) & 1ull) != 0;
}
inline bool Failed(std::string_view pac, std::string_view entry, uint32_t variant) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return false;   // the usual case: nothing is failed (a Clear brings it back here)
  std::lock_guard<std::mutex> l(s.mu);
  return s.failed.count(detail::Key(pac, entry, variant)) != 0;
}
// S103 ARM (HNN fix 4): a composite or remapped attire body whose named load fails is a counted HOLE, not the GuestModel rows (they bind textures the stream-free build never fills:
// the garbage coat of run 042). The loader marks the ref here (true the first time: it says `attire_load_hole N (key)`); the contributor's fallback tests FailedToGuest.
inline bool MarkHole(std::string_view pac, std::string_view entry, uint32_t variant) {
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  const bool fresh = s.holes.insert(detail::Key(pac, entry, variant)).second;
  if (fresh) s.held.fetch_add(1, std::memory_order_relaxed);
  return fresh;
}
inline bool IsHole(std::string_view pac, std::string_view entry, uint32_t variant) {
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  return s.holes.count(detail::Key(pac, entry, variant)) != 0;
}
// A failed ref the GuestModel rows may draw: failed and not a marked hole.
inline bool FailedToGuest(std::string_view pac, std::string_view entry, uint32_t variant) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> l(s.mu);
  const std::string k = detail::Key(pac, entry, variant);
  return s.failed.count(k) != 0 && s.holes.count(k) == 0;
}
// The loader's rule on a failed named wrestler load (GameFilesSource::Load, the production call): a composite attire (the colour table lists it) or a tagged entry (/c: or /r) is a hole.
// True the first time this ref became a hole (the caller says `attire_load_hole N (key)`).
inline bool HoleOnFailure(bool composite_attire, bool tagged_entry, std::string_view pac, std::string_view entry, uint32_t variant) {
  return (composite_attire || tagged_entry) && MarkHole(pac, entry, variant);
}
// The hole rule's switch (wwe2k14_rdr_attire_remap): the loader sets it at each wrestler load; false = the pre-ARM behaviour everywhere (a failed or unusable composite goes to the guest rows).
inline std::atomic<bool>& HoleRule() { static std::atomic<bool> on{true}; return on; }
// A composite attire whose link is unusable this latch (WrestlerContributor::RecordCanName): a hole under the rule, counted.
inline std::atomic<uint64_t>& LinkHoles() { static std::atomic<uint64_t> n{0}; return n; }
inline uint64_t Holes() { return detail::S().held.load(std::memory_order_relaxed); }
inline void Clear() {
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.holes.clear();
  for (auto& w : s.chars) w.store(0, std::memory_order_relaxed);
  if (!s.failed.empty()) s.failed.clear(), s.gen.fetch_add(1, std::memory_order_release), s.count.store(0, std::memory_order_release);
}
inline uint64_t Noted() { return detail::S().noted.load(std::memory_order_relaxed); }
inline uint64_t FellBack() { return detail::S().fell_back.load(std::memory_order_relaxed); }
inline void CountFallback() { detail::S().fell_back.fetch_add(1, std::memory_order_relaxed); }

}  // namespace rdr_scene::namedfail
