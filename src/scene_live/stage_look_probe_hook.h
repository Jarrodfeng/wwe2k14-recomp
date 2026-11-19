// DEV PROBE (Session 103, lane L2; docs\renderer\STAGE_LIGHTING.md class B "the live read"): logs the rig objects the entrance's pool tracks live in, so the next step can write the live reader from facts.
// Off by default: --wwe2k14_stage_look_probe=N logs N frames of the entrance (one every 20 guest frames), nothing otherwise. Read only: the original runs first, then a post-hook read. Never compiled into
// a player's default behaviour (one relaxed load of the cvar per call of sub_827F8808, which runs once per frame).
//
// What it reads (all from the listings, STAGE_LIGHTING.md): the manager r3 of sub_827F8808; its second list = count +144, rig objects (+136)[j], light objects (+140)[j]. Each rig object's output rows
// (written by sub_82417D98 per track index t = 0..16): four arrays of 17 rows of 16 bytes at 16 * (216 + t), 16 * (233 + t), 16 * (250 + t), 16 * (267 + t). Tracks t = 2..6 are the five lights (sub_827F8808's
// copy loop); t = 7..16 are [U, the leading hypothesis] the rig's tracks 50..59 (the follow-spot tint and the eight pools' position / tint keys), because 17 = 0, 1, 2..6, 7..16 and entrance_rig.md lists
// slot 2's tracks 0, 1, 10-14, 50-59. The probe prints every row of every rig object so the pool centres (known from the game's page of the same frame) can be matched to a row by value.
// Log line: [wwe2k14][stage_probe] frame F mgr 0x.. pool_n N | obj j 0x.. rig 0x.. | rig j t=T A (x y z w) B (..) C (..) D (..)
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "frame_stats.h"
#include "gfx_menu.h"  // IsDevBuild
#include "guest_read.h"

REXCVAR_DEFINE_INT32(wwe2k14_stage_look_probe, 0, "wwe2k14", "Session 103 lane L2 (dev probe): log N frames of the entrance rig objects' output rows (pool / spot tracks), one every 20 guest frames; 0 = off");

REX_EXTERN(__imp__sub_827F8808);

