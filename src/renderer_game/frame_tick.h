// S103 landing 2, piece P6 (docs\renderer\LANDING2_PLAN.md section 4 "Must MOVE out of the reader first", out\s103\decomp\claims_R16.jsonl):
// the scene record's frame boundary, out of the stream reader. Kept in EVERY build (the reader is not, once WWE2K14_KEEP_STREAM is off).
//
// Before P6 the boundary lived inside reader.h OnPresent (EndFrame / TailLatch, the tail's finish, the display table), itself behind ReaderOn() in the
// present hook sub_82CB37C0 (half2_hooks.h): without the reader nothing would latch, end or tick. Now:
//   - the scene boundary calls (rdr_scene::live::EndFrame, LatchFrame + EmitLatched for the deferred tail) are SceneEnd / SceneLatch / SceneEmit here;
//     reader.h calls them at exactly the points it called live:: itself (behaviour with the reader on is unchanged: same calls, same order, same
//     arguments, same split timer);
//   - the tail's finish after the barrier release (tail_release_hook.h) goes through FinishTailAtRelease (= the finish function the latch registered;
//     tail_release.h FlushIfPending), not through a reader function;
//   - the game's display table (sub_82CBABB0, half2_hooks.h) is parsed and kept HERE for every build (the reader takes it from here for its frame);
//   - the present hook calls StreamFreePresent when the reader is off: the scene boundary (EndFrame) and one Tick per present to the record-only poster.
//     The poster (the render thread's record-only message, LANDING2_PLAN section 4 item 2) is P7's: until something registers one, each tick is counted
//     as unposted. The deferred tail (LatchFrame / EmitLatched) is not taken on the stream-free path yet: its finish needs the poster (P7).
//
// The core (everything above the game part) has no game, REX or SDK includes: tools\renderer\frame_tick_unit.cpp builds THIS file with
// WWE2K14_FRAME_TICK_CORE_ONLY and stand-in live:: functions, so what is tested is what ships.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../frame_split.h"                  // ScopedSceneEnd (the split timer the reader put around EndFrame / LatchFrame / EmitLatched)
#include "../renderer_adapter/tail_release.h"  // the tail's window / pending state (header-only, no game includes)
#include "../renderer_scene/live_scene.h"     // EndFrame / LatchFrame / EmitLatched / Enabled / kNoCaptureFrame

