// A guest part's FLOAT parameter rows, read from the game's memory (S111 GUESTHAIR, docs\research\s111\GUESTHAIR.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: no game or SDK includes (the reader's L2 rule). The part's parameter list ([desc+148] count, [desc+152] the array of record pointers) is the jboy mesh record's, relocated: each record is
// name[16], then (kind << 16 | size), the value(s) at +20 (kind 0x0A one float, 0x0D four). The game's run-time relinks (sub_82BD6200 / sub_82BD6378: g_fHdLigLvl, g_f4MuscleParam) repoint a slot
// at another record of the same layout, so the value read through the pointer is the one the draw takes.
// Read in the BYTE pass (guest_model_reader.h ReadModelBytes), not the layout pass: the values are not layout (two of them move every tick), so they never join the layout key or the model key;
// a fresh read (a layout change, an unlock, the forced period) takes them again.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdr_scene::live::guest_model {

inline constexpr uint32_t kParamScalar = 0x0A, kParamVec4 = 0x0D;
inline constexpr uint32_t kMaxFloatParams = 96;   // a part's float rows kept (no real part has more than 68 parameters, S110 WHITEFACE); more are counted in float_overflow

struct FloatParamRead {
  char name[16] = {};   // as the game holds it (the file's own spelling: sub_82BDB460 copies the record as is; S111 UNIFY3), NUL padded; a 16-character name has no NUL
  uint32_t kind = 0;    // kParamScalar / kParamVec4
  float v[4] = {0, 0, 0, 0};
};

// Every kind 0x0A / 0x0D record of the list `parr` (`pc` pointers): plain reads through M (u32 / bytes), never folded into a key.
template <class M>
inline void ReadFloatParams(const M& m, uint32_t parr, uint32_t pc, std::vector<FloatParamRead>& out, uint32_t& overflow) {
  out.clear(), overflow = 0;
  if (!parr) return;
  for (uint32_t i = 0; i < pc; ++i) {
    uint32_t po = 0;
    if (!m.u32(parr + 4 * i, po) || !po) break;   // (the layout pass stops at the same word)
    uint8_t b[36] = {};
    if (!m.bytes(po, 24, b)) continue;
    const uint32_t kind = (uint32_t(b[16]) << 8) | b[17];
    if (kind != kParamScalar && kind != kParamVec4) continue;
    if (kind == kParamVec4 && !m.bytes(po + 24, 12, b + 24)) continue;
    if (out.size() >= kMaxFloatParams) { ++overflow; continue; }
    FloatParamRead f;
    bool end = false;
    for (uint32_t k = 0; k < 16; ++k) {
      if (b[k] == 0) end = true;
      f.name[k] = end ? '\0' : char(b[k]);
    }
    f.kind = kind;
    for (uint32_t c = 0; c < (kind == kParamVec4 ? 4u : 1u); ++c) {
      const uint8_t* w = b + 20 + 4 * c;
      const uint32_t u = (uint32_t(w[0]) << 24) | (uint32_t(w[1]) << 16) | (uint32_t(w[2]) << 8) | w[3];   // the guest's big-endian float
      std::memcpy(&f.v[c], &u, 4);
    }
    out.push_back(f);
  }
}

}  // namespace rdr_scene::live::guest_model