namespace wwe2k14::scene_live {

inline bool ProbeReadable(const uint8_t* base, uint32_t a, uint32_t len) { return a >= 0x10000 && wwe2k14::GuestWordReadable(a & ~3u, base + (a & ~3u)) && wwe2k14::GuestWordReadable((a + len - 4) & ~3u, base + ((a + len - 4) & ~3u)); }
inline uint32_t ProbeWord(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::byteswap(v);
}
inline float ProbeFloat(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(ProbeWord(base, a)); }

// One line per DISTINCT early-return reason (run 13 printed nothing: this says why, and the "hook alive" line says whether sub_827F8808's hook is reached at all).
inline void ProbeWhy(int code, const char* why, uint64_t frame, uint32_t mgr, uint32_t n, uint32_t rigs, uint32_t objs) {
  static std::atomic<uint32_t> seen{0};
  if (seen.fetch_or(1u << code, std::memory_order_relaxed) & (1u << code)) return;
  REXLOG_INFO("[wwe2k14][stage_probe] early return: {} (frame {} mgr 0x{:08X} count@+144 {} rigs@+136 0x{:08X} objs@+140 0x{:08X})", why, frame, mgr, n, rigs, objs);
}

inline void OnRigProbe(const uint8_t* base, uint32_t mgr, uint64_t frame) {
  static std::atomic<int> logged{0};
  static std::atomic<uint64_t> last_frame{0};
  static std::atomic<bool> alive{false};
  const int want = REXCVAR_GET(wwe2k14_stage_look_probe);
  if (!alive.exchange(true, std::memory_order_relaxed)) REXLOG_INFO("[wwe2k14][stage_probe] hook alive: sub_827F8808 reached at frame {} (mgr 0x{:08X}, probe cvar {})", frame, mgr, want);
  if (want <= 0 || logged.load(std::memory_order_relaxed) >= want) return;
  if (frame < last_frame.load(std::memory_order_relaxed) + 20 && last_frame.load(std::memory_order_relaxed) != 0) return;
  if (!ProbeReadable(base, mgr, 148)) return ProbeWhy(0, "the manager (r3) is not readable to +148", frame, mgr, 0, 0, 0);
  const uint32_t n = ProbeWord(base, mgr + 144), rigs = ProbeWord(base, mgr + 136), objs = ProbeWord(base, mgr + 140);
  if (n == 0) return ProbeWhy(1, "the second list's count (+144) is 0", frame, mgr, n, rigs, objs);
  if (n > 32) return ProbeWhy(2, "the second list's count (+144) is above 32 (not a count: the offset or the object is wrong)", frame, mgr, n, rigs, objs);  // A2 reads up to kLightMaxPool = 32
  if (!ProbeReadable(base, rigs, 4 * n)) return ProbeWhy(3, "the rig list (+136) is not readable", frame, mgr, n, rigs, objs);
  if (!ProbeReadable(base, objs, 4 * n)) return ProbeWhy(4, "the light-object list (+140) is not readable", frame, mgr, n, rigs, objs);
  last_frame.store(frame, std::memory_order_relaxed);
  logged.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("[wwe2k14][stage_probe] frame {} mgr 0x{:08X} pool_n {} rigs 0x{:08X} objs 0x{:08X}", frame, mgr, n, rigs, objs);
  for (uint32_t j = 0; j < n; ++j) {
    const uint32_t rig = ProbeWord(base, rigs + 4 * j), obj = ProbeWord(base, objs + 4 * j);
    REXLOG_INFO("[wwe2k14][stage_probe] frame {} obj {} light 0x{:08X} rig 0x{:08X}", frame, j, obj, rig);
    if (!ProbeReadable(base, rig, 6176)) { REXLOG_INFO("[wwe2k14][stage_probe] frame {} rig {} 0x{:08X}: null or unreadable to +6176 (an unused slot)", frame, j, rig); continue; }  // sub_82417D98's block runs to +6176 (the 80-byte blocks end at 4816 + 80 * 17 = 6176): the whole object head, one check
    // B2 (review): the other things sub_82417D98 writes per track t: an 80-byte block at +4816 + 80 t (sub_827F8808 copies the light colours from +5008 + 80 k = t 2.. there: the leading candidate for the
    // tints), a 16-byte row at 16 * (182 + t), and a 40-byte block at +48 + 40 t. Four floats each (the 80 / 40-byte blocks: their first 16 bytes and the next 16).
    for (uint32_t t = 0; t < 17; ++t) {
      const uint32_t p80 = rig + 4816 + 80 * t, p40 = rig + 48 + 40 * t, p16 = rig + 16 * (182 + t);
      REXLOG_INFO("[wwe2k14][stage_probe] frame {} rig {} t={} E80 ({:.3f} {:.3f} {:.3f} {:.3f}|{:.3f} {:.3f} {:.3f} {:.3f}) E40 ({:.3f} {:.3f} {:.3f} {:.3f}|{:.3f} {:.3f}) E16 ({:.3f} {:.3f} {:.3f} {:.3f})", frame, j, t,
                  ProbeFloat(base, p80), ProbeFloat(base, p80 + 4), ProbeFloat(base, p80 + 8), ProbeFloat(base, p80 + 12), ProbeFloat(base, p80 + 16), ProbeFloat(base, p80 + 20), ProbeFloat(base, p80 + 24), ProbeFloat(base, p80 + 28),
                  ProbeFloat(base, p40), ProbeFloat(base, p40 + 4), ProbeFloat(base, p40 + 8), ProbeFloat(base, p40 + 12), ProbeFloat(base, p40 + 16), ProbeFloat(base, p40 + 20),
                  ProbeFloat(base, p16), ProbeFloat(base, p16 + 4), ProbeFloat(base, p16 + 8), ProbeFloat(base, p16 + 12));
    }
    for (uint32_t t = 0; t < 17; ++t) {
      float v[4][4];
      for (uint32_t a = 0; a < 4; ++a)
        for (uint32_t c = 0; c < 4; ++c) v[a][c] = ProbeFloat(base, rig + 16 * (216 + 17 * a + t) + 4 * c);  // 216 + t, 233 + t, 250 + t, 267 + t
      REXLOG_INFO("[wwe2k14][stage_probe] frame {} rig {} t={} A ({:.3f} {:.3f} {:.3f} {:.3f}) B ({:.3f} {:.3f} {:.3f} {:.3f}) C ({:.3f} {:.3f} {:.3f} {:.3f}) D ({:.3f} {:.3f} {:.3f} {:.3f})", frame, j, t,
                  v[0][0], v[0][1], v[0][2], v[0][3], v[1][0], v[1][1], v[1][2], v[1][3], v[2][0], v[2][1], v[2][2], v[2][3], v[3][0], v[3][1], v[3][2], v[3][3]);
    }
  }
}

}  // namespace wwe2k14::scene_live

// sub_827F8808 = the rig's second pass over the light objects (see the header). Post-hook: the original, then a bounded read (the cvar is 0 by default).
REX_HOOK_RAW(sub_827F8808) {
  const uint32_t mgr = ctx.r3.u32;  // saved before the call (r3 is the return value afterwards)
  __imp__sub_827F8808(ctx, base);
  if (wwe2k14::gfx_menu::IsDevBuild()) wwe2k14::scene_live::OnRigProbe(base, mgr, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));  // dev builds only; the first call prints "hook alive" even with the cvar 0
}