namespace wwe2k14::frame_tick {

// ---- the event trace (tests only: null in the game, one relaxed load per event) ---------------------------------------------------------------
enum Ev : uint8_t {
  kEvEnd = 0,      // SceneEnd: the record's boundary now (EndFrame, when the writer is enabled)
  kEvLatch,        // SceneLatch: the latch (LatchFrame, when enabled)
  kEvEmit,         // SceneEmit: a deferred record emitted after the release
  kEvPost,         // a frame message for the render thread: the reader's stream message (NoteStreamPost) or the stream-free Tick
  kEvNoPost,       // a stream-free tick with no poster registered (counted)
  kEvCount
};
inline const char* EvName(uint8_t e) {
  static const char* const k[] = {"end", "latch", "emit", "post", "no_post"};
  return e < kEvCount ? k[e] : "?";
}
using TraceFn = void (*)(Ev ev, uint64_t guest_frame, uint32_t interval);
inline std::atomic<TraceFn> g_trace{nullptr};
inline void Trace(Ev ev, uint64_t guest_frame, uint32_t interval = 0) {
  if (TraceFn f = g_trace.load(std::memory_order_relaxed)) f(ev, guest_frame, interval);
}

// ---- counts (the census can print them; the unit reads them) -------------------------------------------------------------------------------------
struct Counts {
  std::atomic<uint64_t> ends{0}, latches{0}, emits{0}, stream_posts{0}, ticks{0}, ticks_posted{0}, ticks_unposted{0};
};
inline Counts& TheCounts() {
  static Counts* c = new Counts;   // leaked: a late hook never meets a destroyed counter
  return *c;
}

// ---- the scene record's boundary (moved from reader.h OnPresent / TailLatch / TailFinishAt; same calls, same order) ---------------------------
// The boundary now: the record of guest_frame (capture_frame = the capture folder's frame number, or kNoCaptureFrame).
inline void SceneEnd(uint64_t guest_frame, uint32_t capture_frame) {
  TheCounts().ends.fetch_add(1, std::memory_order_relaxed);
  Trace(kEvEnd, guest_frame);
  if (rdr_scene::live::Enabled()) {
    wwe2k14::frame_split::ScopedSceneEnd split_timer;  // S102 H3: one relaxed load when --wwe2k14_frame_split is off
    rdr_scene::live::EndFrame(guest_frame, capture_frame);
  }
}
// The latch (the update thread parked): `out` is reset, then the contributors latch; out.emit_deferred = the record is built at SceneEmit.
inline void SceneLatch(uint64_t guest_frame, rdr_scene::live::Latched& out) {
  TheCounts().latches.fetch_add(1, std::memory_order_relaxed);
  Trace(kEvLatch, guest_frame);
  out = {};
  if (rdr_scene::live::Enabled()) {
    wwe2k14::frame_split::ScopedSceneEnd split_timer;
    rdr_scene::live::LatchFrame(guest_frame, rdr_scene::live::kNoCaptureFrame, /*allow_defer=*/true, out);  // (no capture: kNoCaptureFrame, as EndFrame's argument)
  }
}
// The deferred emit (after the release, the finish mutex held by the caller). A no-op unless the latch deferred it.
inline void SceneEmit(uint64_t guest_frame, rdr_scene::live::Latched& l) {
  if (!l.emit_deferred) return;
  TheCounts().emits.fetch_add(1, std::memory_order_relaxed);
  Trace(kEvEmit, guest_frame);
  wwe2k14::frame_split::ScopedSceneEnd split_timer;
  rdr_scene::live::EmitLatched(l);
}
// The tail release hook (sub_82C3BB38, after the original woke the clients): finish a latched frame. The finish function is the one the latch
// registered (tail::State::SetFinish); it re-checks Pending itself, so this equals calling it directly.
inline void FinishTailAtRelease() { rdr_adapter::tail::The().FlushIfPending(rdr_adapter::tail::kAtRelease); }

// ---- the frame message ----------------------------------------------------------------------------------------------------------------------
// What a stream-free frame tells the render thread (LANDING2_PLAN section 4 item 2): the record is record `guest_frame` (record j, the SnapshotKeeper
// picks it), the present interval, and whether the display table changed since the last tick (DisplayCurve below holds it).
struct Tick {
  uint64_t guest_frame = 0;
  uint32_t present_interval = 0;
  // S103 P7 H1: the picture the present shows, as the present hook read it (the present's r4 = a texture object whose fetch dwords sit at +28, the words the reader's
  // RegisterTextureObject / ShapeOf read). no_picture = r4 was null ("nothing to show": the host repeats its last picture); width / height 0 = not a plain 2D texture (unknown).
  bool no_picture = false;
  uint32_t width = 0, height = 0;
  bool display_curve_changed = false;
  std::array<uint8_t, 1024> display_curve{};   // valid when display_curve_changed
};
// The size a fetch constant describes (reader.h ShapeOf, 2D / stacked only: dim 1; border adds 3 instead of 1). False = not a plain 2D texture.
inline bool SizeOfFetch(const uint32_t w[6], uint32_t& width, uint32_t& height) {
  const uint32_t dim = (w[5] >> 9) & 3;
  if (dim != 1) return false;
  const uint32_t adj = (w[3] >> 31) ? 3u : 1u;
  width = (w[2] & 0x1FFFu) + adj, height = ((w[2] >> 13) & 0x1FFFu) + adj;
  return width && height;
}
// What the present hook tells the boundary about its picture (r4). has_texture = r4 != 0; `fetch` null = the words were not read (size unknown).
struct PresentPicture {
  bool has_texture = true;
  const uint32_t* fetch = nullptr;   // the six fetch dwords at r4 + 28
};
using PostFn = void (*)(const Tick& t);
inline std::atomic<PostFn> g_post{nullptr};   // the record-only poster (P7). Null: the tick is counted unposted.

// The reader posted its stream message for this frame (its own Message, with the stream frame; it stays in the reader): counted and traced only.
inline void NoteStreamPost(uint64_t guest_frame, uint32_t interval) {
  TheCounts().stream_posts.fetch_add(1, std::memory_order_relaxed);
  Trace(kEvPost, guest_frame, interval);
}

// ---- the display table (moved from reader.h OnGammaRamp: parsed and kept here in every build) ------------------------------------------------
// The library's display lookup emitter 0x82CBABB0 (r3 device, r4 three 256-entry u16 tables R / G / B at +0 / +512 / +1024 bytes, each value a 10-bit
// level in bits 6-15). 256 x RGBA8, alpha 255. Guarded by its own mutex (the hook's thread vs the present's).
class DisplayCurve {
 public:
  // `be16(i)` = the big-endian u16 at byte offset i of the tables. Returns true when the table changed (it is then pending).
  template <class Ld16>
  bool Set(Ld16&& be16) {
    std::array<uint8_t, 1024> curve{};
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        const uint32_t level = (uint32_t(be16(512 * c + 2 * i)) >> 6) & 0x3FF;  // 10 bits
        curve[4 * i + c] = uint8_t((level * 255 + 511) / 1023);
      }
      curve[4 * i + 3] = 255;
    }
    std::lock_guard lock(mu_);
    if (known_ && curve == curve_) return false;
    curve_ = curve;
    known_ = pending_ = true;
    return true;
  }
  // The frame takes a changed table once (the reader's live path / the stream-free tick).
  bool TakePending(std::array<uint8_t, 1024>& out) {
    std::lock_guard lock(mu_);
    if (!pending_) return false;
    out = curve_;
    pending_ = false;
    return true;
  }
  bool Pending() {
    std::lock_guard lock(mu_);
    return pending_;
  }
  // A renderer (re)started: a table set before it (the game sets it at boot) is sent again with its first frame.
  void RepostIfKnown() {
    std::lock_guard lock(mu_);
    if (known_) pending_ = true;
  }
  std::array<uint8_t, 1024> Current() {
    std::lock_guard lock(mu_);
    return curve_;
  }

 private:
  std::mutex mu_;
  std::array<uint8_t, 1024> curve_{};
  bool known_ = false, pending_ = false;
};
inline DisplayCurve& TheCurve() {
  static DisplayCurve* c = new DisplayCurve;   // leaked (hooks)
  return *c;
}

