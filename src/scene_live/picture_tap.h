// The game side of PictureSource (Session 103, landing 2, piece PN3; docs\renderer\PICTURES_NO_STREAM.md sections 5.1-5.2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// --wwe2k14_rdr_picture_source (DEV ONLY, default false, a start-only switch): off = one relaxed load per birth / death / 2D row and nothing else, behaviour byte-identical (the ON build and every
// release build). On (a +dev build only), three things feed the source (src\scene_live\picture_source.h):
//   birth  sub_822951F0 (picture_hooks.h): the key = word 0 of the pair it filled at r3, the caller's call site (lr - 4) names the creator class;
//   death  sub_82BAF210 (picture_hooks.h): the handle registry's slot free (every release path calls it): the key = the word r4 points at, a death only when r3 is the texture registry's manager and the free matched;
//   sight  the op-19 tap (quads2d_tap.h OnOp19, on the game's DRAWING thread): a sprite / glyph row's key at its op-19. The texture is alive and filled at that instant, so the first sight reads its bytes
//          from guest memory (registry -> wrapper -> header -> fetch dwords -> pixel memory) and joins the file index; later sights are hits (no read, no hash).
// Read only: nothing is written to the guest, no register changes, no call is skipped; the originals run exactly as before. The source is process-wide; the host (src\renderer_host) answers a row
// through renderer_scene::PictureResolver (picture_resolver.h), which reads the source from the host thread.
#pragma once
#include <rex/cvar.h>

#include <atomic>
#include <cstdint>

#include "../keep_stream.h"   // WWE2K14_STREAM()
#include "../dev_build.h"     // wwe2k14::IsDevBuild
#include "../frame_stats.h"   // g_guest_frame
#include "wrestler_hook.h"    // wwe2k14::wrestlers::ReadWord / GuestMem / SafeCopy (the bounds- and fault-checked guest reader)
#include "picture_global.h"
#include "../renderer_scene/stream_hash_tap.h"

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_picture_source, false, "wwe2k14",
                    "S103 PN3 DEV ONLY (ignored by a release build; docs\\renderer\\PICTURES_NO_STREAM.md): feed the stream-free picture source from the game's texture handle birth / death and the 2D rows' first sight; read only, off = no change, start-only");

namespace wwe2k14::picture_tap {

namespace ps = rdr_scene::live::picsrc;

inline void StreamHash(uint32_t key, uint64_t hash, uint32_t mips);
inline std::atomic<int> g_state{-1};   // -1 = the cvar not read yet, 0 off, 1 on
inline bool Active() {
  const int s = g_state.load(std::memory_order_relaxed);
  if (s >= 0) return s != 0;
  const bool dev_on = wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_picture_source);   // DEV ONLY: a release STREAM build never feeds the source
#if !WWE2K14_STREAM()
  const int v = 1;   // S103 PN4: in a build without the stream the source is the ONLY picture path (the host's resolver reads it): armed always, no switch. The ON build and the player default are unchanged (this branch does not exist there).
#else
  const int v = dev_on ? 1 : 0;
#endif
  if (dev_on) rdr_scene::g_stream_hash_tap.store(&StreamHash, std::memory_order_relaxed);   // (the stream's hash parity exists only where the stream does)
  g_state.store(v, std::memory_order_relaxed);
  return v != 0;
}

inline ps::PictureSource& Source() { return ps::TheSource(); }   // (picture_global.h: the host's unit reaches the same instance)

inline uint64_t Frame() { return uint64_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); }

// guest_tex's memory contract over the game's memory: `u32` the host value of a big-endian word (a refusal sets `fail`: the picture then reports its own why), `phys` pixel memory by PHYSICAL address
// (the same reader as inputs_glue.h CrowdGuestMem::phys).
struct Mem {
  mutable bool fail = false;
  uint32_t u32(uint32_t a WWE_FEED_LOC_PARAM) const {
    uint32_t v = 0;
    if (!wwe2k14::wrestlers::ReadWord(a, v WWE_FEED_LOC_ARG)) fail = true;
    return v;
  }
};
inline bool Phys(uint32_t addr, uint32_t n, uint8_t* dst) {
  if (!n || n > (8u << 20) || !rdr_scene::live::guest_tex::PhysRangeOk(addr, n)) return false;   // A8: the range check is ours, not the fault handler's
  const auto* host = wwe2k14::wrestlers::GuestMem()->TranslatePhysical<const uint8_t*>(addr);
  return host && wwe2k14::wrestlers::PhysRangeReadable(addr, n) && wwe2k14::wrestlers::SafeCopy(dst, host, n);
}

// Birth: sub_822951F0 returned; `pair` = r3 (the {key, cache} pair it filled), `site` = lr - 4.
inline void OnBorn(uint32_t pair, uint32_t site) {
  if (!Active()) return;
  uint32_t key = 0;
  if (wwe2k14::wrestlers::ReadWord(pair, key)) Source().OnBorn(key, site);
}
// Death: sub_82BAF210 (the handle registry's slot free, A1: the ONE function every release path calls; r3 = the manager, r4 -> the key word) is about to run. Returns the key to hand to OnSlotFreed after the original.
inline uint32_t KeyOfPtr(uint32_t key_ptr) {
  uint32_t key = 0;
  return wwe2k14::wrestlers::ReadWord(key_ptr, key) ? key : 0;
}
// After the original: `manager` = r3 at entry, `freed` = r3 at return (non-zero: the id matched and the slot was released). Only the texture registry's manager ([0x8392DD9C]) counts.
inline void OnSlotFreed(uint32_t manager, uint32_t key, uint32_t freed) {
  if (!Active() || !key || !freed || !manager) return;
  uint32_t registry = 0;
  if (!wwe2k14::wrestlers::ReadWord(rdr_scene::live::particles::kTextureHandleGlobal, registry) || registry != manager) return;
  Source().OnDied(key, Frame());
}
// Sight: a 2D sprite / glyph row's key at its op-19 (the drawing thread). Trimming is the HOST's (A5: PictureResolver::EndFrame -> TrimDrawn with the record it has drawn), never this thread's.
inline void OnRowKey(uint32_t key) {
  if (!Active() || !key) return;
  Source().Seen(key, Frame(), Mem{}, &Phys);
}
// Dev parity: the stream path (UiCore::Decide) names the key's image content hash; the source compares it with its own (guest_parity_ok / bad in the counters).
inline void StreamHash(uint32_t key, uint64_t hash, uint32_t mips) {
  if (Active()) Source().NoteStreamHash(key, hash, mips);
}

}  // namespace wwe2k14::picture_tap
