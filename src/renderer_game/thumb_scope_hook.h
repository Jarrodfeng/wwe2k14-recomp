// S103 lane RBC: the hook that opens the thumbnail scope (thumb_scope.h). Included once, from d3d_textures.h (both builds: the 2D tap that reads the scope runs in both).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once

#include <rex/hook.h>

#include "../d3d_layer.h"  // Ld32
#include "thumb_scope.h"

REX_EXTERN(__imp__sub_8274FF48);

// 0x8274FF48 the replay-thumbnail object's render method (r3 = the object; [r3+24] == 2 marks a thumbnail object, P2D 2.2). The original always runs; the scope only changes how the 2D tap counts the
// frame-restore draw inside it.
REX_HOOK_RAW(sub_8274FF48) {
  const uint32_t obj = ctx.r3.u32;
  wwe2k14::thumb_scope::Scope scope(obj && wwe2k14::d3d_layer::Ld32(base, obj + 24) == 2);
  __imp__sub_8274FF48(ctx, base);
}
