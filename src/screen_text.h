// wwe2k14 - screen text from the game's own text lookups (Session 30, #3; test harness only).
//
// The text manager's GetString (sub_822563F8, hooked in src/gfx_menu.h since Session 28) returns every string the
// menus show. Calibrated live (run 2248, every call logged): an open SmartMenu fetches each of its labels again every
// frame (one caller, 0x82B53094, ~8-50 calls a frame); one-shot screens fetch their text once when they open (the
// title's "Press START button", the ATTENTION storage notice: "ATTENTION", "Saving data", ..., "ACCEPT"); the main
// menu's labels carry the W logo glyph as '?' bytes ("??? UNIVERSE"). Not seen there: anything the game draws without
// the text manager (see docs/harness/screen_text.md for what was compared against screenshots).
//
// What "the current screen" is: the distinct strings fetched in the last kWindowFrames frames while the game keeps
// fetching (a menu), or, once it has been quiet for longer, the strings of the last burst (a one-shot screen stays
// current until the next fetch). Script commands (src/input_script.h): log_text, wait_text TEXT, assert_text TEXT;
// the live channel's !status and !text and the F11 bookmark bundle list it. Matching is case-insensitive, by
// substring. On only in harness runs (an input script or the live channel); the strings stay in local logs (game
// text: never committed).

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::screen_text {

constexpr uint64_t kWindowFrames = 8;   // a menu re-fetches every label every frame: 8 frames hold all of them
constexpr uint64_t kBurstGap = 5;       // frames without a fetch that end a burst
constexpr size_t kRing = 8192;          // fetches kept (a busy menu fetches ~50 a frame: ~3 s)

struct Entry {
  uint64_t frame;
  uint32_t id;
  std::string text;
};

struct State {
  std::mutex mutex;
  std::deque<Entry> ring;
  uint64_t burst_start = 0;   // frame of the first fetch of the current / last burst
  uint64_t last_fetch = 0;    // frame of the newest fetch
  uint64_t frame = 0;         // the guest frame now (OnFrame)
  uint64_t fetches = 0;
  std::atomic<bool> on{false};
};

inline State& S() {
  static State s;
  return s;
}

inline void Enable(bool on) { S().on.store(on, std::memory_order_relaxed); }
inline bool Enabled() { return S().on.load(std::memory_order_relaxed); }

// Once per guest frame (the input script's OnFrame).
inline void OnFrame(uint64_t frame) {
  if (!Enabled()) return;
  std::lock_guard lock(S().mutex);
  S().frame = frame;
}

// From the GetString hook: a string the game fetched (already made printable).
inline void Note(uint32_t id, const std::string& text) {
  if (!Enabled() || text.empty()) return;
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (s.fetches == 0 || s.frame > s.last_fetch + kBurstGap) s.burst_start = s.frame;
  s.last_fetch = s.frame;
  ++s.fetches;
  s.ring.push_back({s.frame, id, text});
  if (s.ring.size() > kRing) s.ring.pop_front();
}

// The current screen's strings, in the order they were first fetched (see the header).
inline std::vector<std::string> Current() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<std::string> out;
  if (s.ring.empty()) return out;
  const bool fetching = s.frame <= s.last_fetch + kBurstGap;
  const uint64_t from = fetching ? (s.last_fetch >= kWindowFrames ? s.last_fetch - kWindowFrames : 0) : s.burst_start;
  for (auto it = s.ring.rbegin(); it != s.ring.rend() && it->frame >= from; ++it) {
    if (std::find(out.begin(), out.end(), it->text) == out.end()) out.push_back(it->text);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

inline std::string Lower(std::string t) {
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return t;
}

// The first current string containing `needle` (case-insensitive), or empty.
inline std::string Find(const std::string& needle) {
  const std::string want = Lower(needle);
  for (const std::string& t : Current()) {
    if (Lower(t).find(want) != std::string::npos) return t;
  }
  return {};
}

// The newest `n` distinct strings fetched (newest first), with the frame each was last fetched.
inline std::vector<std::pair<uint64_t, std::string>> Recent(size_t n) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<std::pair<uint64_t, std::string>> out;
  for (auto it = s.ring.rbegin(); it != s.ring.rend() && out.size() < n; ++it) {
    if (std::none_of(out.begin(), out.end(), [&](const auto& p) { return p.second == it->text; })) {
      out.emplace_back(it->frame, it->text);
    }
  }
  return out;
}

// One line: "A | B | C" (at most `max_chars`, cut with "...").
inline std::string Joined(size_t max_chars = 600) {
  std::string line;
  for (const std::string& t : Current()) {
    if (!line.empty()) line += " | ";
    line += t;
    if (line.size() > max_chars) {
      line.resize(max_chars);
      line += "...";
      break;
    }
  }
  return line.empty() ? "(none yet)" : line;
}

}  // namespace wwe2k14::screen_text
