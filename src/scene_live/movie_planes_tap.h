// The game side of the movie planes from guest memory (S103 lane P4; src\scene_live\movie_planes.h has the rules). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The existing plane-flush hook of sub_827FB540 (d3d_textures.h) calls OnFlush(base, set) before the original runs: the movie present has finished writing the display buffer by then. Read only: nothing is
// written to the guest, no register changes, no call is skipped, NO NEW HOOK. --wwe2k14_rdr_movie_planes (DEV ONLY, default false, a start-only switch): off = one relaxed load per flush, nothing else
// (no guest read, no copy, no lock). On, every flush of every display set copies its display buffer into the store (about 1.3 MB for a 1280x400 movie with alpha).
// WHICH THREAD runs the flush is [U] (screen_planes_glue.h says the same): the store is mutex-guarded and the render thread never reads guest memory.
#pragma once
#include <rex/cvar.h>

#include <atomic>
#include <cstdint>

#include "../keep_stream.h"    // WWE2K14_STREAM()
#include "../dev_build.h"      // wwe2k14::IsDevBuild (a DEV ONLY switch is honoured by +dev builds only)
#include "../frame_stats.h"   // g_guest_frame
#include "../guest_read.h"    // GuestWordReadableFast
#include "wrestler_hook.h"    // wwe2k14::wrestlers::ReadGuest (the bounds- and fault-checked guest reader)
#include "movie_planes.h"

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_movie_planes, false, "wwe2k14",
                    "S103 P4 (DEV ONLY, not a player setting): copy the movie planes from guest memory at the game's plane flush, for MovieOurs to upload into its own images (so it never reads a stream image). Default off = no change; start-only");

namespace wwe2k14::movie_planes_tap {

inline std::atomic<int> g_state{-1};   // -1 = the cvar not read yet, 0 off, 1 on
inline bool Active() {
  const int s = g_state.load(std::memory_order_relaxed);
  if (s >= 0) return s != 0;
#if !WWE2K14_STREAM()
  const int v = 1;   // S103 PN4: the host's movie rows take their planes only from this store: armed always in a build without the stream (the ON build / player default: the line below, unchanged)
#else
  const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_movie_planes)) ? 1 : 0;   // DEV ONLY: a release build never copies (review F3)
#endif
  g_state.store(v, std::memory_order_relaxed);
  return v != 0;
}
// ReadGuest page-checks off Windows and has the SEH on Windows (wrestler_hook.h).
inline bool ReadBase(uint32_t guest, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(guest, dst, n); }

// r3 = the display set (the flush hook's own argument).
inline void OnFlush(uint32_t set) {
  if (!Active()) return;
  rdr_scene::live::movieplanes::Store().OnFlush(&ReadBase, set, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), &wwe2k14::wrestlers::ReadGuestRows);   // S108 RDS: a plane in one page check
}

}  // namespace wwe2k14::movie_planes_tap
