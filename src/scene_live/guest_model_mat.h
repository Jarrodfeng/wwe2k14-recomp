// A guest part's material record words and its samplers' texture NAMES, read from the game's memory (S111 EYECUBE, docs\research\s111\EYECUBE.md; UNIFYMAT step 3). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Pure: the standard library and same-folder headers only (the reader's L2 rule). Read in the BYTE pass (guest_model_reader.h ReadModelBytes), plain reads, in no key.
// The names [C sub_82BD9A60]: the model's texture-name table is [m+268] (16 bytes a name; the copy sub_82BDB460 makes of the file's +36 table), or, when [m+268] is 0, the data block's +36 table, the block
// being what the +220 object's vtable +60 returns (sub_827C17B8: [D+12]). Ours takes the data block only when +60 IS that getter (the reader's [m+824] == 0 path does the same); else no names.
// The words: [desc+144] the technique, [desc+140] the cull word (the jboy mesh record's +0x90 / +0x8C): carried on the record (section 72), put on the Material by BuildModel (S111 UNIFY3).
#pragma once
#include <cstdint>
#include <cstring>

#include "skin_data.h"

namespace rdr_scene::live::guest_model {

inline constexpr uint32_t kOffTexNames = 268, kDataTexNames = 36, kTexNameBytes = 16, kDescCull = 140, kDescTechnique = 144;

// The guest address of `model`'s texture-name table (0 = none readable).
template <class M>
inline uint32_t TexNameTable(const M& m, uint32_t model) {
  uint32_t nb = 0;
  if (!model || !m.u32(model + kOffTexNames, nb)) return 0;
  if (nb) return nb;
  uint32_t d = 0, dvt = 0, getter = 0, h = 0;
  if (!m.u32(model + skin::kSkinForwardOff, d) || !d || !m.u32(d, dvt) || !dvt || !m.u32(dvt + skin::kSkinDataBlockGetterOff, getter) || getter != skin::kSkinDataBlockGetterFn) return 0;
  if (!m.u32(d + skin::kSkinDataBlockOff, h) || !h || !m.u32(h + kDataTexNames, nb)) return 0;
  return nb;
}

// Entry t of the table at `names` into `out` (NUL padded; all zero = not read). t was bounded by the model's count [m+256] in the layout pass.
template <class M>
inline void ReadTexName(const M& m, uint32_t names, int32_t t, char (&out)[kTexNameBytes]) {
  std::memset(out, 0, sizeof out);
  if (!names || t < 0) return;
  uint8_t b[kTexNameBytes] = {};
  if (!m.bytes(names + kTexNameBytes * uint32_t(t), kTexNameBytes, b)) return;
  bool end = false;
  for (uint32_t k = 0; k < kTexNameBytes; ++k) {
    if (b[k] == 0) end = true;
    out[k] = end ? '\0' : char(b[k]);
  }
}

// The part record's technique and cull words (0 = not read).
template <class M>
inline void ReadMatWords(const M& m, uint32_t desc, uint32_t& technique, uint32_t& cull) {
  uint32_t a = 0, b = 0;
  technique = cull = 0;
  if (!desc) return;
  if (m.u32(desc + kDescTechnique, a)) technique = a;
  if (m.u32(desc + kDescCull, b)) cull = b;
}

}  // namespace rdr_scene::live::guest_model
