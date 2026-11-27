// S110 SEATREL: only a lookup made INSIDE a real read counts (the thread-local reader set around sub_82C37E48 by model_file_note::ReaderScope, in the
// existing hook of that function in src/load_probe.h: one function has one hook); a menu's probe lookup feeds nothing. NoteBuild (src/fp_probe.h
// wwe2k14_model_link) then gives the reader's note to the descriptor the game built.
// S110 PACANS (docs/research/s110/SEATDRAW.md): a post-hook on sub_82C51D20 (the pac-list lookup: r3 = the pac manager, r4 = the asked path
// `/EMD/<name>` (64 bytes at most), r5 = the out descriptor; on r3 == 1 [r5 + 32] = the list node that matched, its path at node + 0, 48 bytes).
// It tells src/scene_live/pac_answer.h which file the game took for a model name, so our renderer names the same file. Runs for every player; the
// game calls this lookup for every file, so a path that is not `/EMD/` (or, S111 PACANSWER, `/BOBJ/` `/SOBJ/` `/SPMT/`) is dropped by its first bytes (no allocation, no log).
// S111 PACANSWER (docs/research/s111/PACANSWER.md): an entrance prop / recolour mask path's hit goes to assets/pac_path_answer.h Note (any hit, a probe's or a read's: the answer
// is keyed by the path itself), which assets::PropPachEvt / PropMaskPachEvt read before our derived pac-list order. S111 READANSWER: through src/pac_lookup_route.h NotePropHit,
// which also tells Note whether the hit came inside a pac request (a read: model_file_note::CurrentReader), so PropPachEvt takes the section the game READ over its probes'.
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
#include "assets/pac_path_answer.h"   // S111 PACANSWER: the prop / mask paths' answers
#include "pac_lookup_route.h"         // S111 READANSWER: a prop / mask hit -> Note, read or probe

REX_EXTERN(__imp__sub_82C51D20);

namespace wwe2k14::pac_answer_hook {

inline constexpr uint32_t kPathMax = 64, kNodePathBytes = 48, kOutNode = 32;

inline bool Readable(const uint8_t* base, uint32_t a) { return a >= 0x10000 && wwe2k14::GuestWordReadable(a & ~3u, base + (a & ~3u)); }

// Called with the registers the original took (r4 / r5 saved before it ran) once it returned 1.
// Review S110 PACANS: the first-bytes reject reads the path with NO page check. A page check (GuestWordReadable -> BaseHeap::QueryProtect)
// takes the heap mutex (src/load_probe.h), twice per lookup of every file the game opens, before. Unchecked is safe here: on a hit the
// original has just read every byte of the path up to its NUL or 64 bytes (sub_82C51D20 +84..+1C8), and the compares stop at the first
// byte that differs (a NUL differs), so no byte past the ones the game read is touched.
inline void OnLookup(const uint8_t* base, uint32_t path, uint32_t out) {
  if (path < 0x10000) return;
  const uint8_t* p = base + path;
  if (*p == '/') ++p;
  const bool emd = p[0] == 'E' && p[1] == 'M' && p[2] == 'D' && p[3] == '/';
  if (!emd && !::assets::pac_path_answer::IsPropDir(p)) return;   // the rest of the game's files: out here, nothing else done
  const uint32_t reader = emd ? rdr_scene::live::model_file_note::CurrentReader() : 0;
  if (emd && !reader) return;   // S110 SEATREL: a model lookup outside a real read (a menu's existence probe) feeds nothing
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
  const std::string_view asked_v(asked, n), node_v(reinterpret_cast<const char*>(base + node), kNodePathBytes);
  if (emd) rdr_scene::live::model_file_note::NoteRead(reader, asked_v, node_v);
  else {   // S111 PACANSWER / READANSWER: a prop / mask path; inside a pac request it is a read. S111 FOLLOWUPS3: + the reader's class (its vtable word: pac_lookup_route.h kRecolourPropTaskVtable)
    const uint32_t r = rdr_scene::live::model_file_note::CurrentReader();
    bool recoloured = false;
    if (r && Readable(base, r)) {
      uint32_t vbe;
      std::memcpy(&vbe, base + r, 4);
      recoloured = (((vbe & 0xFFu) << 24) | ((vbe & 0xFF00u) << 8) | ((vbe >> 8) & 0xFF00u) | (vbe >> 24)) == ::wwe2k14::pac_lookup_route::kRecolourPropTaskVtable;
    }
    ::wwe2k14::pac_lookup_route::NotePropHit(asked_v, node_v, recoloured);
  }
}

inline void LogLine(const std::string& s) { REXLOG_INFO("{}", s); }
inline const bool g_log_set = (rdr_scene::live::model_file_note::SetLog(&LogLine), true);

}  // namespace wwe2k14::pac_answer_hook

REX_HOOK_RAW(sub_82C51D20) {
  const uint32_t path = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_82C51D20(ctx, base);
  if (ctx.r3.u32 == 1) wwe2k14::pac_answer_hook::OnLookup(base, path, out);
}
