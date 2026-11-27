// The ring-SIDE object's note: the hook on the apron skirts' draw (S111 RINGLOD; docs\systems\ring.md "S111 RINGLOD"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// sub_82609290 (called by the colour pass's category-22 node sub_826096E0 with r3 = its node - 1168 = the ring-side object Y) decides which skirts are drawn from Y + 1248 + 4 i, the camera quadrant and the
// RIDS byte. Y is a block of its own (sub_824EDB50 allocates it apart from the ring object X), so the ring recompute (ring_recompute_contrib.h Take) takes it from here. Read only: one relaxed store of r3
// (mode 0: one relaxed load), then the original, in every mode. Included once, by draw_origin.h (the hook macro defines the function override).
#pragma once
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "ring_recompute_contrib.h"

REX_EXTERN(__imp__sub_82609290);

REX_HOOK_RAW(sub_82609290) {
  rdr_scene::live::ringrecompute::NoteRingSide(ctx.r3.u32);   // S111 RINGLOD: Y, before the original reuses r3
  __imp__sub_82609290(ctx, base);
}
