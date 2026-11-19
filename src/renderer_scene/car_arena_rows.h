// S103 CAR (BCA G1 piece 1): the record row that carries a created arena's part indices from the latch (scene_live\stage_fx_contrib.h, the game's stage object) to the applier (rf_c_core.cpp EnsureArenaImpl).
// One kFxCar row per frame while a stage exists whose arena id ([stage + 1216]) is 200..249. Pure; the unit tools\scene\car_arena_unit.cpp includes it.
//   slot = the CAEN index ([stage + 21184])   movie = the CASD index ([stage + 21188])   seq = the CARL index (the sub_8342A8D8 pre-hook)
//   w = the real bg arena id ([stage + 21192], 0xFFFFFFFF unread)   h = the arena id ([stage + 1216])   pad = the stage address   flags bit 0 = every field was read
// [C, listing 0x8342A758-0x8342A7FC: the stage keeps the CAEN / CASD indices and byte +3 of the part-info record at +21184 / +21188 / +21192; sub_8342A8D8(loader, light) opens /CARL/%04d (fallback 0000) from its r4 & 255.]
#pragma once
#include <cstdint>
#include <vector>

#include "../assets/car_sel.h"
#include "scene_record.h"

namespace rdr_scene {

inline constexpr uint32_t kCarOffCaen = 21184, kCarOffCasd = 21188, kCarOffBg = 21192;   // stage + ...
inline constexpr uint32_t kFxfCarValid = 1;
inline constexpr uint32_t kCarMaxIndex = 1024;   // a sane part index (the shipped files hold 31 / 8 / 10)

// The guest reads: `rd(addr, dst, n)`; `carl` = the light index the hook saw (-1 = never). false = unread / not sane (no row with the valid flag).
template <class Rd>
inline bool ReadCarSel(Rd&& rd, uint32_t stage, uint32_t arena, int32_t carl, assets::car::Sel& out) {
  out = assets::car::Sel();
  auto word = [&](uint32_t a, uint32_t& v) {
    uint8_t b[4];
    if (!rd(a, b, 4)) return false;
    v = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    return true;
  };
  uint32_t caen = 0, casd = 0, bg = 0;
  if (stage == 0 || carl < 0 || !word(stage + kCarOffCaen, caen) || !word(stage + kCarOffCasd, casd) || !word(stage + kCarOffBg, bg)) return false;
  if (caen >= kCarMaxIndex || casd >= kCarMaxIndex) return false;
  if (bg == 0 && caen == 0 && casd == 0) return false;   // S103 CAR fix 6: all-zero words are a stage whose fields are not written yet (the constructor zeroes them), not a venue: unread
  out.arena = int(arena);
  out.valid = true, out.caen = caen, out.casd = casd, out.carl = uint32_t(carl) & 255u;
  out.bg = bg < 400 ? int(bg) : -1;   // an unsane id is "not read" (arena_lm falls back to 99)
  return true;
}

inline void CarToRows(const assets::car::Sel& s, bool read_ok, uint32_t stage, uint32_t arena, std::vector<StageFxRow>& out) {
  StageFxRow r;
  r.kind = kFxCar, r.slot = uint16_t(s.caen), r.movie = s.casd, r.seq = s.carl, r.w = s.bg < 0 ? 0xFFFFFFFFu : uint32_t(s.bg), r.h = arena, r.pad = stage, r.flags = read_ok && s.valid ? kFxfCarValid : 0u;
  out.push_back(r);
}

// The applier's side: false = no kFxCar row (a stage of an ordinary arena, or none). `out.valid` = the row says every field was read. `arena` = the row's arena id.
inline bool CarFromRows(const std::vector<StageFxRow>& rows, int& arena, assets::car::Sel& out) {
  for (const StageFxRow& r : rows) {
    if (r.kind != kFxCar) continue;
    out = assets::car::Sel();
    arena = int(r.h);
    if (r.flags & kFxfCarValid) out.valid = true, out.arena = int(r.h), out.caen = r.slot, out.casd = r.movie, out.carl = r.seq, out.bg = r.w == 0xFFFFFFFFu ? -1 : int(r.w);
    return true;
  }
  return false;
}

}  // namespace rdr_scene
