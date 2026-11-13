// The crowd promotion's speed half, decision logic only (Session 101, lane 50; docs\renderer\CROWD_PROMOTION_MAP.md sections 3-4).
// No game, SDK or renderer includes: the binder (render thread) reports into it, the game-side hooks (crowd_skip.h, game threads) ask it,
// and tools\crowd\crowd_skip_test.cpp runs this very file.
//
//  P2b: do not record the crowd's block / card draws (the op-15 recorder, scene_crowd.h) when ours draws them. REFUSED in the game (crowd_skip.h
//       kP2bEnabled = false): CrowdOurs::Inject takes each member's material and the kept set from the frame's own stream crowd draws, so it needs a
//       record-driven Inject first (docs\systems\crowd.md "P2b is refused"). The decision code stays, with its unit test.
//  P2c: do not run the CPU skin loops (sub_825085F8 / 82508738 / 82508958 / 82508C08) for a group nothing reads.
// Both are allowed only while ours is proven to be drawing the crowd: the binder says, after each bound frame, whether ours drew it and how many
// stream draws it had to keep for members ours could not draw (a kept draw needs the game's record AND the skinned ring). Anything else, or
// silence for kStaleMs (no render thread, a menu, a stalled GPU), and the game's work stays: fallback = the game does everything.
#pragma once

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wwe2k14::crowd_skip {

inline constexpr uint32_t kNeedCleanFrames = 3;  // consecutive frames ours drew with nothing kept before the game's work may be dropped
inline constexpr int64_t kStaleMs = 250;         // a report older than this counts as none
inline constexpr int64_t kGroupReadMs = 2000;    // a skinned-ring group some other draw (sub_8276EAA0) read this recently is still skinned
inline constexpr uint32_t kGroups = 60;          // 3 LODs x 20 person types, group = lod * 20 + type (the ring family at manager word 153 + 7 g)

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The binder's report, one per bound frame. Counters are relaxed atomics: one writer (the render thread), many readers.
struct Gate {
  std::atomic<uint32_t> clean{0};   // consecutive frames ours drew with no stream draw kept
  std::atomic<int64_t> last_ms{0};  // when the last report came (0 = never)
  std::atomic<bool> cards{false};   // ours draws the far cards too (CrowdHook::DrawsCards): only then may their draws be dropped
  std::atomic<uint64_t> reports{0};
  // The census (crowd_skip.h prints it): frames proven, and why the proof was voided.
  std::atomic<uint64_t> proven_frames{0}, void_not_drawn{0}, void_kept{0}, void_stale{0};

  // drew = ours was injected this frame; kept = stream crowd draws kept for members ours could not draw (this frame); cards_ours = DrawsCards().
  void Report(bool drew, uint64_t kept, bool cards_ours, int64_t now_ms) {
    const int64_t before = last_ms.load(std::memory_order_relaxed);
    const bool was_proven = clean.load(std::memory_order_relaxed) >= kNeedCleanFrames;
    if (was_proven && before != 0 && now_ms - before > kStaleMs) void_stale.fetch_add(1, std::memory_order_relaxed);  // silence voided a proof
    if (before != 0 && now_ms - before > kStaleMs) clean.store(0, std::memory_order_relaxed);   // lane 295 SF1 (class audit): a proof does not survive silence; the first report after a gap starts the count again (kNeedCleanFrames frames), it never re-proves at once
    if (drew && kept == 0) {
      const uint32_t c = clean.load(std::memory_order_relaxed);
      if (c < 1000000u) clean.store(c + 1, std::memory_order_relaxed);
      if (c + 1 >= kNeedCleanFrames) proven_frames.fetch_add(1, std::memory_order_relaxed);
    } else {
      if (was_proven) (drew ? void_kept : void_not_drawn).fetch_add(1, std::memory_order_relaxed);
      clean.store(0, std::memory_order_relaxed);
    }
    cards.store(cards_ours, std::memory_order_relaxed);
    last_ms.store(now_ms, std::memory_order_relaxed);
    reports.fetch_add(1, std::memory_order_relaxed);
  }
  bool Proven(int64_t now_ms) const {
    const int64_t t = last_ms.load(std::memory_order_relaxed);
    return t != 0 && now_ms - t <= kStaleMs && clean.load(std::memory_order_relaxed) >= kNeedCleanFrames;
  }
};
inline Gate g_gate;

// P2b, per run of the crowd draw entry (sub_82519508 runs once per tile of a tiled frame). The first block draw of a run is NOT dropped: it is
// the draw the binder injects ours at (and drops itself); without one the frame would have no crowd draw and ours would never be drawn.
struct DrawDecider {
  bool kept_trigger = false;
  void BeginRun() { kept_trigger = false; }
  // block = a block draw (op-15 return 0x82518578); card = a card draw (0x825130C8). True = do not record it.
  bool Drop(bool block, bool card, bool switch_on, bool scene_crowd_run_open, int64_t now_ms) {
    if (!switch_on || scene_crowd_run_open || (!block && !card)) return false;
    if (!g_gate.Proven(now_ms)) return false;
    if (card) return g_gate.cards.load(std::memory_order_relaxed);
    if (!kept_trigger) {
      kept_trigger = true;
      return false;
    }
    return true;
  }
};

inline thread_local DrawDecider t_draws;  // the recorder thread's (the game's draw thread): reset at each run of the crowd draw entry

// P2c: which groups did another reader of the ring (sub_8276EAA0 through the getter sub_82512E80) ask for, and when.
struct GroupReads {
  std::atomic<int64_t> ms[kGroups] = {};
  void NoteRead(uint32_t group, int64_t now_ms) {
    if (group < kGroups) ms[group].store(now_ms, std::memory_order_relaxed);
  }
  bool RecentlyRead(uint32_t group, int64_t now_ms) const {
    if (group >= kGroups) return true;  // unknown: assume read
    const int64_t t = ms[group].load(std::memory_order_relaxed);
    return t != 0 && now_ms - t <= kGroupReadMs;
  }
};
inline GroupReads g_reads;

// Which group a skin loop's destination buffer belongs to. read32(address) reads a guest big-endian word. Mirrors sub_8250BAF0 (listing
// 0x8250BCA4-0x8250BCB4): array = word [manager + 4 * (153 + 7 g + 5 + [manager+548])], model j's buffer = array[j], count j < [manager + 612 + 28 g + 16].
// Returns kGroups when the buffer is in no group (the loop then runs).
template <class Read32>
inline uint32_t GroupOfBuffer(Read32&& read32, uint32_t manager, uint32_t buffer) {
  const uint32_t write_ring = read32(manager + 548);
  if (write_ring > 1 || !buffer) return kGroups;
  for (uint32_t g = 0; g < kGroups; ++g) {
    const uint32_t array = read32(manager + 4 * (153 + 7 * g + 5 + write_ring));
    const uint32_t count = read32(manager + 612 + 28 * g + 16);
    if (!array || count == 0 || count > 64) continue;
    for (uint32_t j = 0; j < count; ++j)
      if (read32(array + 4 * j) == buffer) return g;
  }
  return kGroups;
}

// The buffer -> group map, built once per manager update (the hook on sub_82509CB8, which runs once per sub_8250BAF0 call before the loops, calls
// InvalidateMap) instead of a scan of 60 groups for every one of the loop calls.
struct BufferMap {
  bool valid = false;
  uint32_t manager = 0;
  std::vector<std::pair<uint32_t, uint32_t>> by_buffer;  // sorted (buffer, group)
};
inline thread_local BufferMap t_map;
inline void InvalidateMap() { t_map.valid = false; }

template <class Read32>
inline void BuildMap(Read32&& read32, uint32_t manager, BufferMap& out) {
  out.valid = true, out.manager = manager;
  out.by_buffer.clear();
  const uint32_t write_ring = read32(manager + 548);
  if (write_ring > 1) return;
  for (uint32_t g = 0; g < kGroups; ++g) {
    const uint32_t array = read32(manager + 4 * (153 + 7 * g + 5 + write_ring));
    const uint32_t count = read32(manager + 612 + 28 * g + 16);
    if (!array || count == 0 || count > 64) continue;
    for (uint32_t j = 0; j < count; ++j) {
      const uint32_t buffer = read32(array + 4 * j);
      if (buffer) out.by_buffer.emplace_back(buffer, g);
    }
  }
  std::sort(out.by_buffer.begin(), out.by_buffer.end());
}
inline uint32_t MapLookup(const BufferMap& m, uint32_t buffer) {
  const auto it = std::lower_bound(m.by_buffer.begin(), m.by_buffer.end(), std::make_pair(buffer, 0u));
  return it != m.by_buffer.end() && it->first == buffer ? it->second : kGroups;
}

// The skin-loop counters of the census, and the last skip (crowd_ours.cpp skips its pose comparison while skins are being skipped).
inline std::atomic<uint64_t> g_loops_total{0}, g_loops_skipped{0}, g_loops_unknown_group{0};
inline std::atomic<int64_t> g_last_skip_ms{0};
inline bool SkippingNow(int64_t now_ms) {
  const int64_t t = g_last_skip_ms.load(std::memory_order_relaxed);
  return t != 0 && now_ms - t <= kStaleMs;
}

// P2c: may this skin loop call be skipped? (switch on, ours proven, the group known and not read by anyone else). Counts the loop in the census.
template <class Read32>
inline bool SkipSkin(Read32&& read32, uint32_t manager, uint32_t buffer, bool switch_on, int64_t now_ms) {
  if (!switch_on) return false;
  g_loops_total.fetch_add(1, std::memory_order_relaxed);
  if (!g_gate.Proven(now_ms)) return false;
  if (!t_map.valid || t_map.manager != manager) BuildMap(read32, manager, t_map);
  const uint32_t g = MapLookup(t_map, buffer);
  if (g >= kGroups) {
    g_loops_unknown_group.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (g_reads.RecentlyRead(g, now_ms)) return false;
  g_loops_skipped.fetch_add(1, std::memory_order_relaxed);
  g_last_skip_ms.store(now_ms, std::memory_order_relaxed);
  return true;
}

}  // namespace wwe2k14::crowd_skip
