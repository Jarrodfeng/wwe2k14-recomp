// S103 FXF: notes the 'gGsf' node's state after each run of its per-frame method (src\scene_live\gsf_latch.h; FXP.md adversary C, review fixes 2 / 3). Read-only: the original runs first, then r3 + 16 (index),
// r3 + 20 (t) and the four words it wrote into S+208..+220 are read.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>

#include "scene_live/gsf_latch.h"

REX_EXTERN(__imp__sub_825B7A38);

REX_HOOK_RAW(sub_825B7A38) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_825B7A38(ctx, base);
  const auto word = [base](uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    return uint32_t(_byteswap_ulong(v));
  };
  const uint32_t s = word(0x8392DD88);   // the post settings object S (the method itself writes through it)
  if (!s) return;
  uint32_t node[4];
  for (int i = 0; i < 4; ++i) node[i] = word(s + 208 + 4 * i);
  const uint32_t tb = word(self + 20);
  float t;
  std::memcpy(&t, &tb, 4);
  scene_inputs::gsf::Note(int32_t(word(self + 16)), t, node);   // the component's index and blend: the method itself dereferences this object
}