// ---- the stream-free present (the reader off: the present hook's other branch) ------------------------------------------------------------------
// One per present: the record's boundary, then one Tick for the render thread. guest_frame = wwe2k14::g_guest_frame as the present reads it (the
// same clock the reader's OnPresent passes to EndFrame).
inline void StreamFreePresentCore(uint64_t guest_frame, uint32_t present_interval, const PresentPicture& pic = PresentPicture{}) {
  SceneEnd(guest_frame, rdr_scene::live::kNoCaptureFrame);
  Counts& n = TheCounts();
  n.ticks.fetch_add(1, std::memory_order_relaxed);
  PostFn post = g_post.load(std::memory_order_acquire);
  if (!post) {
    n.ticks_unposted.fetch_add(1, std::memory_order_relaxed);
    Trace(kEvNoPost, guest_frame, present_interval);
    return;   // the display table stays pending for the first frame a poster takes
  }
  Tick t;
  t.guest_frame = guest_frame;
  t.present_interval = present_interval;
  t.no_picture = !pic.has_texture;
  if (pic.has_texture && pic.fetch) SizeOfFetch(pic.fetch, t.width, t.height);
  t.display_curve_changed = TheCurve().TakePending(t.display_curve);
  n.ticks_posted.fetch_add(1, std::memory_order_relaxed);
  Trace(kEvPost, guest_frame, present_interval);
  post(t);
}

}  // namespace wwe2k14::frame_tick

#if !defined(WWE2K14_FRAME_TICK_CORE_ONLY)
// ---- the game part: guest memory reads (the hooks in half2_hooks.h call these) -------------------------------------------------------------------
#include <rex/logging.h>
#include <rex/system/xmemory.h>   // PhysicalHostOffset
#include "../frame_stats.h"       // wwe2k14::g_guest_frame

namespace wwe2k14::frame_tick {

inline constexpr uint32_t kDevPresentInterval = 14000;   // the device's present interval word (reader.h kDevPresentInterval; static_assert there)

inline uint32_t GuestLd32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 4);
  return __builtin_bswap32(be);
}
inline uint32_t GuestLd16(const uint8_t* base, uint32_t a) {
  uint16_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 2);
  return __builtin_bswap16(be);
}
inline uint32_t PresentInterval(const uint8_t* base, uint32_t device) { return GuestLd32(base, device + kDevPresentInterval); }

// sub_82CBABB0 (every build, before the original). A null table (the present's no-texture path) is not taken (the reader notes it, as before).
inline void OnGammaRamp(uint32_t tables, const uint8_t* base) {
  if (!tables) return;
  DisplayCurve& dc = TheCurve();
  if (!dc.Set([&](uint32_t off) { return GuestLd16(base, tables + off); })) return;
  const std::array<uint8_t, 1024> curve = dc.Current();
  REXLOG_INFO("[rdr] display table from the game: levels 0 / 16 / 32 / 64 / 128 / 192 / 255 -> {} / {} / {} / {} / {} / {} / {} (red)", curve[0], curve[64],
              curve[128], curve[256], curve[512], curve[768], curve[1020]);
}

// sub_82CB37C0 with the reader off (r3 = the device).
// tex = r4, the texture object the game presents (0 = nothing to show); its fetch dwords are at +28.
inline constexpr uint32_t kObjFetch = 28;
inline void StreamFreePresent(const uint8_t* base, uint32_t device, uint32_t tex = 0) {
  PresentPicture pic;
  pic.has_texture = tex != 0;
  uint32_t w[6] = {};
  if (tex) {
    for (uint32_t i = 0; i < 6; ++i) w[i] = GuestLd32(base, tex + kObjFetch + 4 * i);
    pic.fetch = w;
  }
  StreamFreePresentCore(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), PresentInterval(base, device), pic);
}

}  // namespace wwe2k14::frame_tick
#endif
