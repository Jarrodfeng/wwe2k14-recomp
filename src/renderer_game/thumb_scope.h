// S103 lane RBC (coordinator item, probe P2D, docs\research\s103\P2D.md section 2): the replay-thumbnail render scope.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure state, no game or REX includes (the hook that sets it is thumb_scope_hook.h).
//
// sub_8274FF48 is the render method of the replay-thumbnail object (vtable word 0x8202A2A0). It draws the frame shrunk into the 128x128 thumbnail target, resolves it, then REDRAWS the saved
// frame copy over the whole frame target with sub_82BC3098 (vtable slot 39, op-19 site 0x82BC32B8, called at 0x82750340): the small pass overwrote the 360's EDRAM and the game puts the frame back.
// That redraw is a 360 HABIT (an EDRAM restore after an off-screen render): our frame is never touched by a thumbnail render, so dropping the draw is correct and it is not a 2D hole.
// While the scope is open (the render method is running on this thread for a thumbnail object, [r3+24] == 2), a scope-less hole-site op-19 draw is counted `habit_thumb_restore` and nothing else:
// no 2D hole, no unknown_site note in the fallback census. The off-screen T_Sprite row of the downscale draw (site 0x82BC5688, into the 128x128 target) is a Row inside an open 2D scope: its
// pass is kQpOffscreen (quads2d_core.h:458: the target is not the 1280x720 frame) and the host drops it `pass` (scene_host.cpp FateOf :324-330), so it never reaches F.
// quads2d_core.h:63 keeps the site as Leaf2D (it is a hole anywhere else).
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::thumb_scope {

inline thread_local uint32_t t_depth = 0;           // this thread is inside sub_8274FF48 for a thumbnail object
inline std::atomic<uint64_t> g_scopes{0};           // thumbnail renders seen
inline std::atomic<uint64_t> g_habit_thumb_restore{0};   // hole-site draws dropped inside one
inline std::atomic<uint32_t> g_first_site{0};       // the first such site (the log names it)

inline constexpr uint32_t kRestoreSite = 0x82BC32B8u;   // sub_82BC3098 (vtable slot 39), the op-19 call at lr - 4; P2D 2.1 [C listing]
inline bool Active() { return t_depth != 0; }

// The count; returns the new total (the caller logs at the first and at powers of two).
inline uint64_t NoteRestore(uint32_t site) {
  uint32_t zero = 0;
  g_first_site.compare_exchange_strong(zero, site, std::memory_order_relaxed);
  return g_habit_thumb_restore.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct Scope {
  explicit Scope(bool on) : on_(on) {
    if (on_) ++t_depth, g_scopes.fetch_add(1, std::memory_order_relaxed);
  }
  ~Scope() {
    if (on_) --t_depth;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool on_;
};

}  // namespace wwe2k14::thumb_scope
