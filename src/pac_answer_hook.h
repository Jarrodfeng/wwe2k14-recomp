// S110 SEATREL: only a lookup made INSIDE a real read counts (the thread-local reader set around sub_82C37E48 by model_file_note::ReaderScope, in the
// existing hook of that function in src/load_probe.h: one function has one hook); a menu's probe lookup feeds nothing. NoteBuild (src/fp_probe.h
// wwe2k14_model_link) then gives the reader's note to the descriptor the game built.
// S110 PACANS (docs/research/s110/SEATDRAW.md): a post-hook on sub_82C51D20 (the pac-list lookup: r3 = the pac manager, r4 = the asked path
// `/EMD/<name>` (64 bytes at most), r5 = the out descriptor; on r3 == 1 [r5 + 32] = the list node that matched, its path at node + 0, 48 bytes).
// It tells src/scene_live/pac_answer.h which file the game took for a model name, so our renderer names the same file. Runs for every player; the
// game calls this lookup for every file, so a path that is not `/EMD/` is dropped by its first bytes (no allocation, no log).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "guest_read.h"
#include "scene_live/model_file_note.h"

REX_EXTERN(__imp__sub_82C51D20);

namespace wwe2k14::pac_answer_hook {

inline constexpr uint32_t kPathMax = 64, kNodePathBytes = 48, kOutNode = 32;

inline bool Readable(const uint8_t* base, uint32_t a) { return a >= 0x10000 && wwe2k14::GuestWordReadable(a & ~3u, base + (a & ~3u)); }

// Called with the registers the original took (r4 / r5 saved before it ran) once it returned 1.
// Review S110 PACANS: the first-bytes reject reads the path with NO page check. A page check (GuestWordReadable -> BaseHeap::QueryProtect)
// takes the heap mutex (src/load_probe.h:508), twice per lookup of every file the game opens, before. Unchecked is safe here: on a hit the
// original has just read every byte of the path up to its NUL or 64 bytes (sub_82C51D20 +84..+1C8), and the compares stop at the first
// byte that differs (a NUL differs), so no byte past the ones the game read is touched.
inline void OnLookup(const uint8_t* base, uint32_t path, uint32_t out) {
  if (path < 0x10000) return;
  const uint8_t* p = base + path;
  if (*p == '/') ++p;
  if (p[0] != 'E' || p[1] != 'M' || p[2] != 'D' || p[3] != '/') return;   // the rest of the game's files: out here, nothing else done
  const uint32_t reader = rdr_scene::live::model_file_note::CurrentReader();
  if (!reader) return;   // S110 SEATREL: a lookup outside a real read (a menu's existence probe) feeds nothing
  if (!Readable(base, out + kOutNode)) return;
  uint32_t nbe;
  std::memcpy(&nbe, base + out + kOutNode, 4);
  const uint32_t node = ((nbe & 0xFFu) << 24) | ((nbe & 0xFF00u) << 8) | ((nbe >> 8) & 0xFF00u) | (nbe >> 24);
  if (!Readable(base, node) || !Readable(base, node + kNodePathBytes - 4)) return;
  char asked[kPathMax + 1];
  size_t n = 0;
  for (; n < kPathMax; ++n) {   // bounded, as the game's own walk is (the same bytes it read: no page check, above)
    const char c = char(base[path + uint32_t(n)]);
    if (!c) break;
    asked[n] = c;
  }
  asked[n] = 0;
  rdr_scene::live::model_file_note::NoteRead(reader, std::string_view(asked, n),
                                              std::string_view(reinterpret_cast<const char*>(base + node), kNodePathBytes));
}

inline void LogLine(const std::string& s) { REXLOG_INFO("{}", s); }
inline const bool g_log_set = (rdr_scene::live::model_file_note::SetLog(&LogLine), true);

}  // namespace wwe2k14::pac_answer_hook

REX_HOOK_RAW(sub_82C51D20) {
  const uint32_t path = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_82C51D20(ctx, base);
  if (ctx.r3.u32 == 1) wwe2k14::pac_answer_hook::OnLookup(base, path, out);
}
